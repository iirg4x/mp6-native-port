/* MP6 native port -- MWCC/PPC compat prelude for the decomp's dolphin
 * headers, force-included (`-include dolphin_compat.h`) ahead of every
 * translation unit so the ORIGINAL decomp headers under
 * external_refs/repos/marioparty6/include never need to be touched.
 *
 * Most of dolphin/*.h already tolerates non-MWERKS compilers on its own
 * (AT_ADDRESS(x) -> empty, ATTRIBUTE_ALIGN -> __attribute__, TARGET_PC
 * branches for types.h/vi.h/pad.h/gx headers). The handful of spots that
 * do NOT degrade gracefully are pre-empted here via the include-guard
 * trick: define the header's own guard macro before it's ever reached,
 * and supply a clang-safe replacement for its contents (re-declaring any
 * real, non-inline prototypes the original header also carried, so
 * nothing downstream regresses to an implicit declaration).
 *
 * tools/build.py defines TARGET_PC globally for every decomp translation
 * unit. The TARGET_PC branch's declarations match Aurora's own copies of
 * the same headers exactly for GXVert.h/GXBump.h/GXGeometry.h/
 * GXTransform.h (both trees are independent forks of the same original
 * PC-compat header set) -- which is what lets the game's real GX calls
 * link directly against Aurora's compiled implementations instead of a
 * hand-maintained shim. Do NOT add include-guard preemptions for those
 * headers: with TARGET_PC defined they declare plain `extern` prototypes
 * (not `static inline` GXFIFO_ADDR pokes), so there is nothing to
 * preempt, and an override would SHADOW the real declarations with fake
 * ones and defeat the point of linking Aurora in.
 */
#ifndef MP6_DOLPHIN_COMPAT_H
#define MP6_DOLPHIN_COMPAT_H

/* include/musyx/musyx.h (pulled in via game/msm.h, itself pulled in by
 * ~40 game files through game/audio.h) uses bare `bool` return types and
 * parameters with no #include <stdbool.h> of its own anywhere in its
 * include chain -- "unknown type name 'bool'" at ~300 call sites across
 * the header until this is included first. */
#include <stdbool.h>

#include <dolphin/types.h>

/* include/musyx/musyx.h defaults MUSY_TARGET to MUSY_TARGET_PC (0) via its
 * own `#ifndef MUSY_TARGET` guard in musyx/platform.h -- which makes IT
 * typedef u8/u32/etc itself, conflicting with dolphin/types.h's own ones
 * the moment both are visible in the same TU ("typedef redefinition with
 * different types") unless the two agree on the underlying type. Pre-
 * defining MUSY_TARGET (their own #ifndef guard lets this stick, unlike
 * AT_ADDRESS above) keeps MuSyX's fallback typedefs consistent with
 * dolphin's.
 *
 * The value must be 0 == MUSY_TARGET_PC: tools/build.py defines TARGET_PC
 * globally (see COMMON_FLAGS's own comment), which flips
 * dolphin/types.h's s32/u32 from `signed long`/`unsigned long` to the
 * stdint.h `int32_t`/`uint32_t` (still 4 bytes on this LLP64 target, but
 * a DIFFERENT type for C's typedef-redefinition rule, which cares about
 * the type, not just its size) -- musyx.h's MUSY_TARGET_DOLPHIN branch
 * typedefs s32/u32 as `signed long`/`unsigned long` unconditionally (it
 * doesn't know about TARGET_PC at all), which does not match and
 * hard-errors; musyx.h's OWN MUSY_TARGET_PC branch uses `signed
 * int`/`unsigned int` instead, which IS compatible with int32_t/uint32_t
 * (a typedef to `int` either way). The two MUSY_TARGET-gated behavioral
 * differences elsewhere (musyx/assert.h's MUSY_PANIC/MUSY_REPORT, only
 * used inside `#ifdef _DEBUG` blocks this build never defines;
 * musyx/dsp_import.h's dspSlave/dspSlaveLength externs, only referenced
 * from src/musyx/runtime/hw_dolphin.c, which -- like the rest of decomp's
 * own from-scratch SDK reimplementation under src/dolphin/ / src/musyx/
 * -- is never compiled into this port at all) are both inert either way
 * for this build. */
