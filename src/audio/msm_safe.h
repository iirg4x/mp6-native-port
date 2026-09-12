#ifndef MP6_MSM_SAFE_H
#define MP6_MSM_SAFE_H

#include <stdint.h>

/* A malformed but file-contained bank must not turn a tiny header into a
 * multi-gigabyte decoded allocation.  32 Mi PCM frames is over ten minutes
 * at MP6's authored 32 kHz stream rate and comfortably exceeds the retail
 * bank census; the byte ceiling bounds peak stream decode workspace to
 * 256 MiB (8 bytes per PCM frame for stereo output + L/R temporaries). */
#define MP6_MSM_MAX_DECODED_PCM_FRAMES (32u * 1024u * 1024u)
#define MP6_MSM_MAX_DECODED_BYTES      (256u * 1024u * 1024u)

static inline int mp6_msm_decode_budget_valid(uint64_t pcmFrames,
                                               uint32_t peakBytesPerFrame)
{
    return pcmFrames > 0 && pcmFrames <= MP6_MSM_MAX_DECODED_PCM_FRAMES &&
           peakBytesPerFrame > 0 &&
           pcmFrames <= (uint64_t)MP6_MSM_MAX_DECODED_BYTES / peakBytesPerFrame;
}

/* Small, dependency-free helpers shared by the production parser/mixer and
 * its focused regression test.  All arithmetic is widened before addition so
 * malformed 32-bit file offsets cannot wrap back into an apparently-valid
 * span. */
static inline int mp6_msm_span_valid_u32(uint32_t total, uint32_t offset, uint32_t size)
{
    return (uint64_t)offset <= (uint64_t)total &&
           (uint64_t)size <= (uint64_t)total - (uint64_t)offset;
}

/* Validate an authored SAMPLE_HEADER loop.  MuSyX data in MP6 contains a
 * handful of loop lengths that include the terminal sample (end == total+1),
 * so accept that documented on-disc shape and clamp the exclusive native end
 * to total.  Larger overruns are malformed.  Returns 1 for a loop, 0 for a
 * one-shot, and -1 for invalid loop metadata. */
static inline int mp6_msm_loop_bounds(uint32_t total, uint32_t start,
                                      uint32_t length, uint32_t *end_out)
{
    uint64_t end;
    if (length == 0) {
        if (end_out) *end_out = 0;
        return 0;
    }
    if (total == 0 || start >= total) {
        return -1;
    }
    end = (uint64_t)start + (uint64_t)length;
    if (end > (uint64_t)total + 1u || end <= start) {
        return -1;
    }
    if (end > total) end = total;
    if (end_out) *end_out = (uint32_t)end;
    return 1;
}

/* Wrap a Q16.16 source position into [start,end).  Modulo handles a very
 * large resampling step without repeated subtraction or a second OOB read. */
static inline int mp6_msm_wrap_q16(uint64_t *position, uint32_t start, uint32_t end)
{
    uint64_t start_q, end_q, span_q;
    if (position == 0 || start >= end) return 0;
    start_q = (uint64_t)start << 16;
    end_q = (uint64_t)end << 16;
    if (*position < end_q) return 1;
    span_q = end_q - start_q;
    *position = start_q + ((*position - end_q) % span_q);
    return 1;
}

/* Mixer end-of-buffer policy shared by streams and SFX. A clear authored
 * loop flag is terminal (the caller deactivates the voice and reports DONE);
 * a set flag wraps to the validated loop interval. */
static inline int mp6_msm_resolve_end_q16(uint64_t *position, int loop,
                                          uint32_t start, uint32_t end)
{
    return loop ? mp6_msm_wrap_q16(position, start, end) : 0;
}

