/* MP6 native port -- Unlocked FPS frame interpolation engine.
 *
 * include/mp6_unlocked_fps.h carries the full design contract; this
 * file is the mechanism. Like aurora_bridge.c, it compiles against
 * AURORA's OWN headers (tools/build.py AURORA_FLAGS -- never the decomp
 * include tree) and only for the windowed build (PLATFORM_AURORA_ONLY).
 *
 * Three parts:
 *   1. CAPTURE -- a drain-capture sink (aurora-patches/0015) appends every
 *      fifo chunk of the current tick's frame into a retained stream;
 *      VIWaitForRetrace's frame boundaries arm/seal it. Two streams (N-1,
 *      N) are retained, rotating in place (grow-only buffers: steady-state
 *      allocation, leakgate-clean).
 *   2. WALK -- a command-stream walker computing every command's length
 *      exactly like aurora's process()/dl::Reader::next() (never a byte
 *      scan: matrix loads share opcode 0x10 with every other XF write, and
 *      0x10 bytes appear freely inside vertex payloads). Draw strides come
 *      from tracked CP state (VCD 0x50/0x60, VAT 0x70-0x97), seeded at
 *      each frame-begin from aurora's live state (patch 0015's
 *      aurora_gx_export_vtx_layout) so mid-session enables are correct
 *      even for vertex formats configured long before capture started.
 *      The walk both fingerprints a stream (pos-matrix-load count +
 *      offsets) and validates it; any surprise fails the stream loudly
 *      into "no replay this window", never a guess.
 *   3. REPLAY -- inside the tick throttle's idle window, build a rewritten
 *      copy of stream N (identity-paired pos matrices advanced along N-1 -> N
 *      at t=1+alpha, nrm matrices recomputed as inverse-transpose,
 *      side-effect commands skip-filtered -- the header's full list) and
 *      present it as one extra aurora begin/submit/end cycle. aurora's
 *      2-slot render worker backpressures this against the display's own
 *      vsync cadence, so no timing code exists here beyond alpha and a
 *      window-fit check. Replay admission itself is non-blocking.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <aurora/aurora.h>
#include <SDL3/SDL.h> /* SDL_PumpEvents in the replay cycle (see mp6_fi_idle_present) */

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXAurora.h>

#include "mp6_unlocked_fps.h"
#include "mp6_fi_model.h" /* model generations, draw context, camera-cut history */
#include "mp6_fi_timing.h"
#include "mp6_ambient_occlusion.h"
#include "mp6_display.h"
#include "mp6_console.h"    /* the fi_diag runtime lever (env latches, console overrides) */
#include "mp6_diag_probe.h" /* mp6_fi_stats_get -- the census this file already keeps */
#include "mp6_enhancements.h" /* mp6_enh_unlocked_fps -- the switch's one front door */
#include "host.h" /* mp6_host_monotonic_ns -- same clock as the tick throttle */

/* Diagnostics only (MP6_FI_DIAG>=3): the VI tick counter, so a diag line can
 * be joined 1:1 against a MP6_FRAME_DUMP index.csv row. FiStream::tick is a
 * SEAL ordinal, which is not that counter. */
extern long mp6_tick_count;

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md). This TU's retained-stream statics
 * -- s_streams[].data/.chunkEnd/.posOff and s_replayBuf -- are realloc-managed
 * HOST pointers. The savestate image sweep would otherwise capture and restore
 * their VALUES (not their heap allocations): loading a state with Unlocked FPS
 * active reinstates a stale/freed pointer, and the next fi_grow()/realloc() or
 * fi_capture_sink() memcpy writes through it -> heap corruption. Carving the TU
 * excludes every file-scope static here from both capture and restore (the
 * restoring process keeps its own live pointers), and mp6_fi_savestate_reset()
 * below additionally drops the now-stale retained motion on load. Registered in
 * tools/build.py HOST_STATE_SECTION_SOURCES; placed AFTER this file's own
 * includes exactly like src/gx/shadow_dump.c. */
#include "mp6_host_section.h"

/* --- aurora-patches/0015 surface (fifo.cpp; no aurora header change) --- */
extern void aurora_gx_set_drain_capture(void (*fn)(const void *data, uint32_t size, void *user), void *user);
extern void aurora_gx_submit_raw(const void *data, uint32_t size);
extern void aurora_gx_export_vtx_layout(uint8_t *vtxDescOut /*21*/, uint8_t *vatCntOut /*8x21*/,
                                        uint8_t *vatTypeOut /*8x21*/);

/* --- launcher/bridge seams --- */
extern void mp6_launcher_frame_overlay(void);   /* launcher_core.cpp; replay frames draw the
                                                 * same ImGui/RmlUi overlay as real frames so
                                                 * the FPS counter / in-game menu don't strobe
                                                 * at the tick rate while presents run faster */

/* =======================================================================
 * Config / diagnostics state.
 * ======================================================================= */

#define FI_ATTR_COUNT 21   /* GX_VA_PNMTXIDX..GX_VA_TEX7 -- the stride-relevant attrs */
#define FI_MIN_SPACING_WAIT_NS 250000 /* 0.25ms -- under the host sleep granularity,
                                       * a spacing wait cannot be honoured anyway */

/* --- Per-pair interpolation gate (see fi_build_replay). ---------------------
 * A replayed frame advances each pos matrix FORWARD along the last tick's
 * motion (t = 1+alpha), so a pair is only safe to interpolate when that motion
 * is small enough that a one-tick forward overshoot is imperceptible; anything
 * faster is a spawn/despawn/cut/flip whose extrapolation smears, and it is
 * passed through at tick N verbatim (snapped) instead.
 *
 * Thresholds are read straight off the MP6_FI_DIAG per-pair delta histograms
 * (mode-select / file-select / party setup / party character-select, 60Hz):
 *   translation -- smoothly-moving objects cluster under ~20u/tick; the only
 *     larger values are screen slides/cuts (whole-scene 50-200u for 1-2 ticks)
 *     and object teleports (200-1900u). 50u sits in the empty gap above real
 *     motion and below every cut, so real motion still interpolates and the
 *     overshoot-prone band snaps.
 *   rotation -- smooth spins stay under ~5 deg/tick; character-select's portrait
 *     flip-in drives up to 14 portraits at 45-120 deg/tick at once (the reported
 *     "flickering all over the place"). 10 deg cleanly separates the two: the old
 *     120 deg guard (qdot>0.5) let every flip extrapolate and overshoot.
 * The old guard was 200u / qdot>0.5 (=120 deg) -- far too loose for both.
 *
 * SCALE IS THE THIRD CHANNEL AND IT WAS UNGATED. Translation and rotation each
 * had a gate from the start; scale had neither a gate nor a diagnostic, so a
 * pop-in whose motion is mostly a scale change slid through both tests and
 * extrapolated at full strength. The w01 dice bloom is the reference case
 * (`src/board/dice.c` DiceObjOMExec case 2): `scale = HuSin(time*180)` over
 * `maxTime = 12` ticks drives 1.0 -> 2.0x and back, so
 *
 *     tick 0 -> 1   s = 1.0000 -> 1.2588   (+25.9%),  y hop +38.8u
 *     tick 11 -> 12 s = 1.2588 -> 1.0000   (-20.6%),  y hop -38.8u
 *
 * Both Y hops sit UNDER FI_TRANS_SNAP_U (50u) and the model barely rotates, so
 * every replay in those windows interpolated -- and because a replay evaluates
 * at t = 1+alpha, the last tick of the bloom is extrapolated PAST the end of an
 * animation that has already stopped: the dice is drawn down to ~0.74x scale
 * and ~39u below its resting height on a frame whose neighbours both show it at
 * rest. That is an A-B-A pop with no counterpart in the motion the gates test.
 *
 * The scale gate is therefore NOT a whole-matrix snap. It holds the SCALE
 * CHANNEL at tick N's value (per-channel alphaS = 0) while translation and
 * rotation keep advancing, because those two are separately gated already and
 * snapping them as well would give this object the tick rate for no measured
 * reason -- the F3 pan defect in reverse (docs/history/F3_LEAF_STROBE.md
 * section 7: an object pinned to tick N while the scene advances is itself the
 * artifact). All three columns are held together, not just the offending one:
 * the dice grows in x/z while it squashes in y, so holding one column and
 * advancing the others would distort the shape instead of freezing it.
 *
 * CAMERA-SAFE BY PROOF, not by measurement. Every pos matrix is a modelview,
 * camera x model, and the Hu3D view matrix is rigid (PSMTXLookAt: orthonormal
 * basis + translation). A rigid left-multiply preserves column LENGTHS, so the
 * decomposed scale of a modelview is exactly the model's own scale and a camera
 * move cannot move this ratio at all -- unlike the translation gate, which sees
 * the pan. Were a view matrix ever non-rigid the gate would only over-trigger,
 * and over-triggering costs one channel's smoothness for one window; it can
 * never displace geometry (O(0) == B still holds exactly, since alphaS = 0
 * changes nothing at alpha = 0).
 *
 * The ratio is measured symmetrically -- max_i max(sb_i/sa_i, sa_i/sb_i) - 1,
 * so a 2x grow and a 2x shrink both read 1.0.
 *
 * THE THRESHOLD IS CALIBRATED, from the maxScale meter this change also added
 * (17,968 replay builds over one boot->board drive, plus a 900-present frame
 * dump of the party-setup board-select confirm; full table in
 * docs/history/F3_LEAF_STROBE.md section 8). Two facts set it, both from the
 * SAME fifteen ticks so they are directly comparable:
 *   - the smooth population tops out at 0.029. Almost every animated scale in
 *     the game is a LINEAR ramp, so its ratio is 1/k where k is how many ticks
 *     of growth remain; a slow ramp (model 72, k~60) reads 0.016-0.017 and
 *     nothing measured anywhere on the route exceeded 0.029 without being a
 *     pop.
 *   - the measured ARTIFACT starts at 0.0345. Model 21's linear shrink-to-death
 *     ran 0.0345 -> 0.0500 over ticks 3133..3142 with maxTrans pinned at 28.3u
 *     (under the 50u gate) and minQdot at 0.99999 (far above the rotation
 *     gate), and the interpolated present at 0.0500 tripped the A-B-A detector
 *     at 32.4 MAD d(prev) against 0.73 d(span) -- i.e. both existing gates
 *     passed it and the frame was visibly wrong.
 * 0.03 is between them. The gap is narrow (0.029 vs 0.0345) and that is
 * acceptable because the two error directions are not comparable: holding a
 * smooth ramp costs one tick of size staleness on a camera-independent
 * quantity -- under 3% of the object, invisible, and it cannot tear against
 * anything because no pan feeds it -- while passing a pop costs a 32-MAD
 * one-frame flash. When in doubt, hold. */
#define FI_TRANS_SNAP_U   50.0    /* |delta translation| >= this -> snap the pair */
#define FI_ROT_SNAP_QDOT  0.99619 /* |quat dot| <= this (>~10 deg/tick) -> snap the pair */
#define FI_SCALE_SNAP_RATIO 0.03  /* worst per-column scale ratio-1 >= this ->
                                   * hold the scale channel (translation and
                                   * rotation still advance) */

static int s_diag = -1; /* MP6_FI_DIAG level: 1 = periodic capture/walk/replay
                         * diagnostics + chunk proof; 2 adds one QPC-stamped
                         * line per sealed tick (mono_ns is the same
                         * QueryPerformanceCounter domain PowerShell's
                         * Stopwatch reads, so an external capture tool can
                         * assign its grabs to exact tick windows -- the
                         * visual same-tick-interpolation gate uses this) */

static int fi_diag(void)
{
    if (s_diag < 0) {
        const char *env = getenv("MP6_FI_DIAG");
        s_diag = (env != NULL && *env != '\0') ? atoi(env) : 0;
        if (s_diag < 0) s_diag = 0;
    }
    /* The env value stays the latched baseline; the dev console may override it
     * live (`set fi_diag 3`), and mp6_console_cvar_get records the env value on
     * the way through so the toggles page can show both and name the winner.
     * With no console and no override this is an array index and a branch. */
    return mp6_console_cvar_get(MP6_CVAR_FI_DIAG, s_diag);
}

/* MP6_FI_NO_INTERP=1 -- diagnosis bisect only. Keeps the replay CADENCE
 * (extra presents in the idle window) but submits the captured stream
 * VERBATIM: no pos-matrix pairing, no forward extrapolation, no normal-matrix
 * recompute. Anything still visible on a replay frame with this set is NOT the
 * matrix rewrite; it is the replay itself (skip-filtered commands, dropped
 * offscreen/EFB-copy brackets, or state the resubmitted bytes do not carry). */
static int fi_no_interp(void)
{
    static int s_val = -1;
    if (s_val < 0) {
        const char *env = getenv("MP6_FI_NO_INTERP");
        s_val = (env != NULL && *env != '\0' && *env != '0') ? 1 : 0;
    }
    return s_val;
}

/* MP6_FI_NO_RESIDUAL=1 -- bisect/measurement lever only. Drops the residual
 * carry at the pos-matrix rewrite (see fi_build_replay), restoring the pure
 * TRS decompose/recompose round trip. That REINSTATES the shear-discard defect
 * -- mode select's right bridge re-poses on every interpolated present -- so it
 * exists purely so the fix can be A/B'd (visually or for cost) inside one
 * binary. Never set it in a real run. */
static int fi_no_residual(void)
{
    static int s_val = -1;
    if (s_val < 0) {
        const char *env = getenv("MP6_FI_NO_RESIDUAL");
        s_val = (env != NULL && *env != '\0' && *env != '0') ? 1 : 0;
    }
    return s_val;
}

/* MP6_FI_NO_SCALE_HOLD=1 -- A/B lever for the scale gate (see
 * FI_SCALE_SNAP_RATIO). Restores the pre-gate behaviour -- the scale channel
 * extrapolates at t = 1+alpha like translation and rotation -- so the same
 * binary can be re-gated both ways against one frame-dump burst. With it set,
 * a bloom/pop tick is expected to be LOUD on the A-B-A detector; that is the
 * measurement, not a regression. */
static int fi_no_scale_hold(void)
{
    static int s_val = -1;
    if (s_val < 0) {
        const char *env = getenv("MP6_FI_NO_SCALE_HOLD");
        s_val = (env != NULL && *env != '\0' && *env != '0') ? 1 : 0;
    }
    return s_val;
}

/* MP6_FI_SCALE_SNAP=<ratio> -- threshold override for the same gate, so the
 * calibration sweep does not need a rebuild per candidate. Parsed once; a
 * value that is not a finite positive number leaves the compiled default in
 * place (a typo must not silently disable a gate -- MP6_FI_NO_SCALE_HOLD is
 * the way to turn it off, and it says so). */
static double fi_scale_snap_ratio(void)
{
    static double s_val = -1.0;
    if (s_val < 0.0) {
        const char *env = getenv("MP6_FI_SCALE_SNAP");
        double v = (env != NULL && *env != '\0') ? atof(env) : 0.0;
        s_val = (isfinite(v) && v > 0.0) ? v : FI_SCALE_SNAP_RATIO;
    }
    return s_val;
}

/* MP6_FI_MARGIN_DIV -- pacing A/B lever for the admission margin (period/N).
 * Clamped to a sane band so a typo cannot disable admission or remove the pad
 * entirely. See the margin computation in mp6_fi_idle_present. */
static int fi_margin_div(void)
{
    static int s_val = -1;
    if (s_val < 0) {
        const char *env = getenv("MP6_FI_MARGIN_DIV");
        s_val = (env != NULL && *env != '\0') ? atoi(env) : 32;
        if (s_val < 2) s_val = 2;
        if (s_val > 512) s_val = 512;
    }
    return s_val;
}