#ifndef MUSY_TARGET
#define MUSY_TARGET 0
#endif

/* include/game/audio.h include "msm.h" (which, from within include/game/,
 * resolves to include/game/msm.h -- a facade that #define MSM_H, defines
 * MSM_AUX_NONE/MSM_GRP_NONE-style constants, etc.) BEFORE separately also
 * #include-ing include/msm_aux.h, include/msm_grp.h, ... (the lower-level
 * headers whose OWN enums declare enumerATORS of those exact same names,
 * e.g. `enum MSM_AUX_ID { MSM_AUX_NONE = -1, ... }`). With game/msm.h's
 * macro already active by the time those enums are parsed, the
 * enumerator name is textually replaced by its own macro's value first,
 * producing nonsense like `-1 = -1,` ("expected identifier"). Pulling
 * these 4 low-level, dependency-free headers in FIRST (their real job is
 * just an enum or a block of plain #defines each) lets their enums be
 * parsed for real while the names are still just identifiers; the later,
 * redundant macro definitions from game/msm.h are then harmless (same
 * values either way) since their own include guards skip re-parsing. */
#include <msm_aux.h>
#include <msm_grp.h>
#include <msm_se.h>
#include <msm_stream.h>

/* include/game/msm.h and the unrelated top-level include/msm.h share one
 * include guard (MSM_H) -- see tools/build.py's GUARD_RENAME_PATCH_FILES
 * comment for why that's left alone rather than "fixed" (the two headers
 * declare ~30 of the same functions with incompatibly different types,
 * so making both visible at once is worse than the original collision).
 * Whichever header a given TU reaches first is authoritative; game/msm.h
 * wins for every real call site in this slice except two, each verified
 * (grep -rl) to have exactly one user:
 *   - src/REL/selmenuDll/selmenu.c uses the top-level header's `MSMSE`
 *     type name (with a `.comp` field game/msm.h's independent MSM_SE,
 *     game/msm_data.h, doesn't have) -- fixed per-file in
 *     shim/include/selmenu_compat.h (-include'd for that ONE file only;
 *     doing it here in the GLOBAL compat header instead broke
 *     platform/null/shims_generated.c, which -- living outside
 *     include/game/ -- reaches the top-level msm.h directly and hits its
 *     own real `MSMSE` typedef, conflicting with a blanket compat one).
 *   - src/game/audio.c uses MSM_STREAMNO_NONE, defined only in the
 *     top-level header; re-defined here with its exact same value (a
 *     plain integer constant -- no per-file/conflict concerns like MSMSE).
 */
#ifndef MSM_STREAMNO_NONE
#define MSM_STREAMNO_NONE -1
#endif

/* src/REL/bootDll/opening.c forward-declares OpeningBgUpdate (line 114,
 * external linkage) but DEFINES it `static` (line 588) -- a real,
 * self-contained bug in that one file ("static declaration follows
 * non-static declaration", a hard error with no -Wno-error=... escape
 * hatch; verified). C linkage rules make a `static` declaration of a
 * name STICK for every later declaration of the same name in the same TU
 * even without repeating `static` -- so a static forward-declaration
 * here, reached before opening.c's own line 114, makes that later
 * non-static-looking re-declaration inherit internal linkage instead of
 * conflicting. Harmless everywhere else: `-w` suppresses the resulting
 * "unused static function" warning in the ~60 other TUs that never
 * define or call a function of this name. */
static void OpeningBgUpdate(void);

/* Same issue, same fix: src/game/data.c declares HuDataDirReadNum non-
 * static in include/game/data.h:42 but defines it `static` at data.c:147.
 * Its only other referencer anywhere in the tree is src/game/kerent.c
 * (grep -rl), which is skipped from the build entirely -- so internal
 * linkage is also safe here, nothing else needs to call it across TUs.
 *
 * HuDataDirReadNum returns HUDATASTAT*, not yet known this early --
 * forward-declare the tag (incomplete type, sufficient for a pointer in
 * a prototype) with the SAME typedef name game/data.h uses
 * (`struct HuDataStat_s`); its later `typedef struct HuDataStat_s {
 * ... } HUDATASTAT;` (this time with a full body) completes the same tag
 * rather than conflicting -- standard, well-supported C (a typedef may
 * be repeated denoting the same type; a struct tag may be forward-
 * declared then completed later in the same TU). */