/* ---------------------------------------------------------------------
 * SFX voice-table capacity (Enhancements: "Extended SFX voices").
 * ---------------------------------------------------------------------
 * The mixer's one-shot SFX voice table (msm_bridge.c's g_sfxVoice[]) is a
 * STATIC array sized to the EXTENDED capacity, and a separate ACTIVE COUNT
 * -- 16 or 32, latched once per run -- decides how much of it the runtime
 * may use.  Two sizes, not a free integer: the savestate marshal has to
 * name the capture-time capacity in the file (see mp6_savestate.h's
 * Mp6SsAudioShadow.voiceCap), and a two-valued field keeps the cross-
 * capacity restore rule below a rule rather than a matrix.
 *
 * WHY A GATE AND NOT TWO ARRAY SIZES.  Slot INDEX is identity here: it is
 * the mixer's render order, the savestate's restore target, and (through
 * first-free allocation) the order the game observes voices being handed
 * out in.  Retail mode must therefore not merely have "16 voices" -- it
 * must have the SAME 16 slots, chosen in the same order, as before this
 * feature existed.  A first-free scan bounded by the active count over an
 * array whose first 16 entries are laid out identically gives exactly
 * that: at cap 16 mp6_msm_voice_first_free() cannot return an index >= 16
 * and never inspects one, so the allocation sequence for a given input is
 * bit-for-bit the pre-existing one.  Sizing the array by the cap instead
 * would have made the savestate voice table two different on-disk shapes.
 */
#define MP6_MSM_SFX_VOICES_RETAIL   16
#define MP6_MSM_SFX_VOICES_EXTENDED 32

/* Fold a requested voice count onto a supported one.  Anything that is not
 * exactly the extended size means retail: an absent/garbled setting must
 * degrade to the GameCube-faithful table, never to an in-between size that
 * no savestate could name. */
static inline int mp6_msm_voice_cap_clamp(int requested)
{
    return requested == MP6_MSM_SFX_VOICES_EXTENDED ? MP6_MSM_SFX_VOICES_EXTENDED
                                                    : MP6_MSM_SFX_VOICES_RETAIL;
}

/* The only two values a capture may carry.  Used by the savestate voice
 * marshal's preflight, which must reject a corrupt capacity BEFORE it is
 * used to bound a loop over the on-disk 32-entry voice table. */
static inline int mp6_msm_voice_cap_valid(int cap)
{
    return cap == MP6_MSM_SFX_VOICES_RETAIL || cap == MP6_MSM_SFX_VOICES_EXTENDED;
}

/* First free voice slot below `cap`, or -1 when all `cap` slots are busy --
 * the single allocation rule msmSePlay() uses.  `active` is a per-slot
 * busy flag array with at least `cap` entries.  Returning -1 is exactly the
 * "all N SFX voice slots busy, dropped" refusal: the 17th simultaneous
 * start is refused at cap 16, the 33rd at cap 32, and the 17th SUCCEEDS at
 * cap 32. */
static inline int mp6_msm_voice_first_free(const unsigned char *active, int cap)
{
    int i;
    if (active == 0 || cap <= 0) return -1;
    for (i = 0; i < cap; i++) {
        if (!active[i]) return i;
    }
    return -1;
}

/* CROSS-CAPACITY SAVESTATE RULE, in one predicate.
 *
 * A capture records its own capacity, and the on-disk voice table is always
 * the extended 32 entries wide (holes are canonical zeros), so a file is
 * readable under either setting.  What differs is what can be RE-ESTABLISHED:
 *
 *   captured 16 -> restored under 32:  every captured slot is < 16 <= 32, so
 *       all of them restore, in their original slots.  The extra slots stay
 *       empty and are simply available to the next msmSePlay.  Nothing is
 *       lost, and slot identity (mixer order, handles) is exact.
 *
 *   captured 32 -> restored under 16:  slots 0..15 restore in place exactly;
 *       any voice captured in slot 16..31 has NO slot to go to and is
 *       DROPPED, with a line naming each one.  It is not remapped into a
 *       lower slot -- that would change mixer order and hand a game-visible
 *       handle to a different slot than the capture recorded -- and it is not
 *       a load failure either: a dropped one-shot SFX is indistinguishable
 *       from one that finished a moment earlier, which msmSeGetStatus already
 *       reports as MSM_SE_DONE.  The captured next-handle counter is restored
 *       unchanged, so a dropped voice's handle can never be re-issued.
 *
 * Both directions therefore LOAD; only the second can lose voices, and only
 * ones the smaller table provably cannot hold. */