/* THE ARMING DECISION for the whole file -- read live, every tick, by
 * mp6_fi_idle_present and the capture arming below.
 *
 * Two sources, in this order:
 *
 *   1. MP6_UNLOCKED_FPS -- the pre-existing per-feature lever. Unchanged, and
 *      it still wins outright in both directions (0 disables, anything else
 *      enables), which is the priority docs/SETTINGS.md promises the legacy
 *      levers keep "at their own consumption sites". Latched once: the
 *      environment cannot change under a running process.
 *
 *   2. the ENHANCEMENTS SEAM (include/mp6_enhancements.h), which is the
 *      single front door for this switch and resolves, in its own documented
 *      order, MP6_ENH_UNLOCKED_FPS -> MP6_ENH_PRESET -> the value the
 *      launcher published from mp6_config.json -> RETAIL.
 *
 * Step 2 replaces a direct mp6_launcher_cfg_unlocked_fps() read. That read
 * was the config and ONLY the config, which is why the switch was declared
 * LIVE in the seam header while no engine consumer could actually see either
 * of its two levers: MP6_ENH_UNLOCKED_FPS and MP6_ENH_PRESET=vanilla-plus
 * both resolved correctly inside the seam and were then thrown away here.
 *
 * The automation contract is unchanged and is preserved BY CONSTRUCTION, not
 * by a branch: automation mode never calls mp6_enh_set_values(), so the
 * seam's store is still at its retail initializer and this returns 0 -- the
 * same 0 mp6_launcher_cfg_unlocked_fps() hard-coded off the launcher path.
 * In launcher mode the seam's published store IS g_cfg's six values
 * (mp6_launcher_publish_enh(), called on config load and from every
 * cfg_save()), so the Mods/Enhancements toggle still applies within a tick. */
int mp6_unlocked_fps_enabled(void)
{
    static int s_envState = -2; /* -2 unparsed; -1 unset; 0 forced off; 1 forced on */
    if (s_envState == -2) {
        const char *env = getenv("MP6_UNLOCKED_FPS");
        if (env == NULL || *env == '\0') {
            s_envState = -1;
        } else {
            s_envState = (*env != '0') ? 1 : 0;
        }
    }
    if (s_envState >= 0) {
        return s_envState; /* legacy env lever set: it wins outright */
    }
    return mp6_enh_unlocked_fps() != 0;
}

/* =======================================================================
 * Retained streams.
 * ======================================================================= */

/* Pairing identity for one GXLoadPosMtxImm.
 *
 * (camera, model, generation) is the identity GROUP -- one model instance under
 * one camera, across its whole frame.  Inside the group a matrix is named by
 * (sub, ordinal):
 *   sub     -- WHICH EMISSION BRACKET.  0xFFFF is the immediate pass (the
 *              matrices Hu3DDraw emits while the model bracket is open); any
 *              other value is a deferred draw object's PUSH-ORDER rank within
 *              that model, reserved by mp6_fi_capture_defer_push() while the
 *              bracket was still open and re-installed by Hu3DDrawPost.  This
 *              is what makes pairing immune to the deferred pass's DEPTH sort:
 *              the sort permutes emission order every time the camera moves,
 *              but never the push order the rank was taken from.
 *   ordinal -- emission index inside that bracket. */
typedef struct {
    int16_t model;
    int8_t camera;
    uint8_t valid;
    uint32_t generation;
    uint16_t ordinal;
    uint16_t sub;
} FiPosKey;

#define FI_AO_MAX 96 /* 16 cameras: decal pair, foliage pair, composite */
typedef struct {
    uint32_t offset;
    int kind; /* 0 composite, 1/2 board decals, 3/4 foliage target */
    int camera;
    Mp6AoView view;
} FiAoMarker;

typedef struct FiCmd FiCmd;
typedef struct FiPreparedPair FiPreparedPair;

typedef struct {
    /* AO is a host GPU pass, not a GX opcode. Its exact drain boundary must
     * survive command filtering or replay would either omit it or shade HUD. */
    FiAoMarker ao[FI_AO_MAX];
    uint32_t aoCount;
    /* raw captured bytes (all drain chunks of one tick's frame, in order) */
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
    /* chunk framing (for the drain-chunk boundary proof) */
    uint32_t *chunkEnd; /* end offset of each captured chunk */
    uint32_t chunkCount;
    uint32_t chunkCap;
    /* walker seed state, snapshotted at frame-begin (stream start) */
    uint8_t seedDesc[FI_ATTR_COUNT];
    uint8_t seedCnt[8 * FI_ATTR_COUNT];
    uint8_t seedType[8 * FI_ATTR_COUNT];
    /* walk results (valid only when walked != 0 && walkOk != 0) */
    int walked;
    int walkOk;
    uint32_t posCount;   /* fingerprint: number of GXLoadPosMtxImm commands */
    uint32_t *posOff;    /* offset of each pos-load's 48-byte float payload */
    uint32_t posCap;
    FiPosKey *posKey;     /* same order/count as posOff; captured at GX call time */
    uint32_t keyCount;
    uint32_t keyCap;
    int32_t *prevPos;     /* current pos index -> matching identity in N-1, or -1 */
    uint32_t prevPosCap;
    /* Immutable after a successful seal walk; offsets, never source pointers.
     * Replays reuse framing, while matrix values and live gates stay dynamic. */
    FiCmd *commands;
    uint32_t commandCount;
    uint32_t commandCap;
    int sealed;
    long tick;           /* seal ordinal (diagnostics) */
} FiStream;

static FiStream s_streams[2];
/* Only the latest sealed window can replay. Cache its immutable endpoints,
 * lazily, with a bounded allocation; overflow/allocation failure uses the
 * original calculation. The state bytes avoid clearing the large records. */
#define FI_PAIR_CACHE_MAX 4096u
static FiPreparedPair *s_pairCache;
static uint8_t *s_pairState;
static uint32_t s_pairCacheCap, s_pairStateCap, s_pairCacheCount;
static const FiStream *s_pairCacheCur, *s_pairCachePrev;

static void fi_pair_cache_invalidate(void)
{
    s_pairCacheCur = s_pairCachePrev = NULL;
    s_pairCacheCount = 0;
}
static FiAoMarker s_replayAo[FI_AO_MAX];
static uint32_t s_replayAoCount;
static int s_cur = -1;            /* index being captured into; -1 = not armed */
static int s_latest = -1;         /* sealed stream N */
static int s_prev = -1;           /* sealed stream N-1 */
static int s_active;              /* capture sink registered */
static int s_inReplay;            /* reentrancy: ignore our own replay drain */
static long s_sealCounter;
static int64_t s_lastSealNs;      /* present timestamp of stream N (alpha reference) */
static int s_replaysThisWindow;
static int64_t s_lastPresentNs;   /* end of the last present (real or replay) -- spacing base */
static long s_statReplays, s_statSnaps, s_statSkippedWindows;
static int64_t s_replayBudgetNs;  /* measured CPU cost after spacing, including try-admission */

/* --- Decline census ------------------------------------------------------
 * Every early return in mp6_fi_idle_present is a REASON this window produced
 * no interpolated present, and the reasons are not interchangeable: "the
 * stream failed the walk" is a builder defect, "the deadline never fits" is
 * the tick having no idle time left to give, and "the cap is reached" is the
 * feature working. Without the split, a present rate pinned at the tick rate
 * is unattributable -- which is exactly the state the w01 board report started
 * from. Counted unconditionally (one increment per call, all paths); printed
 * only under MP6_FI_DIAG. */
enum {
    FI_DECL_INACTIVE = 0,    /* feature off, or no sealed stream retained yet */
    FI_DECL_NOSTREAM,        /* stream N unsealed, walk-failed, or empty */
    FI_DECL_CAP,             /* FI_MAX_REPLAYS_PER_WINDOW already reached */
    FI_DECL_DEADLINE_ENTRY,  /* no room at the window's first admission check */
    FI_DECL_DEADLINE_SPACE,  /* the spacing slot itself would land past the deadline */
    FI_DECL_DEADLINE_SLEPT,  /* the spacing sleep overshot the remaining room */
    FI_DECL_BUILD,           /* fi_build_replay returned 0 */
    FI_DECL_DEADLINE_BUILT,  /* the rewrite consumed the remaining room */
    FI_DECL_DEADLINE_PUMPED, /* SDL_PumpEvents consumed the remaining room */
    FI_DECL_BEGINFRAME,      /* aurora refused a non-blocking frame slot */
    FI_DECL_OK,              /* presented */
    FI_DECL_COUNT
};
static const char *const kFiDeclNames[FI_DECL_COUNT] = {
    "inactive", "nostream", "cap", "dl-entry", "dl-space", "dl-slept",
    "build", "dl-built", "dl-pumped", "beginframe", "ok"
};
static long s_declCount[FI_DECL_COUNT];
static long s_declWindowBase[FI_DECL_COUNT];
/* Slack seen at the entry admission check, i.e. how much of the tick period
 * was still unspent when the throttle first offered this window to replay.
 * Separates "the deadline test is too strict" from "there is no idle time". */
static int64_t s_declSlackSumNs, s_declSlackMaxNs, s_declSlackMinNs;
static long s_declSlackSamples;
static long s_censusSeals, s_censusSealWalkFail, s_censusSealNoPrev;
/* What a replay's wall cost is actually made of. The admission budget is one
 * number, but the two halves have different owners: BUILD is this file's own
 * rewrite (walk + copy + matrix maths -- port-side, optimisable), PRESENT is
 * aurora re-processing the stream and submitting it, which can also BLOCK on
 * the render worker. A feature that is capacity-limited by the second is a
 * different problem from one limited by the first. */
static int64_t s_costBuildSumNs, s_costBuildMaxNs, s_costBuildLastNs;
static int64_t s_costPresentSumNs, s_costPresentMaxNs;
static long s_costSamples;

static void fi_cost_observe(int64_t buildNs, int64_t totalNs)
{
    int64_t presentNs = (totalNs > buildNs) ? totalNs - buildNs : 0;
    s_costBuildSumNs += buildNs;
    s_costPresentSumNs += presentNs;
    if (buildNs > s_costBuildMaxNs) s_costBuildMaxNs = buildNs;
    if (presentNs > s_costPresentMaxNs) s_costPresentMaxNs = presentNs;
    s_costSamples++;
}

static int fi_decline(int reason)
{
    s_declCount[reason]++;
    return 0;
}

static void mp6_fi_census_log(long tick); /* defined with the frame-boundary hooks */

static void fi_budget_observe(int64_t costNs)
{
    if (costNs > s_replayBudgetNs) {
        s_replayBudgetNs = costNs;
    } else {
        s_replayBudgetNs -= (s_replayBudgetNs - costNs) / 4;
    }
}

/* What of this replay's measured cost is still OWED at `now`.
 *
 * s_replayBudgetNs is the cost of a whole replay measured from t0, so the
 * re-checks that run partway through one (after the rewrite, after the event
 * pump) must not demand it again in full: the work already done since t0 has
 * been paid, and only the remainder still has to fit before the deadline.
 * Demanding the whole budget at every stage cost the board its replays twice
 * over -- it built the frame (~0.8ms of real work) and then threw it away
 * because it re-reserved room for that same 0.8ms. MP6_FI_DIAG's census named
 * it outright: dl-built was the largest decline reason once the spacing and
 * spin defects were out of the way. */
static int64_t fi_budget_remaining(int64_t t0, int64_t now)
{
    int64_t spent = (now > t0) ? now - t0 : 0;
    return (s_replayBudgetNs > spent) ? s_replayBudgetNs - spent : 0;
}

static void fi_budget_decay(void)
{
    s_replayBudgetNs -= s_replayBudgetNs / 8;
    if (s_replayBudgetNs < 0) s_replayBudgetNs = 0;
}

static void fi_stream_reset(FiStream *s)
{
    fi_pair_cache_invalidate();
    s->aoCount = 0;
    s->size = 0;
    s->chunkCount = 0;
    s->walked = 0;
    s->walkOk = 0;
    s->commandCount = 0;
    s->posCount = 0;
    s->keyCount = 0;
    s->sealed = 0;
}

/* A real frame whose retained capture failed must invalidate the older
 * replay window immediately. Otherwise the next idle period can present N-1
 * over the newer real frame N merely because one grow allocation failed. */
static void fi_capture_fail(void)
{
    s_cur = -1;
    s_latest = -1;
    s_prev = -1;
    s_replaysThisWindow = 0;
    s_replayBudgetNs = 0;
}

static int fi_grow(void **buf, uint32_t *cap, uint32_t need, size_t elem)
{
    uint32_t newCap;
    void *p;
    if (need <= *cap) return 1;
    newCap = (*cap == 0) ? 256 : *cap;
    if (elem != 0 && (uint64_t)need > (uint64_t)SIZE_MAX / elem) return 0;
    while (newCap < need) {
        if (newCap > UINT32_MAX / 2u) {
            newCap = need;
            break;
        }
        newCap *= 2u;
    }
    p = realloc(*buf, (size_t)newCap * elem);
    if (p == NULL) return 0;
    *buf = p;
    *cap = newCap;
    return 1;
}

static void fi_capture_sink(const void *data, uint32_t size, void *user)
{
    FiStream *s;
    (void)user;
    if (s_inReplay || s_cur < 0 || size == 0) {
        return; /* replay's own drain, or not armed (e.g. pre-first-begin boot chunks) */
    }
    s = &s_streams[s_cur];
    if (size > UINT32_MAX - s->size || s->chunkCount == UINT32_MAX ||
        !fi_grow((void **)&s->data, &s->cap, s->size + size, 1) ||
        !fi_grow((void **)&s->chunkEnd, &s->chunkCap, s->chunkCount + 1, sizeof(uint32_t))) {
        fi_capture_fail(); /* drop this frame and every older replay candidate */
        return;
    }
    memcpy(s->data + s->size, data, size);
    s->size += size;
    s->chunkEnd[s->chunkCount++] = s->size;
}

void mp6_fi_note_ao(const Mp6AoView *view)
{
    FiStream *s;
    if (s_inReplay || s_cur < 0 || view == NULL) return;
    s = &s_streams[s_cur];
    if (s->aoCount == FI_AO_MAX) {
        fi_capture_fail(); /* never replay a partial set of scene passes */
        return;
    }
    s->ao[s->aoCount].offset = s->size;
    s->ao[s->aoCount].kind = 0;
    s->ao[s->aoCount++].view = *view;
}

void mp6_fi_note_ao_decals(int camera, int after)
{
    FiStream *s;
    FiAoMarker *marker;
    if (s_inReplay || s_cur < 0) return;
    s = &s_streams[s_cur];
    if (s->aoCount == FI_AO_MAX) { fi_capture_fail(); return; }
    marker = &s->ao[s->aoCount++];
    memset(marker, 0, sizeof(*marker));
    marker->offset = s->size;
    marker->kind = after ? 2 : 1;
    marker->camera = camera;
}

void mp6_fi_note_ao_foliage(int camera, int after)
{
    FiStream *s;
    FiAoMarker *marker;
    if (s_inReplay || s_cur<0) return;
    s=&s_streams[s_cur];
    if (s->aoCount==FI_AO_MAX) { fi_capture_fail(); return; }
    marker=&s->ao[s->aoCount++];
    memset(marker,0,sizeof(*marker));
    marker->offset=s->size; marker->camera=camera; marker->kind=after?4:3;
}

void mp6_fi_stream_note_pos_mtx(void)
{
    FiStream *s;
    FiPosKey key;
    int camera = -1, model = -1;
    uint32_t generation = 0;
    uint16_t ordinal = 0;
    uint16_t sub = 0xFFFFu;

    if (s_inReplay || s_cur < 0) return;
    s = &s_streams[s_cur];
    memset(&key, 0, sizeof(key));
    key.model = -1;
    /* The camera is recorded even when the model is not: an unidentified
     * matrix inside a 3D camera bracket still moves with a pan, so the pairing
     * diagnostics must be able to separate it from a screen-space one. */
    key.camera = (int8_t)mp6_fi_capture_camera_id();
    key.sub = 0xFFFFu;
    if (mp6_fi_capture_context_next(&camera, &model, &generation, &sub, &ordinal)) {
        key.valid = 1;
        key.camera = (int8_t)camera;
        key.model = (int16_t)model;
        key.generation = generation;
        key.sub = sub;
        key.ordinal = ordinal;
    }
    /* Append invalid keys too: their positions preserve exact one-for-one
     * alignment with the walker.  They simply never pair/interpolate. */
    if (!fi_grow((void **)&s->posKey, &s->keyCap,
                 s->keyCount + 1, sizeof(FiPosKey))) {
        fi_capture_fail();
        return;
    }
    s->posKey[s->keyCount++] = key;
}

/* =======================================================================
 * Command-stream walker.
 * ======================================================================= */

typedef struct {
    uint8_t desc[FI_ATTR_COUNT];          /* GXAttrType per attr */
    uint8_t cnt[8][FI_ATTR_COUNT];        /* GXCompCnt per fmt/attr */
    uint8_t type[8][FI_ATTR_COUNT];       /* GXCompType per fmt/attr */
    int32_t stride[8];                    /* cached; -1 = recompute */
} FiVtxState;