typedef struct HuDataStat_s HUDATASTAT;
static HUDATASTAT *HuDataDirReadNum(int dataNum, s32 num);

/* Same issue again: src/game/mic.c forward-declares HuMCMicSaveGet as
 * plain `inline` (line 62, NOT `static inline`) and defines it the same
 * way at line 1026. Plain C99 `inline` (no `static`, no `extern`) without
 * an `extern inline` counterpart somewhere provides NO out-of-line
 * definition at all -- if the compiler doesn't inline every one of
 * mic.c's own ~20 call sites (it doesn't, at -O0), the leftover calls
 * have nothing to link against ("undefined symbol: HuMCMicSaveGet",
 * distinct from the static-vs-non-static class of bug above, but the
 * same `static` forward-declare trick forces internal linkage and sidesteps
 * it either way). Only ever used within mic.c itself. */
static s32 HuMCMicSaveGet(void);

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * 1. include/dolphin/os/OSFastCast.h
 *
 * Every helper below OSInitFastCast() in the real header uses an MWCC
 * `asm { psq_st ...; lha ... }` block with NO `#ifdef __MWERKS__` guard
 * (only OSInitFastCast's own asm block is guarded) -- clang cannot parse
 * MWCC-style register asm syntax at all, so the header hard-fails the
 * moment it's textually included, whether or not the functions are ever
 * called. Two call sites in our slice (game/hsfdraw.c, game/hsfanim.c)
 * use OSf32tos16 for a float->s16 "fast cast"; PPC's paired-single
 * quantized store truncates toward zero same as a plain C cast, so a
 * plain cast is a faithful stand-in.
 * --------------------------------------------------------------------- */
#define _DOLPHIN_OSFASTCAST

static inline void OSInitFastCast(void) { /* PPC GQR setup -- no-op on x86 */ }

static inline s16 OSf32tos16_impl(f32 f) { return (s16)f; }
static inline u8  OSf32tou8_impl(f32 f)  { return (u8)f; }
static inline s8  OSf32tos8_impl(f32 f)  { return (s8)f; }
static inline u16 OSf32tou16_impl(f32 f) { return (u16)f; }

static inline void OSf32tos16(f32 *f, s16 *out) { *out = (s16)(*f); }
static inline void OSf32tou8(f32 *f, u8 *out)   { *out = (u8)(*f); }
static inline void OSf32tos8(f32 *f, s8 *out)   { *out = (s8)(*f); }
static inline void OSf32tou16(f32 *f, u16 *out) { *out = (u16)(*f); }

static inline void OSs8tof32(const s8 *in, float *out)   { *out = (float)(*in); }
static inline void OSs16tof32(const s16 *in, float *out) { *out = (float)(*in); }
static inline void OSu8tof32(const u8 *in, float *out)   { *out = (float)(*in); }
static inline void OSu16tof32(const u16 *in, float *out) { *out = (float)(*in); }

/* ---------------------------------------------------------------------
 * 2. include/dolphin/gx/GXDispList.h
 *
 * GXFastCallDisplayList() is `static inline` with an UNCONDITIONAL raw
 * MMIO poke to the GameCube write-gather-pipe address (0xCC008000) --
 * not asm, so it parses fine under clang, but dereferencing that address
 * on a PC is an instant access violation. Called from game/hsfdraw.c
 * (2 call sites, both deep in the HSF model draw path, guarded by
 * `if(matP != materialBak)`/`else` -- see FaceDraw()/the analogous shadow
 * function: the "then" branch re-establishes TEV/vtx-format render state
 * and calls the plain GXCallDisplayList; the "else" branch (same material
 * as the immediately-previous face, no state change needed) calls THIS
 * function instead). Re-declares the 3 real (non-inline) prototypes from
 * the same header so they still get a normal generated shim.
 *
 * GXFastCallDisplayList MUST forward to the real GXCallDisplayList, never
 * no-op: on real hardware it is NOT a "do nothing" call -- it executes
 * the exact same display-list bytes as GXCallDisplayList (the actual
 * GXPosition/GXNormal/GXTexCoord/primitive stream for that face), just
 * via a lower-overhead MMIO path that skips re-flushing GX state the
 * caller already knows is unchanged. A no-op stand-in silently drops the
 * OVERWHELMING majority of HSF face draws (every face sharing its
 * material with the immediately-preceding face -- the common case within
 * one mesh): sprites/text still render (immediate-mode GXBegin, never
 * this path) while meshes vanish.
 *
 * The forward resolves at link time to Aurora's real
 * display-list-execution implementation in the default build, and to the
 * null shim's logged no-op in --headless, matching how every other real
 * GX call resolves per build mode -- no bridge file needed. Aurora's own
 * GXCallDisplayList already guards its state-flush behind
 * `if (__gx->dirtyState != 0)`, so calling it from the "state is already
 * correct" call site is redundant-but-harmless, never wrong -- the
 * visual result of executing a display list is identical either way; the
 * real-hardware distinction this skips (an MMIO shortcut around the SDK's
 * own bookkeeping) is a pure performance optimization with no bearing on
 * what actually gets drawn. */
#define _DOLPHIN_GXDISPLIST

/* GXBeginDisplayList/GXEndDisplayList renamed to
 * mp6_GXBeginDisplayList/mp6_GXEndDisplayList -- the same PURE-rename
 * mechanism as GXCallDisplayList's #define below (and gen_shims.py
 * MACRO_RESOLUTION covers the --headless build's null shims under the
 * resolved names). Hook point so platform/gx/aurora_bridge.c can track
 * when the game is RECORDING a display list: its learned-size GXSetArray
 * machinery must never emit a re-bind while recording is active, because
 * Aurora's GXSetArray writes LOAD_ARRAYBASE commands into whatever the
 * FIFO currently targets -- during recording that is the game's own DL
 * buffer (unbudgeted bytes + a replayed-every-frame foreign array
 * re-bind). */
void mp6_GXBeginDisplayList(void* list, u32 size);
u32 mp6_GXEndDisplayList(void);
#define GXBeginDisplayList mp6_GXBeginDisplayList
#define GXEndDisplayList mp6_GXEndDisplayList

/* GXCallDisplayList renamed to mp6_GXCallDisplayList -- a PURE rename,
 * the exact same mechanism as GXBegin/GXEnd's own `#define` below
 * (section 4), NOT an arity/ABI bridge like mp6_GXSetArray3 -- just a
 * hook point so platform/gx/aurora_bridge.c can count/optionally-hide
 * individual display-list-execute draws for the draw-call bisect harness
 * (MP6_SKIP_DRAWS). aurora_bridge.c's own mp6_GXCallDisplayList calls the
 * REAL GXCallDisplayList afterward (visible there since that file is
 * compiled against Aurora's OWN headers, never this one, so this #define
 * can't touch its own definition -- same reasoning as
 * mp6_GXSetArray3/mp6_GXBegin's own comments). Placed BEFORE
 * GXFastCallDisplayList's own body (right below) so its internal
 * forwarding call is ALSO redirected and counted -- both are real
 * per-face draw-submission paths (see GXFastCallDisplayList's own header
 * comment above), and a draw-index bisect that silently missed half of
 * them (whichever faces happen to share a material with the one before)
 * would be wrong. */
void mp6_GXCallDisplayList(const void* list, u32 nbytes);
#define GXCallDisplayList mp6_GXCallDisplayList

/* Android only: PADControlMotor renamed to mp6_PADControlMotor -- the
 * exact same PURE-rename mechanism as GXCallDisplayList above, so
 * platform/gx/aurora_bridge.c can DEFER the motor command to the next
 * VIWaitForRetrace instead of executing it on the CALLER's stack. Why
 * deferral is load-bearing on Android and nowhere else: game code calls
 * this from HuPrc coroutines (e.g. omOvlKill -> HuPadRumbleAllStop on an
 * overlay transition), whose stacks live in the low-4GB arena pool -- and
 * on Android aurora's PADControlMotor can reach the SYSTEM VIBRATOR
 * through SDL, a JNI upcall. ART's JNI-entry stack-bounds check reads the
 * current sp, finds it outside the thread's registered stack, and throws
 * a spurious java.lang.StackOverflowError that aborts the app at the next
 * checked JNI call. NO JNI may ever be entered from a coroutine stack
 * (see docs/ARCHITECTURE.md); the bridge applies the queued command from
 * VIWaitForRetrace -- main-loop context, real thread stack, same tick or
 * the next one (rumble latency +<=16ms, imperceptible). Windows/headless:
 * no rename, byte-identical preprocessed output (the guard evaporates). */
#ifdef __ANDROID__
void mp6_PADControlMotor(s32 chan, u32 cmd);
#define PADControlMotor mp6_PADControlMotor
#endif

/* Memory card slot A (platform/os/card_native.c): the SDK's CARDInit is
 * (void); aurora's is an extension taking (gameCode, makerCode). Reroute
 * the game's single call site (game/card.c HuCardInit) through the
 * interposer, which supplies "GP6E"/"01" + the saves base path in aurora
 * builds and stays an honest no-op in headless builds. Unconditional in
 * both modes (game objects are byte-shared across modes -- see the
 * framescope block's comment below). */
#ifndef MP6_GENSHIMS_PROBE
void mp6_CARDInit(void);
#define CARDInit mp6_CARDInit
#endif

/* FRAMESCOPE (fable diagnostic toolkit, platform/gx/framescope.c): reroute
 * the game's TEV/texture-state calls through logging wrappers so one env
 * var (MP6_FRAMESCOPE=N) captures a complete frame's configuration trace,
 * per draw index. Wrappers forward to the real Aurora implementations;
 * overhead when disabled is one integer check per call. Renames stay
 * UNCONDITIONAL in both build modes so game/REL objects remain byte-shared
 * (build.py's invariant): in the aurora build framescope.c defines the
 * strong mp6_fs_* symbols; in the headless build these same renames apply
 * to shims_generated.c's weak GX no-op DEFINITIONS too, which therefore
 * become weak mp6_fs_* no-ops -- the loop closes with no headless
 * framescope TU at all. */
#ifndef MP6_GENSHIMS_PROBE /* gen_shims probe must see pristine SDK names */
#define GXSetTevColorIn        mp6_fs_GXSetTevColorIn
#define GXSetTevAlphaIn        mp6_fs_GXSetTevAlphaIn
#define GXSetTevColorOp        mp6_fs_GXSetTevColorOp
#define GXSetTevAlphaOp        mp6_fs_GXSetTevAlphaOp
#define GXSetTevOrder          mp6_fs_GXSetTevOrder
#define GXSetNumTevStages      mp6_fs_GXSetNumTevStages
#define GXSetTevKColor         mp6_fs_GXSetTevKColor
#define GXSetTevKColorSel      mp6_fs_GXSetTevKColorSel
#define GXSetTevKAlphaSel      mp6_fs_GXSetTevKAlphaSel
#define GXSetTevSwapMode       mp6_fs_GXSetTevSwapMode
#define GXSetTevSwapModeTable  mp6_fs_GXSetTevSwapModeTable
#define GXLoadTexObj           mp6_fs_GXLoadTexObj
#define GXSetBlendMode         mp6_fs_GXSetBlendMode
#define GXSetZMode             mp6_fs_GXSetZMode
#define GXSetTexCopySrc        mp6_fs_GXSetTexCopySrc
#define GXSetTexCopyDst        mp6_fs_GXSetTexCopyDst
#define GXCopyTex              mp6_fs_GXCopyTex
/* TLUT/palettized-texture probes -- same unconditional-rename rules as
 * the block above. */
#define GXInitTlutObj          mp6_fs_GXInitTlutObj
#define GXLoadTlut             mp6_fs_GXLoadTlut
#define GXInitTexObjCI         mp6_fs_GXInitTexObjCI
/* Texmtx/texgen probes. The GXSetTexCoordGen 4-arg form is a header
 * inline forwarding to GXSetTexCoordGen2, so renaming the 2-suffix
 * symbol catches both. */
#define GXLoadTexMtxImm        mp6_fs_GXLoadTexMtxImm
#define GXSetTexCoordGen2      mp6_fs_GXSetTexCoordGen2
#endif /* !MP6_GENSHIMS_PROBE (framescope renames) */


void mp6_shim_log(const char *family, const char *name); /* see mp6_shim_log.h */

static inline void GXFastCallDisplayList(void *list, u32 size)
{
    static int mp6_logged__ = 0;
    if (!mp6_logged__) {
        mp6_logged__ = 1;
        mp6_shim_log("GX", "GXFastCallDisplayList (forwarded to real GXCallDisplayList, see dolphin_compat.h)");
    }
    GXCallDisplayList(list, size);
}

/* ---------------------------------------------------------------------
 * 3. include/dolphin/gx/GXGeometry.h -- GXSetArray rename.
 *
 * tools/build.py's patch_abi_struct_headers() ships a shadow
 * dolphin/gx/GXGeometry.h that pins GXSetArray to an unconditional 3-arg
 * declaration (GXAttr, void*, u8 stride -- matching every real decomp
 * call site, confirmed via grep; see that function's own comment), so
 * decomp always compiles clean against the arity it actually uses.
 *
 * That fixes the DECLARATION, but not what the compiled CALL resolves to
 * at link time: the aurora (non-headless) build links Aurora's real
 * GXGeometry.cpp.o, whose OWN GXSetArray is a genuinely different,
 * 5-argument function (it needs an explicit byte size Aurora's PC/WebGPU
 * backend requires but real hardware and decomp's call sites never did --
 * see platform/gx/aurora_bridge.c's own comment). Two same-named, ABI-
 * incompatible definitions of `GXSetArray` can't coexist in one link
 * (verified empirically: lld-link, this toolchain's linker for
 * x86_64-windows-gnu, hard-errors with "duplicate symbol" the moment
 * BOTH a direct object and an archive member pulled in for other reasons
 * define the same strong symbol -- there's no safe way to just rely on
 * link ORDER here) -- and this toolchain's zig cc/c++ frontend rejects
 * `-Wl,--wrap` outright ("unsupported linker arg: --wrap"), so the usual
 * "GNU ld --wrap" interposition trick that would otherwise apply here
 * isn't available either.
 *
 * Fixed instead by giving decomp's own (3-arg, real-usage-shaped) call
 * a UNIQUE compiled name that can never collide with Aurora's real
 * (5-arg) GXSetArray at all: every decomp call site AND the shadow
 * header's own declaration are renamed, at the preprocessor level, to
 * mp6_GXSetArray3 -- platform/gx/aurora_bridge.c (aurora build) and
 * gen_shims.py's MACRO_RESOLUTION (--headless build's null shim) each
 * provide the actual mp6_GXSetArray3(GXAttr, void*, u8) definition; the
 * name `GXSetArray` itself is never defined by anything OUR code
 * controls, so Aurora's real, differently-shaped GXSetArray is free to
 * exist in the same link with zero conflict (nothing calls it directly
 * by that name -- aurora_bridge.c's mp6_GXSetArray3 calls it explicitly,
 * by name, from a TU that never sees this rename -- see that file). */
#define GXSetArray mp6_GXSetArray3

/* ---------------------------------------------------------------------
 * 4. include/dolphin/gx/GXGeometry.h -- GXBegin/GXEnd balance diagnostic.
 *
 * A PURE rename, same mechanism as the PSMTX-/PSVEC-family wrappers in
 * platform/gx/aurora_bridge.c (NOT an arity bridge like mp6_GXSetArray3
 * above -- decomp's and Aurora's GXBegin/GXEnd signatures match exactly).
 * Every decomp call site is redirected to mp6_GXBegin/mp6_GXEnd so
 * platform/gx/aurora_bridge.c can track open/close balance and log
 * __builtin_return_address(0) the moment an imbalance is detected --
 * the fastest path to root-causing "GXBegin: called without matching
 * GXEnd" (an Aurora-level FATAL). aurora_bridge.c's own wrapper calls the
 * REAL GXBegin/GXEnd afterward (visible there since that file is compiled
 * against Aurora's OWN headers, never this one, so this #define can't
 * touch its own definitions -- same reasoning as mp6_GXSetArray3's own
 * comment above), so behavior is unchanged apart from the diagnostic;
 * gen_shims.py's MACRO_RESOLUTION/AURORA_HAND_BRIDGED cover the
 * --headless build's own null shim under the same resolved names,
 * matching mp6_GXSetArray3's own precedent. */
#define GXBegin mp6_GXBegin
#define GXEnd mp6_GXEnd

#ifdef __cplusplus
}
#endif

#endif /* MP6_DOLPHIN_COMPAT_H */