static inline int mp6_msm_voice_slot_restorable(int slot, int liveCap)
{
    return slot >= 0 && slot < liveCap;
}

/* ---------------------------------------------------------------------
 * AUTHORED MACRO VOLUME ENVELOPE
 * ---------------------------------------------------------------------
 * WHAT THIS MODELS, AND WHAT IT DELIBERATELY DOES NOT.
 *
 * A MuSyX fx macro shapes its voice with a whole family of expression
 * opcodes.  Exactly three of them move the VOLUME, and all three are plain
 * piecewise-linear moves in the engine's own 16.16 volume domain, which is
 * why these three -- and only these three -- can be reproduced here without
 * a macro interpreter (src/musyx/runtime/synthmacros.c):
 *
 *   0x0d mcmdScaleVolume        -- an INSTANT set: volume := base*scale/127,
 *                                  where base is the CURRENT volume, or the
 *                                  voice's authored orgVolume when
 *                                  (u8)(para[1]>>8) != 0.
 *   0x0f mcmdEnvelope           -- a linear ramp from the CURRENT volume to
 *                                  (volume*scale)>>7 over N ms, then held.
 *   0x14 mcmdFadeIn             -- the same ramp, started from ZERO.
 *
 * (0x0f/0x14 share DoEnvelopeCalculation.  The ramp advances once per
 * millisecond in synth.c: `envCurrent += envDelta * (lowDeltaTime >> 8)`,
 * clamped at envTarget -- which is what makes "N milliseconds" literal and
 * the move exactly linear in the volume domain.)
 *
 * NOT MODELLED, and each one makes the scan refuse the whole envelope rather
 * than approximate it: 0x0c SetADSR / 0x16 SetADSRFromCtrl (a real
 * attack/decay/sustain/release state machine driven by keyoff, synth_adsr.c),
 * 0x21 ScaleVolumeDLS, the additive bias byte (u8)(para[0]>>16), a non-0xFFFF
 * volume CURVE (TranslateVolume's dataGetCurve lookup), a tick-timed (tempo-
 * scaled) ramp duration, and anything that makes macro time non-linear.
 *
 * WHAT THE MULTIPLIER IS RELATIVE TO.  Every one of the three opcodes is
 * MULTIPLICATIVE on the running volume, and mcmdScaleVolume's alternate base
 * is the voice's own orgVolume -- so the resulting curve, expressed as a
 * fraction of orgVolume, is INDEPENDENT of what orgVolume actually is.  That
 * is what makes this portable at all: `orgVolume` is modelled as full scale
 * below, the program is normalized against full scale, and the port's mixer
 * keeps applying the fx's authored volume through its own existing baseVol
 * term.  A macro that never moves the volume compiles to no segments and the
 * multiplier is exactly 1.0.
 *
 * The one place the modelled orgVolume is not invariant is the engine's
 * 0x7f0000 CLAMP.  Modelling it as full scale makes the clamp fire EARLIER
 * here than on real hardware (where orgVolume is usually lower), and a clamp
 * is a refusal -- so this direction can only ever be conservative: it never
 * misses a clamp the hardware would have applied.
 *
 * DOMAIN NOTE.  The engine's volume reaches the DSP through a dB-shaped table
 * (musyx_vol_tab in hw_volconv.c's CalcBus), while this port's mixer maps
 * every volume LINEARLY (baseVol/127 * vol/127 * master/127).  That
 * divergence predates this envelope and is not touched here; the multiplier
 * rides on top of whatever law the mixer already uses.
 *
 * THE SAFETY PROPERTY.  segCount == 0 means "this macro authors no volume
 * move I can model", and msm_bridge.c's mixer then does not touch the voice's
 * gain expression at all -- the rendered samples are bit-identical to the
 * pre-envelope build.  Every refusal above lands there.
 *
 * The evaluator lives here, not in msm_bridge.c, so tools/msm_envelope_selftest.c
 * exercises the exact functions the audio callback runs.
 *
 * WHY THE SEGMENT CEILING IS 6 AND NOT THE MEASURED 2.  The widest program
 * this disc compiles is 2 segments (seId 1217: attack, hold, decay), so 6 is
 * three times the measured need.  It stays at 6 because a capacity is only
 * safely tightenable to a bound the FORMAT cannot exceed, and no such bound
 * exists:
 *
 *   - Of the three accepted opcodes, 0x0d mcmdScaleVolume is an INSTANTANEOUS
 *     set with no state that stops it repeating.  N consecutive 0x0d steps in
 *     an otherwise linear macro emit N moves, every one of them at the same
 *     macro time, and the sequential-time rule (a move may not START before
 *     the previous one ENDS -- msm_bridge.c's envPrevEndMs check) accepts all
 *     N.  So the accepted opcode set can structurally emit as many moves as
 *     the macro has steps.
 *   - Nothing in the MuSyX pool format bounds that step count usefully.  A
 *     macro is a variable-length MSTEP array inside a MEM_DATA node whose only
 *     length information is a u32 self-relative `nextOff`
 *     (include/musyx/synthdata.h); the 0x400-byte `data` union in the
 *     decompiled struct is sized by its KEYMAP member (128 x 8), not by any
 *     macro rule, and the 64-step window in find_macro_first_sample is this
 *     reader's own budget rather than a format rule.  Even taking that union
 *     at face value as a macro ceiling gives 0x400 / sizeof(MSTEP) = 128
 *     steps, i.e. 128 moves -- 21x this constant.  There is no candidate
 *     bound anywhere in the format that is SMALLER than 6, which is what
 *     tightening would need.
 *
 * So this constant is a QUALITY knob, not a safety bound.  Exceeding it is the
 * MP6_ENVWHY_TOOMANY refusal, which leaves the voice at the pre-envelope flat
 * gain -- i.e. shrinking it can only ever turn a sound that works today into a
 * refused one.  And it is no longer a footprint argument either: the compiled
 * program lives in a SPARSE side table keyed by fx index (msm_bridge.c's
 * MsmFxEnvRow -- one row per COMPILED program, ten disc-wide), so each segment
 * of headroom costs 16 bytes x 10 rows = 160 bytes for the entire bank. */