static uint32_t rd_be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static float rd_bef32(const uint8_t *p)
{
    uint32_t u = rd_be32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void wr_bef32(uint8_t *p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)u;
}

/* comp_type_size * comp_cnt_count, transcribed from aurora lib/gx/attr_fmt.cpp
 * (the exact tables calculate_last_vtx_size() sums). Returns 0 for any
 * combination aurora itself would FATAL on -- walker fails the stream. */
static uint32_t fi_attr_bytes(uint32_t attr, uint32_t type, uint32_t cnt)
{
    uint32_t tsize, ccount;
    if (attr <= GX_VA_TEX7MTXIDX) return 1; /* matrix-index attrs: 1 byte flat */
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        switch (type) {
        case GX_RGB565: case GX_RGBA4: tsize = 2; break;
        case GX_RGB8:   case GX_RGBA6: tsize = 3; break;
        case GX_RGBX8:  case GX_RGBA8: tsize = 4; break;
        default: return 0;
        }
        return tsize; /* comp_cnt_count is 1 for colors */
    }
    switch (type) {
    case GX_U8: case GX_S8:   tsize = 1; break;
    case GX_U16: case GX_S16: tsize = 2; break;
    case GX_F32:              tsize = 4; break;
    default: return 0;
    }
    if (attr == GX_VA_POS) {
        ccount = (cnt == GX_POS_XY) ? 2 : (cnt == GX_POS_XYZ) ? 3 : 0;
    } else if (attr == GX_VA_NRM) {
        ccount = (cnt == GX_NRM_XYZ) ? 3 : (cnt == GX_NRM_NBT || cnt == GX_NRM_NBT3) ? 9 : 0;
    } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        ccount = (cnt == GX_TEX_S) ? 1 : (cnt == GX_TEX_ST) ? 2 : 0;
    } else {
        ccount = 0;
    }
    return ccount == 0 ? 0 : tsize * ccount;
}

/* Mirror of aurora's calculate_last_vtx_size() over the walker's shadow
 * state. 0 = invalid/unknown (stream fails). */
static int32_t fi_vtx_stride(FiVtxState *vs, uint32_t fmt)
{
    uint32_t attr, total = 0;
    if (fmt >= 8) return 0;
    if (vs->stride[fmt] >= 0) return vs->stride[fmt];
    for (attr = 0; attr < FI_ATTR_COUNT; ++attr) {
        uint32_t sz;
        switch (vs->desc[attr]) {
        case GX_NONE:
            continue;
        case GX_DIRECT:
            sz = fi_attr_bytes(attr, vs->type[fmt][attr], vs->cnt[fmt][attr]);
            if (sz == 0) return 0;
            total += sz;
            break;
        case GX_INDEX8:
            total += (attr == GX_VA_NRM && vs->cnt[fmt][attr] == GX_NRM_NBT3) ? 3 : 1;
            break;
        case GX_INDEX16:
            total += (attr == GX_VA_NRM && vs->cnt[fmt][attr] == GX_NRM_NBT3) ? 6 : 2;
            break;
        default:
            return 0;
        }
    }
    vs->stride[fmt] = (int32_t)total;
    return (int32_t)total;
}

static void fi_vtx_dirty(FiVtxState *vs, int fmt /* -1 = all */)
{
    int i;
    if (fmt >= 0) {
        vs->stride[fmt] = -1;
    } else {
        for (i = 0; i < 8; ++i) vs->stride[i] = -1;
    }
}

static uint32_t bits(uint32_t v, uint32_t size, uint32_t shift) { return (v >> shift) & ((1u << size) - 1u); }

/* CP register shadow -- the exact decode aurora's handle_cp() applies to
 * VCD lo/hi and VAT A/B/C (the only CP regs that affect vertex strides). */
static void fi_track_cp(FiVtxState *vs, uint8_t addr, uint32_t v)
{
    if (addr == 0x50) {
        vs->desc[GX_VA_PNMTXIDX] = (uint8_t)bits(v, 1, 0);
        vs->desc[GX_VA_TEX0MTXIDX] = (uint8_t)bits(v, 1, 1);
        vs->desc[GX_VA_TEX1MTXIDX] = (uint8_t)bits(v, 1, 2);
        vs->desc[GX_VA_TEX2MTXIDX] = (uint8_t)bits(v, 1, 3);
        vs->desc[GX_VA_TEX3MTXIDX] = (uint8_t)bits(v, 1, 4);
        vs->desc[GX_VA_TEX4MTXIDX] = (uint8_t)bits(v, 1, 5);
        vs->desc[GX_VA_TEX5MTXIDX] = (uint8_t)bits(v, 1, 6);
        vs->desc[GX_VA_TEX6MTXIDX] = (uint8_t)bits(v, 1, 7);
        vs->desc[GX_VA_TEX7MTXIDX] = (uint8_t)bits(v, 1, 8);
        vs->desc[GX_VA_POS] = (uint8_t)bits(v, 2, 9);
        vs->desc[GX_VA_NRM] = (uint8_t)bits(v, 2, 11);
        vs->desc[GX_VA_CLR0] = (uint8_t)bits(v, 2, 13);
        vs->desc[GX_VA_CLR1] = (uint8_t)bits(v, 2, 15);
        fi_vtx_dirty(vs, -1);
    } else if (addr == 0x60) {
        vs->desc[GX_VA_TEX0] = (uint8_t)bits(v, 2, 0);
        vs->desc[GX_VA_TEX1] = (uint8_t)bits(v, 2, 2);
        vs->desc[GX_VA_TEX2] = (uint8_t)bits(v, 2, 4);
        vs->desc[GX_VA_TEX3] = (uint8_t)bits(v, 2, 6);
        vs->desc[GX_VA_TEX4] = (uint8_t)bits(v, 2, 8);
        vs->desc[GX_VA_TEX5] = (uint8_t)bits(v, 2, 10);
        vs->desc[GX_VA_TEX6] = (uint8_t)bits(v, 2, 12);
        vs->desc[GX_VA_TEX7] = (uint8_t)bits(v, 2, 14);
        fi_vtx_dirty(vs, -1);
    } else if (addr >= 0x70 && addr <= 0x77) { /* VAT A */
        uint32_t fmt = addr - 0x70;
        uint32_t nrm_cnt = bits(v, 1, 9), nrm_nbt3 = bits(v, 1, 31);
        vs->cnt[fmt][GX_VA_POS] = (uint8_t)bits(v, 1, 0);
        vs->type[fmt][GX_VA_POS] = (uint8_t)bits(v, 3, 1);
        vs->cnt[fmt][GX_VA_NRM] =
            (uint8_t)(nrm_nbt3 ? GX_NRM_NBT3 : (nrm_cnt ? GX_NRM_NBT : GX_NRM_XYZ));
        vs->type[fmt][GX_VA_NRM] = (uint8_t)bits(v, 3, 10);
        vs->cnt[fmt][GX_VA_CLR0] = (uint8_t)bits(v, 1, 13);
        vs->type[fmt][GX_VA_CLR0] = (uint8_t)bits(v, 3, 14);
        vs->cnt[fmt][GX_VA_CLR1] = (uint8_t)bits(v, 1, 17);
        vs->type[fmt][GX_VA_CLR1] = (uint8_t)bits(v, 3, 18);
        vs->cnt[fmt][GX_VA_TEX0] = (uint8_t)bits(v, 1, 21);
        vs->type[fmt][GX_VA_TEX0] = (uint8_t)bits(v, 3, 22);
        fi_vtx_dirty(vs, (int)fmt);
    } else if (addr >= 0x80 && addr <= 0x87) { /* VAT B */
        uint32_t fmt = addr - 0x80;
        vs->cnt[fmt][GX_VA_TEX1] = (uint8_t)bits(v, 1, 0);
        vs->type[fmt][GX_VA_TEX1] = (uint8_t)bits(v, 3, 1);
        vs->cnt[fmt][GX_VA_TEX2] = (uint8_t)bits(v, 1, 9);
        vs->type[fmt][GX_VA_TEX2] = (uint8_t)bits(v, 3, 10);
        vs->cnt[fmt][GX_VA_TEX3] = (uint8_t)bits(v, 1, 18);
        vs->type[fmt][GX_VA_TEX3] = (uint8_t)bits(v, 3, 19);
        vs->cnt[fmt][GX_VA_TEX4] = (uint8_t)bits(v, 1, 27);
        vs->type[fmt][GX_VA_TEX4] = (uint8_t)bits(v, 3, 28);
        fi_vtx_dirty(vs, (int)fmt);
    } else if (addr >= 0x90 && addr <= 0x97) { /* VAT C */
        uint32_t fmt = addr - 0x90;
        vs->cnt[fmt][GX_VA_TEX5] = (uint8_t)bits(v, 1, 5);
        vs->type[fmt][GX_VA_TEX5] = (uint8_t)bits(v, 3, 6);
        vs->cnt[fmt][GX_VA_TEX6] = (uint8_t)bits(v, 1, 14);
        vs->type[fmt][GX_VA_TEX6] = (uint8_t)bits(v, 3, 15);
        vs->cnt[fmt][GX_VA_TEX7] = (uint8_t)bits(v, 1, 23);
        vs->type[fmt][GX_VA_TEX7] = (uint8_t)bits(v, 3, 24);
        fi_vtx_dirty(vs, (int)fmt);
    }
    /* 0x30/0x40 (matrix index), 0xA0-0xBF (array base/stride): no stride effect */
}

/* One walked command. */
struct FiCmd {
    uint32_t offset;
    uint32_t size;
    uint8_t op;        /* raw first byte */
    uint16_t auroraSub; /* GX_AURORA subcommand (op == GX_AURORA only) */
    uint8_t bpReg;     /* BP register id (op == 0x61 only) */
    uint32_t xfAddr;   /* XF destination addr (opcode 0x10 only) */
    uint32_t xfCount;  /* XF word count (opcode 0x10 only) */
};

/* Walk one command at data[pos]. Returns consumed size (>0) and fills cmd,
 * or 0 on any structural surprise (unknown opcode/subcommand, overrun,
 * unknown stride). Exactly mirrors process()'s framing. */
static uint32_t fi_walk_one(const uint8_t *data, uint32_t pos, uint32_t size, FiVtxState *vs, FiCmd *cmd)
{
    if (pos >= size) return 0;
    const uint8_t b = data[pos];
    const uint8_t opcode = b & 0xF8u; /* GX_OPCODE_MASK */
    uint32_t remain = size - pos;

    cmd->offset = pos;
    cmd->op = b;
    cmd->auroraSub = 0xFFFF;
    cmd->bpReg = 0xFF;
    cmd->xfAddr = 0xFFFFFFFFu;
    cmd->xfCount = 0;

    if (opcode == GX_NOP || opcode == (GX_CMD_INVL_VC & 0xF8u)) {
        /* process() masks first: any byte with opcode 0x00 is a NOP there */
        cmd->size = 1;
        return 1;
    }
    if (opcode == (GX_LOAD_BP_REG & 0xF8u)) { /* 0x61 -> 0x60 */
        if (remain < 5) return 0;
        cmd->bpReg = data[pos + 1];
        cmd->size = 5;
        return 5;
    }
    if (opcode == GX_LOAD_CP_REG) { /* 0x08 */
        if (remain < 6) return 0;
        fi_track_cp(vs, data[pos + 1], rd_be32(data + pos + 2));
        cmd->size = 6;
        return 6;
    }
    if (opcode == GX_LOAD_XF_REG) { /* 0x10: u32 header = (count-1)<<16 | addr */
        uint32_t header, count;
        if (remain < 5) return 0;
        header = rd_be32(data + pos + 1);
        count = ((header >> 16) & 0xFFFFu) + 1;
        cmd->xfAddr = header & 0xFFFFu;
        cmd->xfCount = count;
        cmd->size = 5 + count * 4;
        if (remain < cmd->size) return 0;
        return cmd->size;
    }
    if (opcode == GX_LOAD_INDX_A || opcode == GX_LOAD_INDX_B ||
        opcode == GX_LOAD_INDX_C || opcode == GX_LOAD_INDX_D) {
        if (remain < 5) return 0;
        cmd->size = 5;
        return 5;
    }
    if (opcode == GX_CMD_CALL_DL) { /* aurora ignores (GXCallDisplayList inlines) */
        if (remain < 9) return 0;
        cmd->size = 9;
        return 9;
    }
    if (opcode == GX_AURORA) { /* 0x50 + u16 subcommand */
        uint32_t sub, len;
        if (remain < 3) return 0;
        sub = rd_be16(data + pos + 1);
        cmd->auroraSub = (uint16_t)sub;
        switch (sub) {
        case GX_AURORA_LOAD_VIEWPORT_RENDER:   len = 24; break;
        case GX_AURORA_LOAD_SCISSOR_RENDER:    len = 16; break;
        case GX_AURORA_LOAD_PROJECTION_FULL:   len = 64; break;
        case GX_AURORA_LOAD_TEXOBJ:            len = 34; break;
        case GX_AURORA_LOAD_TLUT:              len = 23; break;
        case GX_AURORA_DESTROY_TEXOBJ:         len = 4;  break;
        case GX_AURORA_DESTROY_TLUT:           len = 4;  break;
        case GX_AURORA_DESTROY_COPY_TEX:       len = 8;  break;
        case GX_AURORA_DESTROY_FRAMEBUFFER_CACHE: len = 0; break;
        case GX_AURORA_REPLAY_COPY_CLEAR:      len = 1;  break;
        case GX_AURORA_LOAD_COPY_SRC:          len = 16; break;
        case GX_AURORA_LOAD_COPY_DST:          len = 13; break;
        case GX_AURORA_LOAD_COPY_DEST:         len = 8;  break;
        case GX_AURORA_REQUEST_DEPTH_SNAPSHOT: len = 0;  break;
        case GX_AURORA_BEGIN_OFFSCREEN:        len = 8;  break;
        case GX_AURORA_END_OFFSCREEN:          len = 0;  break;
        case GX_AURORA_SET_COPY_MIP_GEN:       len = 1;  break;
        case GX2_SET_POLYGON_OFFSET:           len = 20; break;
        case GX_AURORA_DEBUG_GROUP_POP:        len = 0;  break;
        case GX_AURORA_DEBUG_GROUP_PUSH:
        case GX_AURORA_DEBUG_MARKER_INSERT:
            if (remain < 5) return 0;
            len = 2 + rd_be16(data + pos + 3);
            break;
        case GX_AURORA_DRAW_SIZED: {
            uint32_t byteLen;
            if (remain < 8) return 0;
            byteLen = rd_be32(data + pos + 4);
            if (byteLen > remain - 8) return 0;
            len = 5 + byteLen; /* cmd u8 + byteLen u32 + vertex bytes */
            break;
        }
        case GX_AURORA_DRAW_INDEXED: {
            uint32_t vtxCount, indexCount;
            int32_t stride;
            if (remain < 10) return 0;
            vtxCount = rd_be16(data + pos + 4);
            indexCount = rd_be32(data + pos + 6);
            stride = fi_vtx_stride(vs, data[pos + 3] & 0x07u);
            if (stride <= 0) return 0;
            const uint64_t payload = (uint64_t)indexCount * 2 +
                                     (uint64_t)vtxCount * (uint32_t)stride;
            if (payload > remain - 10) return 0;
            len = 7 + (uint32_t)payload;
            break;
        }
        case GX_AURORA_LOAD_ARRAYBASE + 0x0: case GX_AURORA_LOAD_ARRAYBASE + 0x1:
        case GX_AURORA_LOAD_ARRAYBASE + 0x2: case GX_AURORA_LOAD_ARRAYBASE + 0x3:
        case GX_AURORA_LOAD_ARRAYBASE + 0x4: case GX_AURORA_LOAD_ARRAYBASE + 0x5:
        case GX_AURORA_LOAD_ARRAYBASE + 0x6: case GX_AURORA_LOAD_ARRAYBASE + 0x7:
        case GX_AURORA_LOAD_ARRAYBASE + 0x8: case GX_AURORA_LOAD_ARRAYBASE + 0x9:
        case GX_AURORA_LOAD_ARRAYBASE + 0xA: case GX_AURORA_LOAD_ARRAYBASE + 0xB:
        case GX_AURORA_LOAD_ARRAYBASE + 0xC: case GX_AURORA_LOAD_ARRAYBASE + 0xD:
        case GX_AURORA_LOAD_ARRAYBASE + 0xE: case GX_AURORA_LOAD_ARRAYBASE + 0xF:
            len = 13;
            break;
        default:
            return 0; /* unknown aurora subcommand: fail the stream */
        }
        cmd->size = 3 + len;
        if (remain < cmd->size) return 0;
        return cmd->size;
    }
    if (b >= 0x80) { /* draw: opcode|fmt, u16 vtxCount, verts */
        uint32_t vtxCount;
        int32_t stride;
        if (remain < 3) return 0;
        vtxCount = rd_be16(data + pos + 1);
        stride = fi_vtx_stride(vs, b & 0x07u);
        if (stride <= 0) return 0;
        cmd->size = 3 + vtxCount * (uint32_t)stride;
        if (remain < cmd->size) return 0;
        return cmd->size;
    }
    return 0; /* unknown opcode */
}

