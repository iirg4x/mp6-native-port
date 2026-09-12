/* MP6 native port -- hardware-faithful float->integer conversion.
 *
 * THE DEFECT THIS EXISTS FOR
 * --------------------------
 * Recovered game code converts unbounded floats to integers and clamps
 * AFTERWARDS. The board's per-player audio pan is the canonical shape
 * (src/board/audio.c, mbAudFXPosPanGet):
 *
 *     Hu3D3Dto2D(pos, HU3D_CAM0, &pos2D);
 *     pan = (int)((pos2D.x * (1.0f / HU_DISP_WIDTH)) * MSM_PAN_WIDTH) + MSM_PAN_LEFT;
 *     if (pan < MSM_PAN_LEFT)  pan = MSM_PAN_LEFT;
 *     else if (pan > MSM_PAN_RIGHT) pan = MSM_PAN_RIGHT;
 *
 * That is correct code ON GEKKO and undefined behaviour on the host.
 * Hu3D3Dto2D (src/game/hsfex.c) divides by `tan(fov/2) * camera-space z`,
 * so a subject on the camera plane makes the divisor zero and the projected
 * x comes back +-inf or NaN. The clamp two lines later never runs: the trap
 * is in the CONVERSION, and zig cc leaves clang's float-cast-overflow check
 * on, which aborts the process --
 *
 *     panic: (float) is outside the range of representable values of type 'int'
 *
 * -- with no [MP6-CRASH], no OSPanic and no "runtime error:" line, because it
 * is zig's own illegal-behaviour handler. See
 * docs/history/BOARD_PAN_FLOAT_TRAP.md.
 *
 * WHAT THE HARDWARE DID
 * ---------------------
 * MWCC compiles a float->int cast on Gekko to `fctiwz` (convert to integer
 * word, round toward zero) followed by a store/load of the low word, so the
 * value C sees is exactly fctiwz's result, and fctiwz SATURATES:
 *
 *     operand > 2^31-1  ->  0x7FFFFFFF
 *     operand < -2^31   ->  0x80000000
 *     operand is NaN    ->  0x80000000
 *     otherwise         ->  truncate toward zero
 *
 * So retail clamped a saturated integer and carried on. `mp6_fctiwz_s32`
 * reproduces those four rules exactly. It cannot change any in-range
 * conversion -- the two guards only engage where a plain cast would have
 * been undefined -- which is what makes it safe to route a recovered
 * expression through it without changing behaviour anywhere it was already
 * defined.
 *
 * WHY NOT JUST TURN THE SANITIZER OFF FOR THE FILE
 * ------------------------------------------------
 * tools/build.py does that for src/board/math.c, and there it is right: the
 * result is immediately MASKED into a 2048-entry table, so every bit pattern
 * is in range by construction and the raw host conversion is as good as the
 * hardware's. Here the result is CLAMPED, not masked, and the raw host
 * conversion is not equivalent:
 *
 *   - x86-64 `cvttss2si` returns the "integer indefinite" value 0x80000000
 *     for +inf, -inf AND NaN. So +inf would clamp to MSM_PAN_LEFT -- full
 *     LEFT where the console panned full RIGHT.
 *   - aarch64 `fcvtzs` saturates like fctiwz on the infinities but returns 0
 *     for NaN, where fctiwz returns 0x80000000. So the Windows and Android
 *     rows would disagree with each other AND with the console.
 *
 * One shared helper makes all three agree, keeps float-cast-overflow ON for
 * the rest of those translation units, and states the rule once.
 *
 * NARROWER DESTINATIONS
 * ---------------------
 * A cast to a type narrower than int (`s16 pan = 64.0f + ...;`, mdsel.c and
 * mdparty.c) is fctiwz followed by ORDINARY integer narrowing on hardware,
 * NOT a clamp into the narrow type's range -- 0x7FFFFFFF truncated to s16 is
 * -1, not 32767. So those sites wrap the float expression in this helper and
 * keep their existing integer conversion, which is already well defined.
 * That is a different rule from include/dolphin_compat.h's
 * OSf32tos16/OSf32tou8, which DO clamp into the narrow range: those model
 * `psq_st`, a quantized store, and quantized stores clamp. Do not use one
 * where the other belongs.
 *
 * This header is deliberately decomp-free (stdint.h only) so
 * tools/fctiwz_selftest.c can drive the exact code the runtime uses.
 */
#ifndef MP6_HWCAST_H
#define MP6_HWCAST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostic seam, called ONLY from the saturating paths below -- i.e. only
 * when a conversion has already left the representable range, which on
 * hardware was silent but is always evidence that something upstream
 * produced a degenerate value. Rate-limited by the implementation
 * (src/os/hwcast.c). tools/fctiwz_selftest.c supplies its own
 * definition so the header stays free of any port dependency. */
void mp6_hwcast_saturation_report(float value, const char *site);

/* Total saturations this process has seen. Zero is the expected value for
 * every run whose projections stayed well conditioned, which is what makes it
 * usable as a gate assertion rather than only as a log line. */
long mp6_hwcast_saturation_count(void);

/* `fctiwz` semantics for float -> s32. See the header comment for the four
 * rules and for why the guards cannot perturb an in-range conversion.
 *
 * Both boundary spellings are deliberate:
 *   - `value >= 2147483648.0f` on the high side because 2^31-1 is NOT
 *     representable in binary32: the largest float below 2^31 is 2147483520,
 *     which converts exactly, so "greater than 2^31-1" and "at least 2^31"
 *     name the same set of floats.
 *   - `value >= -2147483648.0f` is an ORDERED compare, so it is false for NaN
 *     as well as for everything below -2^31 -- precisely the set fctiwz maps
 *     to 0x80000000. Writing the low guard as the negation instead
 *     (`!(value > -2^31)`) would also swallow -2^31 ITSELF, which is exactly
 *     representable and IS in range: the returned value would still be right,
 *     but the diagnostic would cry "degenerate input" on a perfectly ordinary
 *     conversion. tools/fctiwz_selftest.c pins that boundary. */
static inline int32_t mp6_fctiwz_s32(float value, const char *site)
{
    if (value >= 2147483648.0f) {
        mp6_hwcast_saturation_report(value, site);
        return INT32_MAX;
    }
    if (value >= -2147483648.0f) {
        return (int32_t)value;
    }
    mp6_hwcast_saturation_report(value, site);
    return INT32_MIN;
}

/* The spelling patched into decomp sources: the call site names itself, so a
 * saturation report says which recovered expression degenerated without
 * anyone having to map an address back to a line. */
#define MP6_HWCAST_STR2(x) #x
#define MP6_HWCAST_STR(x) MP6_HWCAST_STR2(x)
#define MP6_FCTIWZ_S32(expr) mp6_fctiwz_s32((expr), __FILE__ ":" MP6_HWCAST_STR(__LINE__))

#ifdef __cplusplus
}
#endif

#endif /* MP6_HWCAST_H */