#define MP6_MSM_SE_ENV_MAX_SEGS 6

/* Full scale in the engine's 16.16 volume domain: 127.0, the same ceiling
 * mcmdScaleVolume/DoEnvelopeCalculation clamp against (0x7f0000). Also the
 * modelled orgVolume, and the value every multiplier is normalized against. */
#define MP6_MSM_ENV_UNITY 0x7f0000u

typedef struct {
    uint32_t startMs;  /* start of this move, ms after the macro's StartSample */
    uint32_t durMs;    /* 0 == an instantaneous set (mcmdScaleVolume) */
    float from;        /* multiplier at startMs */
    float to;          /* multiplier at startMs+durMs, and held after it */
} Mp6MsmEnvSeg;

typedef struct {
    uint8_t segCount;  /* 0 == nothing modelled; the mixer stays exactly flat */
    Mp6MsmEnvSeg seg[MP6_MSM_SE_ENV_MAX_SEGS];
} Mp6MsmSeEnv;

/* The same program with every millisecond boundary resolved ONCE to the
 * voice's own sample-frame clock, so the per-frame evaluator below is a
 * couple of compares and one lerp -- no division, no per-voice mutable
 * state, nothing for a savestate to marshal. */
typedef struct {
    uint8_t  segCount;
    uint64_t startF[MP6_MSM_SE_ENV_MAX_SEGS];
    uint64_t endF[MP6_MSM_SE_ENV_MAX_SEGS];
    float    from[MP6_MSM_SE_ENV_MAX_SEGS];
    float    to[MP6_MSM_SE_ENV_MAX_SEGS];
} Mp6MsmEnvPlan;

/* Fold a compiled 16.16 volume into a multiplier of full scale.  Returns 0 --
 * refuse the program -- for a volume outside the engine's own domain, so no
 * caller has to re-derive the ceiling.  The result is always in [0,1]. */