static void fi_seed_state(FiVtxState *vs, const FiStream *s)
{
    int fmt, attr;
    memcpy(vs->desc, s->seedDesc, sizeof(vs->desc));
    for (fmt = 0; fmt < 8; ++fmt) {
        for (attr = 0; attr < FI_ATTR_COUNT; ++attr) {
            vs->cnt[fmt][attr] = s->seedCnt[fmt * FI_ATTR_COUNT + attr];
            vs->type[fmt][attr] = s->seedType[fmt * FI_ATTR_COUNT + attr];
        }
    }
    fi_vtx_dirty(vs, -1);
}

/* Is this XF load a GXLoadPosMtxImm? (addr in the pos-matrix bank, exactly
 * the 12-float row-major 3x4 GXTransform.cpp writes). */
static int fi_is_pos_load(const FiCmd *c)
{
    return c->op == GX_LOAD_XF_REG && c->xfAddr <= 0x77u && c->xfCount == 12;
}
/* GXLoadNrmMtxImm: addr in the nrm bank, repacked 3x3 (9 floats). */
static int fi_is_nrm_load(const FiCmd *c)
{
    return c->op == GX_LOAD_XF_REG && c->xfAddr >= 0x400u && c->xfAddr <= 0x459u && c->xfCount == 9;
}

/* Full-stream walk: validates framing, collects the pos-load fingerprint.
 * Under MP6_FI_DIAG additionally proves/disproves that every drain-chunk
 * boundary falls exactly on a command boundary (the "one concatenated
 * process() call vs chunk-by-chunk" question -- see the final report). */
static uint32_t s_diagChunkAligned, s_diagChunkChecked, s_diagChunkMisaligned;

static void fi_walk_stream(FiStream *s)
{
    FiVtxState vs;
    uint32_t pos = 0, chunkIdx = 0, aoIdx = 0;
    fi_pair_cache_invalidate();
    s->walked = 1;
    s->walkOk = 0;
    s->posCount = 0;
    s->commandCount = 0;
    if (s->aoCount > FI_AO_MAX) return;
    fi_seed_state(&vs, s);
    while (pos < s->size) {
        FiCmd c;
        int markerBoundary = aoIdx < s->aoCount && s->ao[aoIdx].offset == pos;
        while (aoIdx < s->aoCount && s->ao[aoIdx].offset == pos) ++aoIdx;
        if (aoIdx < s->aoCount && s->ao[aoIdx].offset < pos) return;
        uint32_t n = fi_walk_one(s->data, pos, s->size, &vs, &c);
        if (n == 0) {
            if (fi_diag()) {
                fprintf(stderr, "[MP6-FI] walk FAIL tick=%ld at offset %u (op 0x%02X sub 0x%04X) size=%u\n",
                        s->tick, pos, s->data[pos], (unsigned)c.auroraSub, s->size);
            }
            return;
        }
        if (fi_is_pos_load(&c)) {
            if (!fi_grow((void **)&s->posOff, &s->posCap, s->posCount + 1, sizeof(uint32_t))) {
                return; /* allocation failure: stream stays walkOk=0 (no replay) */
            }
            s->posOff[s->posCount] = c.offset + 5; /* skip opcode + header -> float payload */
            s->posCount++;
        }
        /* A run of NOP bytes is one verbatim span. Never merge across an AO
         * boundary, even though the underlying bytes are otherwise inert. */
        if (!markerBoundary && c.op < 8 && s->commandCount &&
            s->commands[s->commandCount - 1].op < 8) {
            s->commands[s->commandCount - 1].size += n;
        } else {
            if (s->commandCount == UINT32_MAX ||
                !fi_grow((void **)&s->commands, &s->commandCap,
                         s->commandCount + 1, sizeof(FiCmd))) return;
            s->commands[s->commandCount++] = c;
        }
        pos += n;
        /* drain-chunk boundary proof: every chunk end must land exactly on a
         * command boundary for "concatenated == chunk-by-chunk" to hold. */
        while (chunkIdx < s->chunkCount && s->chunkEnd[chunkIdx] <= pos) {
            s_diagChunkChecked++;
            if (s->chunkEnd[chunkIdx] == pos) {
                s_diagChunkAligned++;
            } else {
                s_diagChunkMisaligned++;
                if (fi_diag()) {
                    fprintf(stderr, "[MP6-FI] CHUNK MISALIGNED tick=%ld chunk %u ends at %u inside command at %u..%u\n",
                            s->tick, chunkIdx, s->chunkEnd[chunkIdx], c.offset, pos);
                }
            }
            chunkIdx++;
        }
    }
    while (aoIdx < s->aoCount && s->ao[aoIdx].offset == pos) ++aoIdx;
    s->walkOk = (pos == s->size && s->posCount == s->keyCount && aoIdx == s->aoCount);
    if (!s->walkOk && fi_diag() && pos == s->size && s->posCount != s->keyCount) {
        fprintf(stderr, "[MP6-FI] identity/FIFO mismatch tick=%ld: gx-loads=%u stream-pos-loads=%u\n",
                s->tick, s->keyCount, s->posCount);
    }
}

static int fi_key_equal(const FiPosKey *a, const FiPosKey *b)
{
    return a->valid && b->valid && a->model == b->model &&
           a->camera == b->camera && a->generation == b->generation &&
           a->sub == b->sub && a->ordinal == b->ordinal;
}

/* Same identity GROUP: one model instance under one camera (ordinal ignored). */
static int fi_key_same_group(const FiPosKey *a, const FiPosKey *b)
{
    return a->valid && b->valid && a->model == b->model &&
           a->camera == b->camera && a->generation == b->generation;
}

/* --- Key census tables ---------------------------------------------------
 *
 * The census fi_pair_stream needs is, per matrix: how many keys share its
 * GROUP, and how many share its exact name -- on both sides -- plus, for a
 * reordered matrix, WHERE its twin sits in N-1.  Each of those used to be a
 * linear scan run once per matrix, so pairing cost grew as posCount squared
 * and the reordering search added a second such term (the deferred pass is
 * depth-sorted, so on a moving camera nearly every matrix takes it).
 *
 * That cost is not academic and it is not the game's: it is spent inside
 * mp6_fi_note_frame_end, on the tick's own clock, and it therefore comes
 * straight out of the idle window this feature exists to fill.  Measured on
 * the w01 board (~250 pos loads/tick, ~300KB streams): the whole seal ran
 * 1.07ms of a 16.67ms period against a 4.3ms idle window -- the feature was
 * spending a quarter of its own budget deciding what to do with it.
 *
 * Hashing the keys once per stream makes every one of those questions a
 * lookup, so pairing is linear.  Scratch only: rebuilt from scratch each
 * tick, grow-only like the retained streams, and carrying no meaning across
 * ticks (nothing here needs invalidating on a savestate load). */
typedef struct {
    FiPosKey *slot;   /* slot[i].valid == 0 marks an empty bucket */
    uint32_t *count;  /* keys hashing equal to this bucket's key */
    int32_t  *first;  /* lowest stream index carrying it */
    uint32_t  cap;    /* power of two, or 0 when unbuilt */
    int       ok;
} FiKeyTable;

static uint32_t fi_key_hash(const FiPosKey *k, int withName)
{
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)(uint16_t)k->model) * 16777619u;
    h = (h ^ (uint32_t)(uint8_t)k->camera) * 16777619u;
    h = (h ^ k->generation) * 16777619u;
    if (withName) {
        h = (h ^ k->sub) * 16777619u;
        h = (h ^ k->ordinal) * 16777619u;
    }
    return h;
}

/* Bucket equality mirrors fi_key_same_group / fi_key_equal exactly. */
static int fi_key_match(const FiPosKey *a, const FiPosKey *b, int withName)
{
    return withName ? fi_key_equal(a, b) : fi_key_same_group(a, b);
}

static uint32_t fi_table_probe(const FiKeyTable *t, const FiPosKey *k, int withName)
{
    uint32_t mask = t->cap - 1u;
    uint32_t i = fi_key_hash(k, withName) & mask;
    while (t->slot[i].valid && !fi_key_match(&t->slot[i], k, withName)) {
        i = (i + 1u) & mask;
    }
    return i;
}

/* Build the table over `keys[0..n)`. Invalid keys are never inserted: they
 * have no identity, so they belong to no group and name nothing. */
static void fi_table_build(FiKeyTable *t, const FiPosKey *keys, uint32_t n, int withName)
{
    uint32_t need = 64, j;
    t->ok = 0;
    while ((uint64_t)need < (uint64_t)n * 2u) {
        if (need > UINT32_MAX / 2u) return;
        need *= 2u;
    }
    if (need > t->cap) {
        uint32_t capKey = t->cap, capCount = t->cap, capFirst = t->cap;
        if (!fi_grow((void **)&t->slot, &capKey, need, sizeof(FiPosKey)) ||
            !fi_grow((void **)&t->count, &capCount, need, sizeof(uint32_t)) ||
            !fi_grow((void **)&t->first, &capFirst, need, sizeof(int32_t))) {
            return;
        }
        /* fi_grow rounds each independently; the table indexes all three with
         * one mask, so adopt the smallest common power of two it produced. */
        t->cap = capKey < capCount ? capKey : capCount;
        if (capFirst < t->cap) t->cap = capFirst;
    }
    memset(t->slot, 0, (size_t)t->cap * sizeof(FiPosKey));
    for (j = 0; j < n; ++j) {
        uint32_t at;
        if (!keys[j].valid) continue;
        at = fi_table_probe(t, &keys[j], withName);
        if (!t->slot[at].valid) {
            t->slot[at] = keys[j];
            t->count[at] = 1;
            t->first[at] = (int32_t)j;
        } else {
            t->count[at]++;
        }
    }
    t->ok = 1;
}

/* Count of keys matching `k`, and the lowest index carrying it (-1 if none). */
static uint32_t fi_table_lookup(const FiKeyTable *t, const FiPosKey *k, int withName,
                                int32_t *firstOut)
{
    uint32_t at;
    if (firstOut != NULL) *firstOut = -1;
    if (!t->ok || t->cap == 0) return 0;
    at = fi_table_probe(t, k, withName);
    if (!t->slot[at].valid) return 0;
    if (firstOut != NULL) *firstOut = t->first[at];
    return t->count[at];
}

/* Per-side scratch, reused every tick (grow-only). */
static FiKeyTable s_curGroupTab, s_curNameTab, s_prevGroupTab, s_prevNameTab;

/* The linear REFERENCE census the tables replaced: counts keys in
 * `keys[0..n)` belonging to key `k`'s group, and how many carry k's exact
 * (sub, ordinal) name. Retained deliberately -- under MP6_FI_DIAG>=3
 * fi_pair_stream runs it alongside the tables on every key of every tick and
 * prints any disagreement, so the optimisation has a live oracle rather than
 * an argument. */
static void fi_group_census(const FiPosKey *k, const FiPosKey *keys, uint32_t n,
                            uint32_t *groupCount, uint32_t *ordinalCount)
{
    uint32_t j, g = 0, o = 0;
    for (j = 0; j < n; ++j) {
        if (fi_key_same_group(k, &keys[j])) {
            g++;
            if (keys[j].sub == k->sub && keys[j].ordinal == k->ordinal) o++;
        }
    }
    *groupCount = g;
    *ordinalCount = o;
}

/* Build an identity map once per real tick.  The same-index fast path covers
 * normal frames; the fallback search preserves a model's pairing when draw
 * order changes.  Unknown/non-model matrices intentionally remain snapped.
 *
 * PAIRING RULE (see also include/mp6_unlocked_fps.h).  A matrix is named
 * by (camera, model, generation, sub, ordinal) and NEVER by its position in the
 * stream.  `sub` distinguishes the emission bracket -- the immediate in-model
 * pass from each deferred draw object's per-model PUSH-ORDER rank -- so the
 * deferred pass's depth sort, which re-permutes emission order on every camera
 * move, cannot shift what a key names.  Every pos load emitted inside an
 * Hu3DExec camera+model bracket carries this key, including the ones
 * Hu3DDrawPost emits after the bracket closed; matrices with no key at all
 * (sprites, wipes, the shadow/reflect passes) are presented at tick N verbatim.
 *
 * DEGRADATION IS ALWAYS "PRESENT IT UN-INTERPOLATED", NEVER "DROP IT".  Every
 * rejection below only clears cur->prevPos[i], and fi_build_replay emits stream
 * N's own bytes for such a matrix.  No branch here can remove a draw.
 *
 * GROUP-INTEGRITY GUARD (defect-D flicker class).  A key's ordinal is its
 * EMISSION INDEX inside one model draw, not a node identity.  When a model's
 * pos-load membership changes between ticks (a mesh culled in/out, a node
 * newly deferred to the translucent pass, an LOD flip), every later node of
 * that model shifts ordinal by one, so ordinal k in tick N names a DIFFERENT
 * node than ordinal k in tick N-1.  Interpolating such a pair blends two
 * different nodes' matrices; adjacent nodes usually sit inside the 50u/10deg
 * gate, so the blend is NOT snapped and the node renders displaced for every
 * replay frame of that window -- a one-tick flash of shifted geometry, the
 * "old unlocked-FPS flicker" the model path was built to kill (see 37247fa:
 * "a replayed command stream has NO OBJECT IDENTITY").  The same failure
 * shape appears when one model instance is drawn twice under one camera:
 * ordinals restart per bracket, keys duplicate, and the search binds both
 * instances to the first match.
 *
 * The guard: a pair may interpolate only when its (camera, model,
 * generation) group has the SAME pos-load count in N-1 and N, and its exact
 * key is UNIQUE on both sides.  A group whose membership changed presents at
 * tick N verbatim for that one window (exactly feature-off for one tick,
 * imperceptible) and re-pairs on the next tick when N-1/N agree again.
 * A same-count membership SWAP inside one model in one tick remains
 * theoretically pairable-wrong (nothing distinguishes it from motion); it
 * requires a simultaneous add+remove in the same bracket in one tick, which
 * no observed scene does. */
static void fi_pair_stream(FiStream *cur, const FiStream *prev)
{
    uint32_t i;
    uint32_t diagReordered = 0, diagUnmatched = 0, diagGroupSnap = 0;
    /* Why a matrix ended up unpaired, split by cause. "nokey" is the one that
     * matters most in practice: a pos load emitted outside any Hu3DExec model
     * bracket has NO identity at all and can never pair, so it snaps to tick N
     * forever. While the camera is still that is invisible (a snapped matrix
     * and an interpolated one render identically); during a camera pan it is
     * the whole defect -- see the pan-phase section of
     * docs/history/F3_LEAF_STROBE.md. */
    uint32_t diagNoKey = 0, diagNoPrevGroup = 0, diagCountDiff = 0, diagDup = 0;
    uint32_t diagNoKeyInCam = 0; /* of those, the ones inside a 3D camera */
    uint32_t diagNoPrev = 0;     /* no usable predecessor stream at all */
    fi_pair_cache_invalidate();
    if (!cur->walkOk) return;
    if (!fi_grow((void **)&cur->prevPos, &cur->prevPosCap,
                 cur->posCount, sizeof(int32_t))) {
        cur->walkOk = 0;
        return;
    }
    if (prev != NULL && prev->walkOk) {
        fi_table_build(&s_curGroupTab, cur->posKey, cur->keyCount, 0);
        fi_table_build(&s_curNameTab, cur->posKey, cur->keyCount, 1);
        fi_table_build(&s_prevGroupTab, prev->posKey, prev->keyCount, 0);
        fi_table_build(&s_prevNameTab, prev->posKey, prev->keyCount, 1);
    }
    for (i = 0; i < cur->posCount; ++i) {
        uint32_t gCur, oCur, gPrev, oPrev;
        int32_t prevAt;
        cur->prevPos[i] = -1;
        if (!prev || !prev->walkOk) { diagNoPrev++; continue; }
        if (!cur->posKey[i].valid) {
            diagNoKey++;
            if (cur->posKey[i].camera >= 0) diagNoKeyInCam++;
            continue;
        }
        /* An allocation failure in any table degrades to "nothing pairs this
         * tick" -- the same one-directional degradation every other rejection
         * here uses, never a wrong pair. */
        gCur = fi_table_lookup(&s_curGroupTab, &cur->posKey[i], 0, NULL);
        oCur = fi_table_lookup(&s_curNameTab, &cur->posKey[i], 1, NULL);
        gPrev = fi_table_lookup(&s_prevGroupTab, &cur->posKey[i], 0, NULL);
        oPrev = fi_table_lookup(&s_prevNameTab, &cur->posKey[i], 1, &prevAt);
        if (fi_diag() >= 3) {
            /* Oracle for the linear-to-hashed census change: the reference
             * scan and the table must agree on every key of every tick, and
             * the table's first-index must be the exact match the old search
             * returned. Any line here means the optimisation changed a
             * pairing decision, which is a correctness bug, not a slow path. */
            uint32_t rg, ro, pg, po;
            uint32_t j;
            int32_t refAt = -1;
            fi_group_census(&cur->posKey[i], cur->posKey, cur->keyCount, &rg, &ro);
            fi_group_census(&cur->posKey[i], prev->posKey, prev->keyCount, &pg, &po);
            for (j = 0; j < prev->keyCount; ++j) {
                if (fi_key_equal(&cur->posKey[i], &prev->posKey[j])) { refAt = (int32_t)j; break; }
            }
            if (rg != gCur || ro != oCur || pg != gPrev || po != oPrev || refAt != prevAt) {
                fprintf(stderr, "[MP6-FI] CENSUS MISMATCH tick=%ld idx=%u table(g=%u o=%u pg=%u po=%u at=%d) "
                                "scan(g=%u o=%u pg=%u po=%u at=%d)\n",
                        cur->tick, i, gCur, oCur, gPrev, oPrev, prevAt,
                        rg, ro, pg, po, refAt);
            }
        }
        if (gCur != gPrev || oCur != 1 || oPrev != 1) {
            /* membership changed, or ambiguous duplicate key: snap this pair */
            if (gPrev == 0) diagNoPrevGroup++;
            else if (oCur != 1 || oPrev != 1) diagDup++;
            else diagCountDiff++;
            if (fi_diag() && gPrev != 0) diagGroupSnap++;
            continue;
        }
        if (i < prev->posCount && fi_key_equal(&cur->posKey[i], &prev->posKey[i])) {
            cur->prevPos[i] = (int32_t)i;
            continue;
        }
        /* oPrev == 1 above, so the table's first index IS the unique exact
         * match the old linear search would have returned. */
        cur->prevPos[i] = prevAt;
        if (cur->prevPos[i] >= 0) diagReordered++; else diagUnmatched++;
    }
    if (fi_diag() >= 3) {
        fprintf(stderr, "[MP6-FI] pair tick=%ld pos=%u paired=%u reordered=%u | unpaired: "
                        "nokey=%u (incam=%u) noprev=%u noprevgroup=%u countdiff=%u dup=%u "
                        "unmatched=%u\n",
                cur->tick, cur->posCount,
                cur->posCount - (diagNoKey + diagNoPrev + diagNoPrevGroup + diagCountDiff +
                                 diagDup + diagUnmatched),
                diagReordered, diagNoKey, diagNoKeyInCam, diagNoPrev, diagNoPrevGroup,
                diagCountDiff, diagDup, diagUnmatched);
    } else if (fi_diag() && (diagReordered || diagGroupSnap)) {
        fprintf(stderr, "[MP6-FI] pair tick=%ld pos=%u reordered=%u unmatched=%u groupsnap=%u\n",
                cur->tick, cur->posCount, diagReordered, diagUnmatched, diagGroupSnap);
    }
}

/* =======================================================================
 * TRS decompose / advance / recompose + normal-matrix recompute.
 *
 * Matrices are GC Mtx (row-major 3 rows x 4 cols; column j of the 3x3 is
 * the image of basis vector j, column 3 is translation). All math in
 * doubles for headroom; results written back as f32 big-endian.
 * ======================================================================= */

typedef struct {
    double m[3][4];
} FiMtx;

static void fi_read_mtx(const uint8_t *p, FiMtx *out)
{
    int r, c;
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 4; ++c)
            out->m[r][c] = rd_bef32(p + (r * 4 + c) * 4);
}

static void fi_write_mtx(uint8_t *p, const FiMtx *in)
{
    int r, c;
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 4; ++c)
            wr_bef32(p + (r * 4 + c) * 4, (float)in->m[r][c]);
}

typedef struct {
    double t[3];    /* translation */
    double s[3];    /* per-column scale */
    double q[4];    /* rotation quaternion (w,x,y,z) */
} FiTrs;

static int fi_finite3(const double *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

/* 3x3 -> quaternion (Shepperd). R is column-orthonormal by construction. */
static void fi_quat_from_rot(const double R[3][3], double q[4])
{
    double tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0.0) {
        double s = sqrt(tr + 1.0) * 2.0;
        q[0] = 0.25 * s;
        q[1] = (R[2][1] - R[1][2]) / s;
        q[2] = (R[0][2] - R[2][0]) / s;
        q[3] = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        double s = sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2.0;
        q[0] = (R[2][1] - R[1][2]) / s;
        q[1] = 0.25 * s;
        q[2] = (R[0][1] + R[1][0]) / s;
        q[3] = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        double s = sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2.0;
        q[0] = (R[0][2] - R[2][0]) / s;
        q[1] = (R[0][1] + R[1][0]) / s;
        q[2] = 0.25 * s;
        q[3] = (R[1][2] + R[2][1]) / s;
    } else {
        double s = sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2.0;
        q[0] = (R[1][0] - R[0][1]) / s;
        q[1] = (R[0][2] + R[2][0]) / s;
        q[2] = (R[1][2] + R[2][1]) / s;
        q[3] = 0.25 * s;
    }
}

static void fi_rot_from_quat(const double q[4], double R[3][3])
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - w * z);     R[0][2] = 2 * (x * z + w * y);
    R[1][0] = 2 * (x * y + w * z);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - w * x);
    R[2][0] = 2 * (x * z - w * y);     R[2][1] = 2 * (y * z + w * x);     R[2][2] = 1 - 2 * (x * x + y * y);
}

/* Decompose M = T * R * S (column scales). 0 = not decomposable (zero/tiny
 * scale, mirroring, non-finite) -> caller passes the pair through verbatim.
 *
 * NOT LOSSLESS, BY CONSTRUCTION. This form is exact only for a similarity
 * transform. A modelview whose 3x3 carries SHEAR -- which is what a non-uniform
 * parent scale left-multiplying a child rotation produces, e.g. mode select's
 * bridge1..4 under `ground` scale (1.25,1,1), or the board frog's kaeru_b3/b4
 * -- has non-orthogonal columns, and column-normalising throws the shear away.
 * fi_compose() then rebuilds an orthogonal basis, so the round trip alone MOVES
 * the geometry even at alpha = 0. Callers MUST carry the residual
 * (B - fi_compose(fi_decompose(B))); see the rewrite site in fi_build_replay. */
static int fi_decompose(const FiMtx *M, FiTrs *out)
{
    double R[3][3];
    double det, qn;
    int c, r;
    for (c = 0; c < 3; ++c) {
        double len = sqrt(M->m[0][c] * M->m[0][c] + M->m[1][c] * M->m[1][c] + M->m[2][c] * M->m[2][c]);
        if (!isfinite(len) || len < 1e-9) return 0;
        out->s[c] = len;
        for (r = 0; r < 3; ++r) R[r][c] = M->m[r][c] / len;
    }
    det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) -
          R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
          R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
    if (!isfinite(det) || det < 0.5) return 0; /* mirrored or badly skewed: verbatim */
    fi_quat_from_rot((const double(*)[3])R, out->q);
    /* Shepperd's formula assumes R is orthonormal. On a sheared modelview it is
     * not, and the quaternion comes back NON-UNIT -- fi_rot_from_quat() would
     * then scale as well as rotate (and |dot| can exceed 1 in fi_advance).
     * Normalise so the rotation half is a pure rotation and 100% of the shear
     * lands in the caller's residual instead of leaking into the arc. */
    qn = sqrt(out->q[0] * out->q[0] + out->q[1] * out->q[1] +
              out->q[2] * out->q[2] + out->q[3] * out->q[3]);
    if (!isfinite(qn) || qn < 1e-9) return 0;
    for (r = 0; r < 4; ++r) out->q[r] /= qn;
    out->t[0] = M->m[0][3];
    out->t[1] = M->m[1][3];
    out->t[2] = M->m[2][3];
    if (!fi_finite3(out->t) || !isfinite(out->q[0])) return 0;
    return 1;
}

/* Worst per-column SCALE CHANGE between two decomposed matrices, expressed as a
 * ratio above 1: max_i max(sb_i/sa_i, sa_i/sb_i) - 1. Symmetric on purpose, so
 * a 2x grow and a 2x shrink both read 1.0 and one threshold covers both
 * directions of a pop. fi_decompose rejects any column shorter than 1e-9 and
 * every length it accepts is finite, so both divisions are safe on any pair
 * that reached here. */
static double fi_scale_ratio(const FiTrs *a, const FiTrs *b)
{
    double worst = 1.0;
    int i;
    for (i = 0; i < 3; ++i) {
        double u = b->s[i] / a->s[i];
        double v = a->s[i] / b->s[i];
        double m = (u > v) ? u : v;
        if (m > worst) worst = m;
    }
    return worst - 1.0;
}

/* Advance a->b one step further, evaluated at t = 1 + alpha (slerp with
 * t > 1 extrapolates the same arc; translation/scale extend linearly).
 *
 * holdScale pins the SCALE channel to b's own scale (alphaS = 0) while
 * translation and rotation still advance -- the scale gate's action, see
 * FI_SCALE_SNAP_RATIO. It is a per-CHANNEL degradation, not a snap of the
 * matrix, and it is a no-op at alpha = 0 (where b->s is what the linear term
 * returns anyway), which is what keeps O(0) == B exact. */
static void fi_advance(const FiTrs *a, const FiTrs *b, double alpha, int holdScale, FiTrs *out)
{
    double dot, t = 1.0 + alpha;
    double qa[4];
    int i;
    for (i = 0; i < 3; ++i) {
        out->t[i] = b->t[i] + (b->t[i] - a->t[i]) * alpha;
        if (holdScale) {
            out->s[i] = b->s[i];
            continue;
        }
        out->s[i] = b->s[i] + (b->s[i] - a->s[i]) * alpha;
        if (out->s[i] < 1e-9) out->s[i] = b->s[i]; /* sign-flip guard */
    }
    memcpy(qa, a->q, sizeof(qa));
    dot = qa[0] * b->q[0] + qa[1] * b->q[1] + qa[2] * b->q[2] + qa[3] * b->q[3];
    if (dot < 0.0) { /* take the short arc */
        for (i = 0; i < 4; ++i) qa[i] = -qa[i];
        dot = -dot;
    }
    if (dot > 0.9995) { /* near-identical: nlerp-extrapolate + renormalize */
        double n = 0.0;
        for (i = 0; i < 4; ++i) {
            out->q[i] = qa[i] + (b->q[i] - qa[i]) * t;
            n += out->q[i] * out->q[i];
        }
        n = sqrt(n);
        if (n < 1e-9) { memcpy(out->q, b->q, sizeof(out->q)); return; }
        for (i = 0; i < 4; ++i) out->q[i] /= n;
    } else {
        double theta = acos(dot);
        double sinT = sin(theta);
        double wa = sin((1.0 - t) * theta) / sinT;
        double wb = sin(t * theta) / sinT;
        for (i = 0; i < 4; ++i) out->q[i] = wa * qa[i] + wb * b->q[i];
    }
}

static void fi_compose(const FiTrs *in, FiMtx *out)
{
    double R[3][3];
    int r, c;
    fi_rot_from_quat(in->q, R);
    for (r = 0; r < 3; ++r) {
        for (c = 0; c < 3; ++c) out->m[r][c] = R[r][c] * in->s[c];
        out->m[r][3] = in->t[r];
    }
}

/* Normal matrix = inverse-transpose of the pos 3x3 (hsfdraw.c's own
 * PSMTXInvXpose relationship), written in GXLoadNrmMtxImm's repacked
 * 9-float order. 0 = singular -> caller leaves the nrm load verbatim. */
static int fi_write_nrm_from_pos(uint8_t *payload, const FiMtx *pos)
{
    const double(*m)[4] = pos->m;
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                 m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    double inv[3][3];
    int r, c, i;
    if (!isfinite(det) || fabs(det) < 1e-12) return 0;
    /* inverse of the 3x3 (adjugate/det) */
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    /* inverse-TRANSPOSE, rows in GXLoadNrmMtxImm's mtx[0],[1],[2] /
     * [4],[5],[6] / [8],[9],[10] order == the 3x3's rows */
    i = 0;
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c)
            wr_bef32(payload + (i++) * 4, (float)inv[c][r]);
    return 1;
}

/* Endpoint preparation is independent of alpha and live scale/residual
 * policy. Keep those decisions in the builder; cache only the same double
 * arithmetic it previously repeated on every present. */
struct FiPreparedPair {
    FiTrs a, b;
    FiMtx current, rebuilt;
    double distanceSq, distance, qdot, scaleRatio;
};
enum { FI_PAIR_EMPTY, FI_PAIR_EQUAL, FI_PAIR_INVALID, FI_PAIR_READY };

static uint8_t fi_prepare_pair(const uint8_t *pa, const uint8_t *pb, FiPreparedPair *p)
{
    FiMtx a;
    double dx, dy, dz;
    if (memcmp(pa, pb, 48) == 0) return FI_PAIR_EQUAL;
    fi_read_mtx(pa, &a);
    fi_read_mtx(pb, &p->current);
    if (!fi_decompose(&a, &p->a) || !fi_decompose(&p->current, &p->b))
        return FI_PAIR_INVALID;
    dx = p->b.t[0] - p->a.t[0];
    dy = p->b.t[1] - p->a.t[1];
    dz = p->b.t[2] - p->a.t[2];
    p->distanceSq = dx * dx + dy * dy + dz * dz;
    p->distance = sqrt(p->distanceSq);
    p->qdot = fabs(p->a.q[0] * p->b.q[0] + p->a.q[1] * p->b.q[1] +
                   p->a.q[2] * p->b.q[2] + p->a.q[3] * p->b.q[3]);
    p->scaleRatio = fi_scale_ratio(&p->a, &p->b);
    if (p->distanceSq < FI_TRANS_SNAP_U * FI_TRANS_SNAP_U &&
        p->qdot > FI_ROT_SNAP_QDOT)
        fi_compose(&p->b, &p->rebuilt);
    return FI_PAIR_READY;
}

static int fi_pair_cache_begin(const FiStream *cur, const FiStream *prev)
{
    uint32_t count = cur->posCount < FI_PAIR_CACHE_MAX ? cur->posCount : FI_PAIR_CACHE_MAX;
    if (s_pairCacheCur == cur && s_pairCachePrev == prev && s_pairCacheCount == count)
        return 1;
    fi_pair_cache_invalidate();
    if (!count || !fi_grow((void **)&s_pairCache, &s_pairCacheCap, count, sizeof(*s_pairCache)) ||
        !fi_grow((void **)&s_pairState, &s_pairStateCap, count, sizeof(*s_pairState)))
        return 0;
    memset(s_pairState, FI_PAIR_EMPTY, count);
    s_pairCacheCur = cur;
    s_pairCachePrev = prev;
    s_pairCacheCount = count;
    return 1;
}

/* =======================================================================
 * Frame-boundary hooks (called from aurora_bridge.c).
 * ======================================================================= */