static inline int mp6_msm_env_mul_from_vol(uint32_t vol, float *out)
{
    if (out == 0 || vol > MP6_MSM_ENV_UNITY) return 0;
    *out = (float)vol / (float)MP6_MSM_ENV_UNITY;
    return 1;
}

/* ONE volume opcode, decoded exactly as synthmacros.c executes it.
 *
 * `curVol` is the running 16.16 volume; `orgVol` is the voice's orgVolume,
 * which mcmdScaleVolume can select as its base instead (MP6_MSM_ENV_UNITY --
 * see the note above on why modelling it as full scale is exact for the
 * multiplier and conservative for the clamp).  On success outFrom and outTo
 * receive the move's endpoints in the same 16.16 domain and outDurMs its
 * duration (0 for an instantaneous set).  Returns 0 for "not this port's business" --
 * either not a volume opcode at all, or one carrying a feature listed as NOT
 * MODELLED above; the caller must then refuse the envelope, never
 * approximate it. */
static inline int mp6_msm_env_vol_step(uint8_t opcode, uint32_t p0, uint32_t p1,
                                       uint32_t curVol, uint32_t orgVol,
                                       uint32_t *outFrom, uint32_t *outTo,
                                       uint32_t *outDurMs)
{
    uint32_t scale = (p0 >> 8) & 0xFFu;
    uint32_t bias  = (p0 >> 16) & 0xFFu;                      /* additive, not modelled */
    uint32_t curve = ((p0 >> 24) & 0xFFu) | ((p1 & 0xFFu) << 8);
    uint64_t v;

    if (outFrom == 0 || outTo == 0 || outDurMs == 0) return 0;
    if (bias != 0) return 0;
    if (curve != 0xFFFFu) return 0; /* TranslateVolume applies a real curve */

    if (opcode == 0x0Du) { /* mcmdScaleVolume -- instantaneous */
        uint32_t base = ((p1 >> 8) & 0xFFu) != 0 ? orgVol : curVol;
        v = ((uint64_t)base * scale) / 127u;
        if (v > MP6_MSM_ENV_UNITY) return 0; /* the engine clamps; a clamp is not linear */
        *outFrom = curVol;
        *outTo = (uint32_t)v;
        *outDurMs = 0;
        return 1;
    }

    if (opcode == 0x0Fu || opcode == 0x14u) { /* mcmdEnvelope / mcmdFadeIn */
        uint32_t durMs;
        if (((p1 >> 8) & 1u) == 0) return 0; /* tick-timed: sndConvertTicks needs live tempo */
        durMs = (p1 >> 16) & 0xFFFFu;
        if (durMs == 0) durMs = 1;           /* DoEnvelopeCalculation's own mstime==0 -> 1 */
        v = ((uint64_t)curVol * scale) >> 7;
        if (v > MP6_MSM_ENV_UNITY) return 0;
        *outFrom = (opcode == 0x14u) ? 0u : curVol;
        *outTo = (uint32_t)v;
        *outDurMs = durMs;
        return 1;
    }

    return 0;
}

/* Is this program a CONSTANT -- every move instantaneous and at time zero?
 *
 * This is the one class of program that needs no clock at all, and saying so
 * matters: the mixer reads the envelope at the voice's sample INDEX, which is
 * a faithful stand-in for elapsed time only while the index rises
 * monotonically from zero. A LOOPING voice rewinds its index on every wrap, so
 * a timed program must not be installed on one -- but a constant multiplier is
 * the same at every index, wrap or not, so it can be. That is exactly the case
 * MP6's own bank needs: a macro that sets the volume once before its
 * StartSample and then holds until keyoff. */
static inline int mp6_msm_env_is_constant(const Mp6MsmSeEnv *env)
{
    int i;
    if (env == 0 || env->segCount == 0) return 0;
    for (i = 0; i < (int)env->segCount && i < MP6_MSM_SE_ENV_MAX_SEGS; i++) {
        if (env->seg[i].startMs != 0 || env->seg[i].durMs != 0) return 0;
    }
    return 1;
}