static void fi_set_active(int on)
{
    if (on && !s_active) {
        aurora_gx_set_drain_capture(fi_capture_sink, NULL);
        s_active = 1;
        if (fi_diag()) fprintf(stderr, "[MP6-FI] capture armed (unlocked fps ON)\n");
    } else if (!on && s_active) {
        aurora_gx_set_drain_capture(NULL, NULL);
        s_active = 0;
        s_cur = -1;
        s_latest = -1;
        s_prev = -1; /* retention invalidated; buffers kept (grow-only) */
        s_replayBudgetNs = 0;
        mp6_fi_model_reset();
    }
}

void mp6_fi_note_frame_begin(void)
{
    int enabled;
    mp6_ao_begin_frame(); /* even when Unlocked FPS itself is disabled */
    enabled = mp6_unlocked_fps_enabled() && mp6_tick_interpolation_possible();
    fi_set_active(enabled);
    if (!enabled) return;
    /* Arm capture for the new tick into the stream slot NOT holding N
     * (which the idle window may still be replaying from -- N-1's slot is
     * the one being retired, exactly the decided N-1/N retention bound). */
    s_cur = (s_latest == 0) ? 1 : 0;
    fi_stream_reset(&s_streams[s_cur]);
    aurora_gx_export_vtx_layout(s_streams[s_cur].seedDesc, s_streams[s_cur].seedCnt,
                                s_streams[s_cur].seedType);
}

void mp6_fi_note_frame_end(void)
{
    FiStream *s;
    if (fi_diag() >= 2) {
        /* One QPC stamp per REAL tick present, feature on or off (the bridge
         * calls this after every aurora_end_frame) -- external capture tools
         * assign their grabs to exact tick windows with it. */
        static long s_presentStamp;
        fprintf(stderr, "[MP6-FI] present n=%ld mono_ns=%lld\n",
                ++s_presentStamp, (long long)mp6_host_monotonic_ns());
    }

    /* GATE-A instrument, every REAL tick regardless of mode/enable: a digest of
     * live model tick+anim-clock+transform state. Comparing the stream with the
     * feature ON vs OFF proves the idle-window re-runs advanced no game state.
     * Cached-getenv no-op when MP6_FI_ANIMLOG is unset. */
    {
        static long s_animTick;
        mp6_fi_model_animlog(++s_animTick);
    }

    if (!s_active || s_cur < 0) return;
    s = &s_streams[s_cur];
    s->sealed = 1;
    s->tick = ++s_sealCounter;
    fi_walk_stream(s);
    s_prev = s_latest;
    s_latest = s_cur;
    fi_pair_stream(s, s_prev >= 0 ? &s_streams[s_prev] : NULL);
    s_censusSeals++;
    if (!s->walkOk) s_censusSealWalkFail++;
    if (s_prev < 0 || !s_streams[s_prev].walkOk) s_censusSealNoPrev++;
    mp6_fi_model_snapshot();
    s_cur = -1;
    s_lastSealNs = (int64_t)mp6_host_monotonic_ns();
    s_lastPresentNs = s_lastSealNs; /* the real present anchors the spacing grid */
    s_replaysThisWindow = 0;
    if (fi_diag() && (s->tick % 300) == 0) {
        /* posChanged: how many paired pos-matrix loads actually moved this
         * tick -- distinguishes "replaying but everything is static" (2D
         * scenes; interpolation output == snap output) from real 3D motion. */
        uint32_t posChanged = 0;
        if (s->walkOk && s_prev >= 0 && s_streams[s_prev].walkOk) {
            uint32_t k;
            for (k = 0; k < s->posCount; ++k) {
                int32_t p = s->prevPos[k];
                if (p >= 0 && memcmp(s_streams[s_prev].data + s_streams[s_prev].posOff[p],
                           s->data + s->posOff[k], 48) != 0) {
                    posChanged++;
                }
            }
        }
        fprintf(stderr, "[MP6-FI] tick=%ld stream=%uB chunks=%u walk=%s posMtx=%u posChanged=%u | chunk-proof: %u/%u aligned, %u misaligned | replays=%ld snaps=%ld skipped=%ld\n",
                s->tick, s->size, s->chunkCount, s->walkOk ? "ok" : "FAIL", s->posCount, posChanged,
                s_diagChunkAligned, s_diagChunkChecked, s_diagChunkMisaligned,
                s_statReplays, s_statSnaps, s_statSkippedWindows);
        mp6_fi_census_log(s->tick);
    }
}

/* One line naming WHY the last 300 ticks' idle windows did or did not present.
 * Deltas, not cumulatives: a scene change moves the answer, and a cumulative
 * count would bury it under the boot path's history. */
static void mp6_fi_census_log(long tick)
{
    int i;
    long calls = 0;
    char buf[512];
    size_t used = 0;
    if (!fi_diag()) return;
    for (i = 0; i < FI_DECL_COUNT; ++i) {
        long delta = s_declCount[i] - s_declWindowBase[i];
        calls += delta;
        if (delta != 0 && used < sizeof(buf) - 1) {
            int n = snprintf(buf + used, sizeof(buf) - used, " %s=%ld",
                             kFiDeclNames[i], delta);
            if (n > 0) used += (size_t)n;
            if (used >= sizeof(buf)) used = sizeof(buf) - 1;
        }
    }
    fprintf(stderr,
            "[MP6-FI-CENSUS] vitick=%ld tick=%ld calls=%ld |%s | slack(min=%.3fms avg=%.3fms "
            "max=%.3fms n=%ld) budget=%.3fms | cost-ms(build avg=%.3f max=%.3f, "
            "present avg=%.3f max=%.3f, n=%ld) | seals=%ld walkfail=%ld noprev=%ld\n",
            mp6_tick_count, tick, calls, used ? buf : " (none)",
            (double)s_declSlackMinNs / 1e6,
            s_declSlackSamples ? (double)s_declSlackSumNs / (double)s_declSlackSamples / 1e6 : 0.0,
            (double)s_declSlackMaxNs / 1e6, s_declSlackSamples,
            (double)s_replayBudgetNs / 1e6,
            s_costSamples ? (double)s_costBuildSumNs / (double)s_costSamples / 1e6 : 0.0,
            (double)s_costBuildMaxNs / 1e6,
            s_costSamples ? (double)s_costPresentSumNs / (double)s_costSamples / 1e6 : 0.0,
            (double)s_costPresentMaxNs / 1e6, s_costSamples,
            s_censusSeals, s_censusSealWalkFail, s_censusSealNoPrev);
    fflush(stderr);
    for (i = 0; i < FI_DECL_COUNT; ++i) s_declWindowBase[i] = s_declCount[i];
    s_declSlackSumNs = 0;
    s_declSlackMaxNs = 0;
    s_declSlackMinNs = 0;
    s_declSlackSamples = 0;
    s_costBuildSumNs = s_costBuildMaxNs = 0;
    s_costPresentSumNs = s_costPresentMaxNs = 0;
    s_costSamples = 0;
    s_censusSeals = s_censusSealWalkFail = s_censusSealNoPrev = 0;
}

/* =======================================================================
 * Pull-side census reader (include/mp6_diag_probe.h).
 *
 * Everything below already existed and is already maintained on every path --
 * only the PRINTING is gated by MP6_FI_DIAG. So the dev console reads the very
 * counters [MP6-FI-CENSUS] formats instead of keeping its own, which is the
 * point: two independent counts of "why did this window not present" could
 * disagree, and then neither could be trusted. Pure read; it does not reset
 * the census window (mp6_fi_census_log owns that).
 * ======================================================================= */
typedef char fi_decl_fits_probe[(FI_DECL_COUNT <= MP6_FI_DECL_MAX) ? 1 : -1];

const char *mp6_fi_decl_name(int index)
{
    if (index < 0 || index >= FI_DECL_COUNT) return "?";
    return kFiDeclNames[index];
}

void mp6_fi_stats_get(Mp6FiStats *out)
{
    int i;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->declCount = FI_DECL_COUNT;
    for (i = 0; i < FI_DECL_COUNT; ++i) out->decl[i] = s_declCount[i];
    out->slackMinNs = s_declSlackMinNs;
    out->slackMaxNs = s_declSlackMaxNs;
    out->slackSumNs = s_declSlackSumNs;
    out->slackSamples = s_declSlackSamples;
    out->budgetNs = s_replayBudgetNs;
    out->costBuildSumNs = s_costBuildSumNs;
    out->costBuildMaxNs = s_costBuildMaxNs;
    out->costPresentSumNs = s_costPresentSumNs;
    out->costPresentMaxNs = s_costPresentMaxNs;
    out->costSamples = s_costSamples;
    out->seals = s_censusSeals;
    out->sealWalkFail = s_censusSealWalkFail;
    out->sealNoPrev = s_censusSealNoPrev;
    out->statReplays = s_statReplays;
    out->statSnaps = s_statSnaps;
    out->statSkipped = s_statSkippedWindows;
    if (s_latest >= 0) {
        const FiStream *cur = &s_streams[s_latest];
        out->streamBytes = cur->size;
        out->chunkCount = cur->chunkCount;
        out->posCount = cur->posCount;
        out->walkOk = cur->walkOk;
    }
}

/* =======================================================================
 * Replay.
 * ======================================================================= */

static uint8_t *s_replayBuf;
static uint32_t s_replayCap;

static void fi_flush_replay_copy(const uint8_t *source, uint32_t from, uint32_t to, uint32_t *bytes)
{
    if (*bytes) {
        memcpy(s_replayBuf + to, source + from, *bytes);
        *bytes = 0;
    }
}

/* Diagnostics (MP6_FI_DIAG>=3): what the last fi_build_replay actually did to
 * the stream, so a replay frame's on-screen change can be attributed to (or
 * cleared of) the matrix rewrite without filtering by magnitude. */
static uint32_t s_dbgPosSeen, s_dbgRewritten, s_dbgSnapUnpaired, s_dbgSnapGate, s_dbgVerbatimEq;
/* The two classes that used to be counted NOWHERE, which is why s_dbgPosSeen
 * never equalled the sum of the buckets and neither class could be argued about
 * from a log:
 *   snapCamera  -- paired, but its camera moved discontinuously this tick
 *                  (mp6_fi_model_camera_stable said no).
 *   snapDecompose -- fi_decompose refused one of the two matrices (a column
 *                  shorter than 1e-9, a mirrored/badly-skewed basis with
 *                  det < 0.5, a non-finite element). Mirrored geometry is real
 *                  in this game -- reflections and some flipped sprites -- so
 *                  this bucket is expected to be nonzero, and "expected to be
 *                  nonzero" is exactly the state that needs a counter rather
 *                  than a silent fall-through.
 * With these two the six buckets partition s_dbgPosSeen exactly, and
 * fi_build_replay asserts that at MP6_FI_DIAG>=3. */
static uint32_t s_dbgSnapCamera, s_dbgSnapDecompose;
static uint32_t s_dbgDropOffscreen, s_dbgDropCopyBind, s_dbgDropCopyExec, s_dbgDropDestroy,
                s_dbgDropOther, s_dbgDropPosLoads, s_dbgCopyClear;
static double s_dbgMaxTrans, s_dbgMinQdot;
/* Worst per-column scale ratio-1 seen this replay (fi_scale_ratio), with the
 * model/ordinal that produced it, and how many pairs the scale gate acted on.
 * maxTrans/minQdot have always been here; scale had no meter at all, so the
 * dice bloom was invisible in every diagnostic this file emits. */
static double s_dbgMaxScaleRatio;
static int s_dbgScaleModel, s_dbgScaleOrd;
static uint32_t s_dbgScaleHold;
/* The worst ratio the gate ACTUALLY ACTED ON, i.e. the largest scale change
 * that reached the hold after passing FI_TRANS_SNAP_U and FI_ROT_SNAP_QDOT.
 * maxScale alone cannot answer this and it matters: many of the loudest scale
 * changes in the game are pop-ins that SPIN as they grow, so the rotation gate
 * already snaps the whole pair and the scale channel was never the actor there.
 * maxHeld is the size of the overshoot that was genuinely unbounded before this
 * gate existed -- the number the change is worth. */
static double s_dbgMaxHeldScale;
static int s_dbgHeldModel, s_dbgHeldOrd;
/* Worst TRS round-trip residual over the replay: max |compose(decompose(B)) - B|
 * element-wise. TRS decomposition is exact only for a similarity transform; a
 * modelview with non-uniform object scale under rotation has non-orthogonal
 * columns, so the round trip alone would displace the geometry. This is now the
 * size of the shear the rewrite CARRIES rather than injects -- nonzero here is
 * expected and harmless; s_dbgMaxAlpha0Err below is the error meter. */
static double s_dbgMaxRoundTrip;
static int s_dbgRoundTripModel, s_dbgRoundTripOrd;
/* Self-check: the finished rewrite evaluated at alpha = 0, compared against the
 * tick matrix it was built from. MUST stay at float-rounding noise -- any real
 * value here means a replay frame disagrees with its own tick frame, which is
 * exactly the A-B-A flicker signature. */
static double s_dbgMaxAlpha0Err;
static int s_dbgAlpha0Model, s_dbgAlpha0Ord;

/* Build the skip-filtered, matrix-rewritten replay image of stream `cur`
 * (paired against `prev` when interp != 0). Returns length, 0 on failure. */
static uint32_t fi_build_replay(const FiStream *cur, const FiStream *prev, int interp, double alpha)
{
    FiMtx lastPos[10];       /* interpolated pos 3x4 per PN slot (id/3) */
    uint8_t lastPosState[10]; /* 0 = untouched/verbatim, 1 = rewritten */
    uint32_t pos = 0, out = 0, posIdx = 0, commandIdx = 0;
    uint32_t copyFrom = 0, copyTo = 0, copyBytes = 0;
    uint32_t aoIdx = 0;
    int skipDepthOffscreen = 0;
    int pairCache;

    memset(lastPosState, 0, sizeof(lastPosState));
    s_replayAoCount = 0;
    s_dbgPosSeen = s_dbgRewritten = s_dbgSnapUnpaired = s_dbgSnapGate = s_dbgVerbatimEq = 0;
    s_dbgSnapCamera = s_dbgSnapDecompose = 0;
    s_dbgDropOffscreen = s_dbgDropCopyBind = s_dbgDropCopyExec = s_dbgDropDestroy = 0;
    s_dbgDropOther = s_dbgDropPosLoads = s_dbgCopyClear = 0;
    s_dbgMaxTrans = 0.0;
    s_dbgMinQdot = 1.0;
    s_dbgMaxScaleRatio = 0.0;
    s_dbgScaleModel = s_dbgScaleOrd = -1;
    s_dbgScaleHold = 0;
    s_dbgMaxHeldScale = 0.0;
    s_dbgHeldModel = s_dbgHeldOrd = -1;
    s_dbgMaxRoundTrip = 0.0;
    s_dbgRoundTripModel = s_dbgRoundTripOrd = -1;
    s_dbgMaxAlpha0Err = 0.0;
    s_dbgAlpha0Model = s_dbgAlpha0Ord = -1;
    if (!cur->walkOk || !fi_grow((void **)&s_replayBuf, &s_replayCap, cur->size, 1)) return 0;
    pairCache = interp && fi_pair_cache_begin(cur, prev);

    while (pos < cur->size) {
        FiCmd c;
        if (commandIdx >= cur->commandCount) return 0;
        c = cur->commands[commandIdx++];
        if (c.offset != pos || c.size > cur->size - pos) return 0;
        while (aoIdx < cur->aoCount && cur->ao[aoIdx].offset == pos) {
            s_replayAo[s_replayAoCount] = cur->ao[aoIdx++];
            s_replayAo[s_replayAoCount++].offset = out;
        }
        if (aoIdx < cur->aoCount && cur->ao[aoIdx].offset < pos) return 0;
        uint32_t n = c.size;
        uint32_t streamPosIdx = UINT32_MAX;
        int drop = 0;
        int emitCopyClear = 0;
        if (n == 0) return 0; /* cannot happen after a clean seal walk; belt+braces */
        if (fi_is_pos_load(&c)) streamPosIdx = posIdx++;

        if (c.op == GX_AURORA) {
            switch (c.auroraSub) {
            case GX_AURORA_BEGIN_OFFSCREEN:
                skipDepthOffscreen = 1; /* drop bracket + contents (offscreen shadow pass) */
                drop = 1;
                break;
            case GX_AURORA_END_OFFSCREEN:
                skipDepthOffscreen = 0;
                drop = 1;
                break;
            case GX_AURORA_LOAD_COPY_SRC:
            case GX_AURORA_LOAD_COPY_DST:
            case GX_AURORA_LOAD_COPY_DEST:
            case GX_AURORA_REQUEST_DEPTH_SNAPSHOT: /* replay re-arm would poison GXPeekZ */
            case GX_AURORA_SET_COPY_MIP_GEN:
            case GX_AURORA_DESTROY_TEXOBJ:
            case GX_AURORA_DESTROY_TLUT:
            case GX_AURORA_DESTROY_COPY_TEX:
            case GX_AURORA_DESTROY_FRAMEBUFFER_CACHE:
                drop = 1;
                break;
            default:
                break;
            }
        } else if (c.op == 0x61 && c.bpReg == 0x52) {
            /* Suppress the CPU-addressed texture resolve, but preserve the
             * command's clear-after-copy pass boundary below. */
            emitCopyClear = (rd_be32(cur->data + pos + 1) & (1u << 11)) != 0;
            drop = 1;
        }
        if (skipDepthOffscreen) {
            drop = 1;
            emitCopyClear = 0;
        }
        if (drop) {
            /* Keep verbatim commands in one span. A removed/replaced command
             * breaks contiguity, so commit the preceding span first. */
            fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
            if (skipDepthOffscreen || c.auroraSub == GX_AURORA_BEGIN_OFFSCREEN ||
                c.auroraSub == GX_AURORA_END_OFFSCREEN) {
                s_dbgDropOffscreen += c.op < 8 ? n : 1;
            } else if (c.op == 0x61 && c.bpReg == 0x52) {
                s_dbgDropCopyExec++;
            } else if (c.auroraSub == GX_AURORA_LOAD_COPY_SRC ||
                       c.auroraSub == GX_AURORA_LOAD_COPY_DST ||
                       c.auroraSub == GX_AURORA_LOAD_COPY_DEST) {
                s_dbgDropCopyBind++;
            } else if (c.auroraSub == GX_AURORA_DESTROY_TEXOBJ ||
                       c.auroraSub == GX_AURORA_DESTROY_TLUT ||
                       c.auroraSub == GX_AURORA_DESTROY_COPY_TEX ||
                       c.auroraSub == GX_AURORA_DESTROY_FRAMEBUFFER_CACHE) {
                s_dbgDropDestroy++;
            } else {
                s_dbgDropOther++;
            }
            if (fi_is_pos_load(&c)) s_dbgDropPosLoads++;
        }
        if (emitCopyClear) s_dbgCopyClear++;
        if (emitCopyClear) {
            /* Replacement is four bytes versus the BP command's five, so the
             * replay buffer (sized to the original stream) always has room. */
            s_replayBuf[out++] = GX_AURORA;
            s_replayBuf[out++] = (uint8_t)(GX_AURORA_REPLAY_COPY_CLEAR >> 8);
            s_replayBuf[out++] = (uint8_t)GX_AURORA_REPLAY_COPY_CLEAR;
            s_replayBuf[out++] = 1;
        }

        if (!drop) {
            if (!copyBytes) {
                copyFrom = pos;
                copyTo = out;
            }
            copyBytes += n;
            if (fi_is_pos_load(&c)) {
                uint32_t slot = c.xfAddr / 4 / 3; /* addr = id*4, id = slot*3 (GX_PNMTX0..9) */
                uint8_t *payload = s_replayBuf + out + 5;
                int rewritten = 0;
                /* Hoisted so the decline census can NAME which of the two
                 * pre-conditions failed instead of one of them vanishing into a
                 * silent fall-through, and so camera_stable is still evaluated
                 * exactly once, only for a paired matrix. */
                int paired = interp && streamPosIdx < cur->posCount &&
                             cur->prevPos[streamPosIdx] >= 0;
                int camStable = paired &&
                    mp6_fi_model_camera_stable(cur->posKey[streamPosIdx].camera);
                s_dbgPosSeen++;
                if (camStable) {
                    uint32_t prevIdx = (uint32_t)cur->prevPos[streamPosIdx];
                    const uint8_t *pa = prev->data + prev->posOff[prevIdx];
                    const uint8_t *pb = cur->data + c.offset + 5;
                    FiPreparedPair fallback;
                    FiPreparedPair *pair = pairCache && streamPosIdx < s_pairCacheCount ?
                                           &s_pairCache[streamPosIdx] : &fallback;
                    uint8_t state = pair == &fallback ? FI_PAIR_EMPTY : s_pairState[streamPosIdx];
                    if (state == FI_PAIR_EMPTY) {
                        state = fi_prepare_pair(pa, pb, pair);
                        if (pair != &fallback) s_pairState[streamPosIdx] = state;
                    }
                    if (state == FI_PAIR_EQUAL) s_dbgVerbatimEq++;
                    if (state != FI_PAIR_EQUAL) {
                        FiMtx O;
                        FiTrs to;
                        const FiTrs *ta = &pair->a, *tb = &pair->b;
                        const FiMtx *B = &pair->current, *RB = &pair->rebuilt;
                        if (state == FI_PAIR_INVALID) {
                            s_dbgSnapDecompose++;
                        } else {
                            double distanceSq = pair->distanceSq;
                            double qdot = pair->qdot;
                            /* Scale is the third TRS channel and the only one
                             * that had no gate. Measured for every pair
                             * (attributed, so a log names the model), and gated
                             * by a CHANNEL HOLD rather than a snap. */
                            double sratio = pair->scaleRatio;
                            int holdScale = !fi_no_scale_hold() &&
                                            sratio >= fi_scale_snap_ratio();
                            /* Per-pair gate (FI_TRANS_SNAP_U / FI_ROT_SNAP_QDOT):
                             * a replay advances this matrix one tick FORWARD, so
                             * interpolate only when the tick's motion is small
                             * enough that the overshoot is imperceptible. A fast
                             * translation (spawn/despawn/scene cut) or fast
                             * rotation (menu portrait flip) extrapolates into a
                             * visible smear -- the reported flicker -- so those
                             * pairs pass through stream N's matrix verbatim
                             * (snapped, i.e. that object stays at the tick rate)
                             * while the genuinely-smooth remainder interpolates. */
                            double dmag = pair->distance;
                            if (dmag > s_dbgMaxTrans) s_dbgMaxTrans = dmag;
                            if (qdot < s_dbgMinQdot) s_dbgMinQdot = qdot;
                            if (sratio > s_dbgMaxScaleRatio) {
                                s_dbgMaxScaleRatio = sratio;
                                s_dbgScaleModel = (int)cur->posKey[streamPosIdx].model;
                                s_dbgScaleOrd = (int)cur->posKey[streamPosIdx].ordinal;
                            }
                            if (distanceSq >= FI_TRANS_SNAP_U * FI_TRANS_SNAP_U ||
                                qdot <= FI_ROT_SNAP_QDOT) {
                                s_dbgSnapGate++;
                            }
                            if (distanceSq < FI_TRANS_SNAP_U * FI_TRANS_SNAP_U &&
                                qdot > FI_ROT_SNAP_QDOT) {
                                int rr, cc;
                                if (fi_diag() && (uint32_t)cur->prevPos[streamPosIdx] != streamPosIdx) {
                                    /* fallback-matched pair that will interpolate: if the
                                     * key search bound two DIFFERENT nodes (membership
                                     * shift), this is the mispair-flash mechanism. */
                                    fprintf(stderr, "[MP6-FI] interp-fallback tick=%ld model=%d sub=%u ord=%u "
                                            "pairIdx %u->%d dTrans=%.2f qdot=%.5f\n",
                                            cur->tick, (int)cur->posKey[streamPosIdx].model,
                                            (unsigned)cur->posKey[streamPosIdx].sub,
                                            (unsigned)cur->posKey[streamPosIdx].ordinal,
                                            streamPosIdx, cur->prevPos[streamPosIdx],
                                            pair->distance, qdot);
                                }
                                if (fi_diag() >= 3 &&
                                    distanceSq > 1.0) {
                                    /* Level 3: every pair that actually gets
                                     * displaced, with both sides' identity.
                                     * A static object appearing here at all is
                                     * a mispair by construction. */
                                    fprintf(stderr,
                                            "[MP6-FI] rewrite vitick=%ld tick=%ld prevTick=%ld alpha=%.3f "
                                            "idx %u->%u cam=%d model=%d gen=%u sub=%u ord=%u "
                                            "prevCam=%d prevModel=%d prevGen=%u prevSub=%u prevOrd=%u "
                                            "dTrans=%.2f qdot=%.5f\n",
                                            mp6_tick_count, cur->tick, prev->tick, alpha,
                                            streamPosIdx, prevIdx,
                                            (int)cur->posKey[streamPosIdx].camera,
                                            (int)cur->posKey[streamPosIdx].model,
                                            (unsigned)cur->posKey[streamPosIdx].generation,
                                            (unsigned)cur->posKey[streamPosIdx].sub,
                                            (unsigned)cur->posKey[streamPosIdx].ordinal,
                                            (int)prev->posKey[prevIdx].camera,
                                            (int)prev->posKey[prevIdx].model,
                                            (unsigned)prev->posKey[prevIdx].generation,
                                            (unsigned)prev->posKey[prevIdx].sub,
                                            (unsigned)prev->posKey[prevIdx].ordinal,
                                            pair->distance, qdot);
                                }
                                /* ---- RESIDUAL CARRY --------------------------
                                 * fi_decompose/fi_compose is lossy on any
                                 * sheared modelview (see fi_decompose): the
                                 * round trip alone re-poses the object even
                                 * with alpha taken out, which is what made
                                 * mode select's right bridge (model 30 ord 3,
                                 * bridge4) shear on EVERY interpolated present
                                 * and snap back on the next tick frame -- an
                                 * A-B-A flicker whose amplitude is set by the
                                 * decomposition error, not by object motion,
                                 * so no motion gate above can ever catch it.
                                 *
                                 * Split B into the part the TRS model can
                                 * represent (RB) and the part it cannot
                                 * (B - RB, the shear). Interpolate only the
                                 * first and add the second back unchanged:
                                 *
                                 *   O = compose(advance(A,B,alpha)) + (B - RB)
                                 *
                                 * At alpha = 0 advance() returns B's own TRS,
                                 * so O == B EXACTLY: a replay frame can never
                                 * disagree with the tick frame it was built
                                 * from. That removes the whole defect class
                                 * rather than one object, and every object
                                 * keeps interpolating (no new snap gate, so
                                 * the smoothness the feature exists for is
                                 * preserved). The residual is a small constant
                                 * in eye space; over a single tick's gated
                                 * rotation (<=10 deg) its failure to rotate
                                 * with the object is second-order.
                                 * Column 3 needs no correction: decompose
                                 * copies the translation verbatim, so the
                                 * residual there is identically zero. */
                                fi_advance(ta, tb, alpha, holdScale, &to);
                                fi_compose(&to, &O);
                                if (holdScale) {
                                    s_dbgScaleHold++;
                                    if (sratio > s_dbgMaxHeldScale) {
                                        s_dbgMaxHeldScale = sratio;
                                        s_dbgHeldModel = (int)cur->posKey[streamPosIdx].model;
                                        s_dbgHeldOrd = (int)cur->posKey[streamPosIdx].ordinal;
                                    }
                                }
                                if (!fi_no_residual()) {
                                    for (rr = 0; rr < 3; ++rr) {
                                        for (cc = 0; cc < 3; ++cc) {
                                            O.m[rr][cc] += B->m[rr][cc] - RB->m[rr][cc];
                                        }
                                    }
                                }
                                if (fi_diag() >= 3) {
                                    /* maxResid: how much shear the TRS model
                                     * cannot represent (this is what USED to be
                                     * injected as displacement -- it is now
                                     * carried).  maxA0: the real self-check --
                                     * run the whole rewrite at alpha = 0 and
                                     * measure |O0 - B|, which must be 0. */
                                    FiTrs t0chk;
                                    FiMtx O0;
                                    /* Same holdScale the real rewrite used: the
                                     * self-check must exercise the code path
                                     * that shipped, not a second variant of it.
                                     * (alphaS = 0 is a no-op at alpha = 0, so
                                     * maxA0Err must stay 0 either way -- that
                                     * is the property being checked.) */
                                    fi_advance(ta, tb, 0.0, holdScale, &t0chk);
                                    fi_compose(&t0chk, &O0);
                                    for (rr = 0; rr < 3; ++rr) {
                                        for (cc = 0; cc < 4; ++cc) {
                                            double e = RB->m[rr][cc] - B->m[rr][cc];
                                            double e0;
                                            if (cc < 3 && !fi_no_residual())
                                                O0.m[rr][cc] += B->m[rr][cc] - RB->m[rr][cc];
                                            e0 = O0.m[rr][cc] - B->m[rr][cc];
                                            if (e < 0.0) e = -e;
                                            if (e0 < 0.0) e0 = -e0;
                                            if (e > s_dbgMaxRoundTrip) {
                                                s_dbgMaxRoundTrip = e;
                                                s_dbgRoundTripModel = (int)cur->posKey[streamPosIdx].model;
                                                s_dbgRoundTripOrd = (int)cur->posKey[streamPosIdx].ordinal;
                                            }
                                            if (e0 > s_dbgMaxAlpha0Err) {
                                                s_dbgMaxAlpha0Err = e0;
                                                s_dbgAlpha0Model = (int)cur->posKey[streamPosIdx].model;
                                                s_dbgAlpha0Ord = (int)cur->posKey[streamPosIdx].ordinal;
                                            }
                                        }
                                    }
                                }
                                fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
                                fi_write_mtx(payload, &O);
                                if (slot < 10) {
                                    lastPos[slot] = O;
                                    lastPosState[slot] = 1;
                                }
                                rewritten = 1;
                            }
                        }
                    }
                }
                /* Exhaustive by construction: rewritten / unpaired / camera-cut
                 * / byte-equal / decompose-refused / motion-gated are disjoint
                 * and cover every pos load counted in s_dbgPosSeen. The sum is
                 * checked below so a later edit cannot reintroduce a silent
                 * class. */
                if (rewritten) {
                    s_dbgRewritten++;
                } else if (!paired) {
                    s_dbgSnapUnpaired++;
                } else if (!camStable) {
                    s_dbgSnapCamera++;
                }
                if (!rewritten && slot < 10) {
                    lastPosState[slot] = 0; /* verbatim pos -> leave its nrm verbatim too */
                }
            } else if (fi_is_nrm_load(&c)) {
                uint32_t slot = (c.xfAddr - 0x400u) / 3 / 3; /* addr = id*3+0x400, id = slot*3 */
                if (slot < 10 && lastPosState[slot]) {
                    /* rewritten pos -> recompute inverse-transpose; on a
                     * singular matrix keep the original bytes */
                    fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
                    (void)fi_write_nrm_from_pos(s_replayBuf + out + 5, &lastPos[slot]);
                }
            }
            out += n;
        }
        pos += n;
    }
    fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
    if (fi_diag() >= 3) {
        /* The census must PARTITION the position loads it saw. Before the two
         * missing buckets existed it did not, and the difference was silently
         * absorbed -- so "no pair was refused for reason X" was unfalsifiable
         * from a log. Any line here means a rewrite decision exists that no
         * counter names, which is a diagnostic defect even when the pixels are
         * right. */
        uint32_t sum = s_dbgRewritten + s_dbgSnapUnpaired + s_dbgSnapCamera +
                       s_dbgVerbatimEq + s_dbgSnapDecompose + s_dbgSnapGate;
        if (sum != s_dbgPosSeen) {
            fprintf(stderr, "[MP6-FI] CENSUS SUM MISMATCH tick=%ld seen=%u sum=%u "
                            "(rw=%u unpaired=%u cam=%u byteEq=%u decomp=%u gate=%u)\n",
                    cur->tick, s_dbgPosSeen, sum, s_dbgRewritten, s_dbgSnapUnpaired,
                    s_dbgSnapCamera, s_dbgVerbatimEq, s_dbgSnapDecompose, s_dbgSnapGate);
        }
    }
    while (aoIdx < cur->aoCount && cur->ao[aoIdx].offset == pos) {
        s_replayAo[s_replayAoCount] = cur->ao[aoIdx++];
        s_replayAo[s_replayAoCount++].offset = out;
    }
    if (aoIdx != cur->aoCount) return 0;
    return out;
}