/* Resolve a compiled program onto a voice whose playback clock ticks in
 * `rateHz` sample frames.  Returns 1 when `out` carries a usable plan, 0 when
 * it carries the neutral one (segCount 0 -- the mixer leaves gain alone).
 * Fails closed on a non-monotonic or wrapping program rather than trusting
 * the compiler upstream. */
static inline int mp6_msm_env_plan(const Mp6MsmSeEnv *env, uint32_t rateHz,
                                   Mp6MsmEnvPlan *out)
{
    int i;
    uint64_t prevEnd = 0;
    if (out == 0) return 0;
    out->segCount = 0;
    for (i = 0; i < MP6_MSM_SE_ENV_MAX_SEGS; i++) {
        out->startF[i] = 0; out->endF[i] = 0;
        out->from[i] = 1.0f; out->to[i] = 1.0f;
    }
    if (env == 0 || rateHz == 0 || env->segCount == 0) return 0;
    if (env->segCount > MP6_MSM_SE_ENV_MAX_SEGS) return 0;
    for (i = 0; i < (int)env->segCount; i++) {
        uint64_t startF = ((uint64_t)env->seg[i].startMs * rateHz) / 1000u;
        uint64_t endF = (((uint64_t)env->seg[i].startMs + (uint64_t)env->seg[i].durMs)
                         * rateHz) / 1000u;
        float from = env->seg[i].from, to = env->seg[i].to;
        if (endF < startF || startF < prevEnd) return 0; /* not monotonic in time */
        /* The !(>=) forms also reject NaN, which a garbled bank could reach
         * only through a corrupted in-memory program -- but this is the last
         * gate before a float goes into the audio callback's gain. */
        if (!(from >= 0.0f) || from > 1.0f) return 0;
        if (!(to >= 0.0f) || to > 1.0f) return 0;
        out->startF[i] = startF;
        out->endF[i] = endF;
        out->from[i] = from;
        out->to[i] = to;
        prevEnd = endF;
    }
    out->segCount = env->segCount;
    return 1;
}

/* The multiplier at sample frame `frame`.  Exactly 1.0f before the first
 * move and for an empty plan, so a caller can apply it unconditionally
 * without changing a voice that has no envelope. */
static inline float mp6_msm_env_mul(const Mp6MsmEnvPlan *plan, uint64_t frame)
{
    int i;
    float level = 1.0f;
    if (plan == 0 || plan->segCount == 0) return 1.0f;
    for (i = 0; i < (int)plan->segCount && i < MP6_MSM_SE_ENV_MAX_SEGS; i++) {
        if (frame >= plan->endF[i]) {   /* finished -- also the instantaneous case */
            level = plan->to[i];
            continue;
        }
        if (frame < plan->startF[i]) return level; /* not started yet */
        /* At or inside the move. The boundary belongs to the MOVE, not to the
         * level before it: DoEnvelopeCalculation assigns svoice->volume =
         * start_vol the instant it runs, so a mcmdFadeIn at time zero is
         * SILENT on its first frame. Treating startF as "not started" instead
         * emitted one frame at the pre-fade level -- a full-scale sample in
         * front of every fade-in. endF > frame >= startF here, so the span
         * below is non-zero. */
        return plan->from[i] + (plan->to[i] - plan->from[i]) *
               ((float)(frame - plan->startF[i]) /
                (float)(plan->endF[i] - plan->startF[i]));
    }
    return level;
}

/* Convert a positive millisecond fade duration to output frames without a
 * narrowing wrap.  INT_MAX milliseconds at 48 kHz exceeds uint32_t; a
 * saturated duration remains monotonic and simply approaches a zero step. */
static inline uint32_t mp6_msm_fade_frames_from_ms(int32_t speedMs, uint32_t rateHz)
{
    uint64_t frames;
    if (speedMs <= 0 || rateHz == 0) return 0;
    frames = ((uint64_t)(uint32_t)speedMs * rateHz) / 1000u;
    if (frames == 0) frames = 1;
    if (frames > UINT32_MAX) frames = UINT32_MAX;
    return (uint32_t)frames;
}

#endif /* MP6_MSM_SAFE_H */