int mp6_fi_idle_present(int64_t deadlineNs, int64_t periodNs)
{
    const FiStream *cur, *prev;
    int interp, paced;
    double alpha;
    uint32_t len;
    int64_t t0, now, margin;

    if (!s_active || s_latest < 0 || periodNs <= 0) return fi_decline(FI_DECL_INACTIVE);
    cur = &s_streams[s_latest];
    if (!cur->sealed || !cur->walkOk || cur->size == 0) {
        if (fi_diag()) s_statSkippedWindows++;
        return fi_decline(FI_DECL_NOSTREAM);
    }
    /* Immediate/Mailbox is genuinely uncapped. With no count ceiling there
     * is no burst followed by a cap-induced idle tail to space out. GPU slot
     * availability and the absolute simulation deadline remain authoritative.
     * Preserve the existing paced policy when VSync is enabled. */
    paced = mp6_display_vsync_enabled();
    if (mp6_fi_replay_limit_reached(paced, s_replaysThisWindow))
        return fi_decline(FI_DECL_CAP);
    /* Admission is always evaluated against the throttle's one absolute
     * deadline. A small fixed margin plus the measured replay CPU cost keeps
     * work out of the next simulation tick; the estimate decays on a decline
     * so one transient stall cannot permanently latch replay off. Every stage
     * below re-samples the same deadline after work that can consume slack. */
    /* Admission margin: the pad that absorbs the COST ESTIMATE being wrong,
     * on top of the estimate itself.
     *
     * It used to be periodNs/8 (~2.08ms at 60Hz), described as "one present's
     * worth of headroom" -- but s_replayBudgetNs already covers a whole
     * present, and it is a max-tracking estimator (it latches any larger cost
     * immediately and gives ground only a quarter at a time), so the pad was
     * double-counting the same present. On a light scene that is invisible: a
     * ~14ms window swallows it. On the w01 board it is decisive -- measured
     * there, the tick's own work leaves ~4.3-6.0ms of window against a
     * ~4.5ms replay, so a 2.08ms pad refuses a replay that demonstrably fits
     * (game 6.5 + endframe 4.1 + seal 0.4 + replay 4.5 = 15.5ms of a 16.67ms
     * period) and the board sits at exactly the tick rate.
     *
     * periodNs/32 (~0.52ms at 60Hz) keeps a real pad for estimator error and
     * scheduler jitter without reserving a second frame's worth of time.
     * MP6_FI_MARGIN_DIV overrides the divisor for pacing A/Bs (the lever this
     * value was chosen with); larger = more conservative. */
    margin = periodNs / fi_margin_div();
    now = (int64_t)mp6_host_monotonic_ns();
    {   /* census: the idle room this window actually had to offer */
        int64_t slack = (deadlineNs > now) ? deadlineNs - now : 0;
        if (s_declSlackSamples == 0 || slack < s_declSlackMinNs) s_declSlackMinNs = slack;
        if (slack > s_declSlackMaxNs) s_declSlackMaxNs = slack;
        s_declSlackSumNs += slack;
        s_declSlackSamples++;
    }
    if (!mp6_fi_deadline_fits(deadlineNs, now, margin, s_replayBudgetNs)) {
        fi_budget_decay();
        return fi_decline(FI_DECL_DEADLINE_ENTRY);
    }

    /* In paced mode spread the limited replays across the window. Target one
     * present per period/(cap+1) slot (the real tick present anchors slot
     * 0); when the gap hasn't elapsed, sleep it off HERE rather than
     * declining -- a decline would fall into the throttle's coarse remainder
     * sleep and skip the rest of the window's replays entirely. Under a real
     * blocking vsync the previous present already consumed the slot, so the
     * wait is naturally zero and pacing stays purely vsync-driven. */
    if (paced) {
        int64_t spacing = mp6_fi_replay_spacing_ns(paced, periodNs);
        int64_t target = (s_lastPresentNs > INT64_MAX - spacing)
                             ? INT64_MAX : s_lastPresentNs + spacing;
        if (target > now) {
            int64_t wait = target - now;
            /* The wait may only be paid out of SURPLUS -- the room left after
             * the margin and this replay's own measured cost are set aside.
             *
             * The grid is anchored on the real tick present, but on a heavy
             * scene the idle window does not open until most of the period is
             * already spent: measured on the w01 board, the tick's own work
             * runs ~11.9ms of a 16.67ms period, so slot 1 (present + period/9)
             * still lies ~1.85ms in the future while only ~4.3ms of window
             * remains. Sleeping to it consumed 43% of the entire window and
             * the replay was then declined on the remainder -- the feature
             * spent the only room it had waiting to use it. Clamping the wait
             * to the surplus keeps the anti-bunching behaviour intact wherever
             * there IS surplus (a light scene has ~14ms of window against a
             * ~1ms replay, so the full slot wait is still paid) and simply
             * stops it from evicting the one replay a heavy window can hold. */
            int64_t surplus = deadlineNs - now - margin - s_replayBudgetNs;
            if (surplus <= 0) {
                fi_budget_decay();
                return fi_decline(FI_DECL_DEADLINE_SPACE);
            }
            if (wait > surplus) wait = surplus;
            /* Below the host's sleep granularity a wait buys no spacing and
             * only risks overshooting into the replay's own room -- which is
             * exactly what a surplus-clamped wait tends to be on a heavy
             * scene (tens of microseconds). Skip it rather than gamble the
             * window on Sleep() rounding. */
            if (wait > FI_MIN_SPACING_WAIT_NS) {
                mp6_host_sleep_ns((uint64_t)wait);
            }
        }
    }

    /* The sleep request may overshoot. Re-read the absolute deadline before
     * doing any replay work. */
    t0 = (int64_t)mp6_host_monotonic_ns();
    if (!mp6_fi_deadline_fits(deadlineNs, t0, margin, s_replayBudgetNs)) {
        fi_budget_decay();
        return fi_decline(FI_DECL_DEADLINE_SLEPT);
    }

    /* Don't rebuild/pump a replay the renderer cannot accept. Resource
     * notifications wake this surplus-only wait; no frame or input is
     * consumed here. Re-sample time and interpolation alpha afterward. */
    {
        int ready = aurora_wait_replay_ready(
            (uint64_t)(deadlineNs - t0 - margin - s_replayBudgetNs));
        if (ready <= 0) {
            fi_decline(FI_DECL_BEGINFRAME);
            return (ready < 0 || paced) ? 0 : -1;
        }
        t0 = (int64_t)mp6_host_monotonic_ns();
        if (!mp6_fi_deadline_fits(deadlineNs, t0, margin, s_replayBudgetNs)) {
            fi_budget_decay();
            return fi_decline(FI_DECL_DEADLINE_SLEPT);
        }
    }

    prev = (s_prev >= 0) ? &s_streams[s_prev] : NULL;
    interp = (prev != NULL && prev->sealed && prev->walkOk && cur->posCount > 0);
    if (fi_no_interp()) interp = 0; /* MP6_FI_NO_INTERP bisect: replay verbatim */

    alpha = (double)(t0 - s_lastSealNs) / (double)periodNs;
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    if (fi_diag() >= 3) {
        fprintf(stderr, "[MP6-FI] replay vitick=%ld tick=%ld prevTick=%ld alpha=%.3f interp=%d pos=%u\n",
                mp6_tick_count, cur->tick, prev ? prev->tick : -1L, alpha, interp, cur->posCount);
    }
    len = fi_build_replay(cur, prev, interp, alpha);
    s_costBuildLastNs = (int64_t)mp6_host_monotonic_ns() - t0;
    if (fi_diag() >= 3) {
        fprintf(stderr, "[MP6-FI] built vitick=%ld len=%u pos=%u rewritten=%u unpaired=%u "
                        "camSnap=%u decompSnap=%u gateSnap=%u byteEq=%u "
                        "maxTrans=%.3f minQdot=%.6f maxScale=%.4f (model=%d ord=%d) "
                        "scaleHold=%u maxHeld=%.4f (model=%d ord=%d) | drops: "
                        "offscreen=%u copyExec=%u copyBind=%u destroy=%u other=%u "
                        "posLoads=%u copyClear=%u | maxResid=%.4f (model=%d ord=%d) "
                        "maxA0Err=%.6f (model=%d ord=%d)\n",
                mp6_tick_count, len, s_dbgPosSeen, s_dbgRewritten, s_dbgSnapUnpaired,
                s_dbgSnapCamera, s_dbgSnapDecompose, s_dbgSnapGate, s_dbgVerbatimEq,
                s_dbgMaxTrans, s_dbgMinQdot,
                s_dbgMaxScaleRatio, s_dbgScaleModel, s_dbgScaleOrd, s_dbgScaleHold,
                s_dbgMaxHeldScale, s_dbgHeldModel, s_dbgHeldOrd,
                s_dbgDropOffscreen, s_dbgDropCopyExec, s_dbgDropCopyBind, s_dbgDropDestroy,
                s_dbgDropOther, s_dbgDropPosLoads, s_dbgCopyClear,
                s_dbgMaxRoundTrip, s_dbgRoundTripModel, s_dbgRoundTripOrd,
                s_dbgMaxAlpha0Err, s_dbgAlpha0Model, s_dbgAlpha0Ord);
    }
    if (len == 0) return fi_decline(FI_DECL_BUILD);
    now = (int64_t)mp6_host_monotonic_ns();
    if (!mp6_fi_deadline_fits(deadlineNs, now, margin, fi_budget_remaining(t0, now))) {
        fi_budget_decay();
        return fi_decline(FI_DECL_DEADLINE_BUILT);
    }

    /* Service the OS message pump once per replay frame. The bridge polls
     * the SDL event QUEUE exactly once per tick (aurora_update in
     * VIWaitForRetrace -- input latching semantics stay tick-boundary and
     * are NOT duplicated here); this only transfers pending OS messages
     * into that queue, so DWM/compositor handshakes (window drags, live
     * thumbnails, PrintWindow captures) are serviced at presentation
     * cadence instead of stalling up to a full tick period while the
     * feature is presenting. Feature off = no replays = untouched. */
    SDL_PumpEvents();

    now = (int64_t)mp6_host_monotonic_ns();
    if (!mp6_fi_deadline_fits(deadlineNs, now, margin, fi_budget_remaining(t0, now))) {
        fi_budget_decay();
        return fi_decline(FI_DECL_DEADLINE_PUMPED);
    }

    s_inReplay = 1;
    if (!aurora_try_begin_frame()) {
        s_inReplay = 0;
        /* busy/minimized/surface lost: never wait inside the idle window */
        fi_decline(FI_DECL_BEGINFRAME);
        /* Queue/staging pressure is transient, unlike an exhausted deadline.
         * The caller yields and rechecks the SAME deadline. Do not decay the
         * cost estimate on these retries: GPU pressure says nothing about CPU
         * replay cost and repeated decay would erase the admission guard. */
        return paced ? 0 : -1;
    }
    mp6_ao_begin_frame();
    mp6_launcher_frame_overlay(); /* keep FPS overlay/menu present on every frame */
#ifdef __ANDROID__
    /* Redraw the Android on-screen touch overlay on interpolated frames too.
     * On real frames aurora_bridge.c draws it right after aurora_begin_frame();
     * without this the controls would render only on the 60 real frames/s and
     * vanish on every replay frame in between, strobing at the (refresh - tick)
     * beat -- the exact twin of the RmlUi overlay strobe the line above fixes.
     * ImGui foreground-draw-list only (zero GX contact), and ImGui::NewFrame
     * already ran inside aurora_begin_frame() above, so this is the same valid
     * draw window the real-frame call uses. Compiled out entirely off Android
     * (touch_pad.cpp is Android-windowed only); self-gates to a no-op before
     * its first laid-out frame. */
    { extern void mp6_touch_pad_draw(void); mp6_touch_pad_draw(); }
#endif
    {
        uint32_t offset = 0, i;
        int skipFoliage=0;
        for (i = 0; i < s_replayAoCount; ++i) {
            const FiAoMarker *marker = &s_replayAo[i];
            if (marker->offset > offset && !skipFoliage)
                aurora_gx_submit_raw(s_replayBuf + offset, marker->offset - offset);
            if (marker->kind==3)
                skipFoliage=!mp6_ao_foliage_begin(marker->camera);
            else if (marker->kind==4) {
                if (!skipFoliage) mp6_ao_foliage_end(marker->camera);
                skipFoliage=0;
            } else if (marker->kind)
                mp6_ao_capture_decals(marker->camera, marker->kind == 2);
            else
                mp6_ao_apply(&marker->view);
            offset = marker->offset;
        }
        if (len > offset && !skipFoliage) aurora_gx_submit_raw(s_replayBuf + offset, len - offset);
    }
    /* NO second clock around this aurora_end_frame(). Replay-side submit cost
     * is ALREADY measured: fi_cost_observe() below splits the replay's wall
     * time into BUILD (this file's rewrite, up to s_costBuildLastNs) and
     * PRESENT (everything after it -- the pump, the begin-frame permit and
     * this submit), and that split is the one the admission budget is fed
     * from. A separate bracket here would be a second measurement of the same
     * interval that could disagree with the budget the scheduler actually
     * uses; the console reads s_costPresent* through mp6_fi_stats_get()
     * instead. */
    aurora_end_frame();
    /* MP6_FRAME_DUMP (include/mp6_frame_dump.h): an interpolated frame
     * is a PRESENTED frame -- the user sees it, and this layer is the prime
     * suspect for the reported flicker, so a capture that skipped it would
     * be lying by omission. Flagged replay=1 so the offline diff can tell
     * the two populations apart. Standing no-op unless a burst is armed;
     * while one IS armed the readback stall will blow this window's budget
     * and fi_budget_decay() will throttle replays -- accepted, the capture
     * is a diagnosis burst, not an always-on instrument. */
    { extern void mp6_frame_dump_present(int replayFrame); mp6_frame_dump_present(1); }
    s_inReplay = 0;

    mp6_present_counters_add(1, 1);
    s_replaysThisWindow++;
    if (interp) s_statReplays++; else s_statSnaps++;

    {
        int64_t totalNs = (int64_t)mp6_host_monotonic_ns() - t0;
        fi_budget_observe(totalNs);
        fi_cost_observe(s_costBuildLastNs, totalNs);
    }

    s_lastPresentNs = (int64_t)mp6_host_monotonic_ns(); /* spacing base for the next replay */
    s_declCount[FI_DECL_OK]++;
    return 1;
}

/* =======================================================================
 * Savestate restore hook (src/os/savestate.c). See the carve-out note
 * at the top of this file.
 * ======================================================================= */

void mp6_fi_savestate_reset(void)
{
    /* The TU's statics are carved out (a restore does not clobber them), so
     * the live grow-only buffers stay valid across a load. But the two
     * retained streams describe PRE-restore frames: pairing a pre-restore
     * N-1 against the first post-restore N would extrapolate motion across
     * the state discontinuity for one window. Free the buffers and reset the
     * rotation indices/counters so the first post-restore capture starts
     * clean and re-seeds on the next mp6_fi_note_frame_begin().
     *
     * Safe to call unconditionally on any restore: free(NULL) is a no-op, and
     * s_active (the aurora drain-capture registration -- a live host resource)
     * is deliberately left intact so the sink is neither double-registered nor
     * dropped. Single-threaded with capture/replay (all on the game thread's
     * frame boundary), so no allocation can be in flight here. */
    int i;
    for (i = 0; i < 2; ++i) {
        free(s_streams[i].data);
        free(s_streams[i].chunkEnd);
        free(s_streams[i].posOff);
        free(s_streams[i].posKey);
        free(s_streams[i].prevPos);
        free(s_streams[i].commands);
        memset(&s_streams[i], 0, sizeof(s_streams[i]));
    }
    free(s_pairCache);
    free(s_pairState);
    s_pairCache = NULL;
    s_pairState = NULL;
    s_pairCacheCap = s_pairStateCap = 0;
    fi_pair_cache_invalidate();
    free(s_replayBuf);
    s_replayBuf = NULL;
    s_replayCap = 0;
    s_cur = -1;
    s_latest = -1;
    s_prev = -1;
    s_replaysThisWindow = 0;
    s_sealCounter = 0;
    s_lastSealNs = 0;
    s_lastPresentNs = 0;
    s_replayBudgetNs = 0;

    /* Camera history and model generations are host-state carved out too.
     * Invalidate them so a restored slot cannot pair with the prior timeline. */
    mp6_fi_model_reset();
}
