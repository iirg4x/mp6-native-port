/* MP6 native port -- Unlocked FPS: the idle-window presentation layer.
 *
 * shim/include/mp6_unlocked_fps.h carries the full design contract; this
 * file is the mechanism. Like aurora_bridge.c, it compiles against AURORA's
 * OWN headers (tools/build.py AURORA_FLAGS -- never the decomp include tree)
 * and only for the windowed build (PLATFORM_AURORA_ONLY).
 *
 * Two parts, and only two:
 *   1. HOOKS -- mp6_fi_note_frame_begin/end, called by aurora_bridge.c's
 *      VIWaitForRetrace around the real tick's aurora begin/end pair. The
 *      END hook takes the MODEL snapshot (platform/hsf/mp6_fi_model.c) and
 *      anchors this window's pacing clock.
 *   2. PACING -- mp6_fi_idle_present, called from the tick throttle's idle
 *      window. A plain wall-clock budget (no cost prediction, no EMA, no
 *      latch-able state) decides whether another in-between frame fits, the
 *      per-window spread keeps the presents evenly spaced, and the body is
 *      one aurora begin/submit/end cycle around mp6_fi_model_replay().
 *
 * WHAT USED TO BE HERE. Until the commit that trimmed this file there was a
 * second, STREAM-level mechanism: a fifo drain-capture sink (aurora patch
 * 0015) retained every GX command byte of the last two ticks, a full
 * command-stream walker re-derived every command's length to locate the
 * GXLoadPosMtxImm loads, a TRS decompose/slerp/recompose advanced those
 * matrices and recomputed each paired normal matrix as an inverse-transpose,
 * and a skip-filtered copy of the stream was resubmitted raw. It worked, but
 * a replayed command stream has NO OBJECT IDENTITY -- a portrait that flipped
 * and a bridge that slid are indistinguishable -- so its per-pair snap gates
 * could only ever be global heuristics, and fast movers smeared. The model
 * path has identity for free (the Hu3DData[] slot index), was validated
 * on-device against it, and is now the ONLY path. The stream machinery, its
 * retained buffers and aurora patch 0015 are gone; there is no mode selector
 * and no fallback.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <aurora/aurora.h>
#include <SDL3/SDL.h> /* SDL_PumpEvents in the replay cycle (see mp6_fi_idle_present) */

#include "mp6_unlocked_fps.h"
#include "mp6_fi_model.h" /* the interpolation path itself */
#include "host.h" /* mp6_host_monotonic_ns -- same clock as the tick throttle */

/* SAVESTATE CARVE-OUT. This TU's file-scope statics are
 * the RUNNING process's pacing clock (monotonic timestamps taken from THIS
 * process's timer) plus diagnostic counters -- host state, not captured game
 * state. The savestate image sweep would otherwise restore a capturing
 * process's timestamps into the loading one, producing a nonsense alpha and
 * spacing for the first post-restore window. Carving the TU excludes every
 * file-scope static here from both capture and restore; mp6_fi_savestate_reset()
 * below additionally re-anchors the window and drops the model module's
 * retained motion. Registered in tools/build.py HOST_STATE_SECTION_SOURCES;
 * placed AFTER this file's own includes exactly like platform/gx/shadow_dump.c. */
#include "mp6_host_section.h"

/* --- launcher/bridge seams --- */
extern int mp6_launcher_cfg_unlocked_fps(void); /* launcher_core.cpp; 0 in automation */
extern void mp6_launcher_frame_overlay(void);   /* launcher_core.cpp; replay frames draw the
                                                 * same ImGui/RmlUi overlay as real frames so
                                                 * the FPS counter / in-game menu don't strobe
                                                 * at the tick rate while presents run faster */

/* =======================================================================
 * Config / diagnostics state.
 * ======================================================================= */

#define FI_MAX_REPLAYS_PER_WINDOW 8 /* only ever binds with vsync off/Mailbox; under Fifo
                                     * the present block itself paces to the display */

static int s_diag = -1; /* MP6_FI_DIAG level: 1 = periodic replay diagnostics;
                         * 2 adds one QPC-stamped line per real tick present
                         * (mono_ns is the same QueryPerformanceCounter domain
                         * PowerShell's Stopwatch reads, so an external capture
                         * tool can assign its grabs to exact tick windows) */

static int fi_diag(void)
{
    if (s_diag < 0) {
        const char *env = getenv("MP6_FI_DIAG");
        s_diag = (env != NULL && *env != '\0') ? atoi(env) : 0;
        if (s_diag < 0) s_diag = 0;
    }
    return s_diag;
}

/* Diagnostic A/B lever for the BUG-2 wipe gate below: 1 disables the gate,
 * restoring the pre-fix behavior (replays presented through a transition) so
 * the strobe window can be counted from the SAME binary. Never set in a ship
 * configuration. */
static int fi_nowipegate(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MP6_FI_MODEL_NOWIPEGATE");
        s = (e && *e && *e != '0') ? 1 : 0;
    }
    return s;
}

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
        return s_envState; /* env lever set: it wins outright */
    }
    return mp6_launcher_cfg_unlocked_fps() != 0; /* live config (Mods toggle); 0 in automation */
}

/* =======================================================================
 * Pacing state (the idle window's own clock).
 * ======================================================================= */

static int64_t s_lastSealNs;      /* present timestamp of tick N (alpha reference) */
static int s_replaysThisWindow;
static int64_t s_lastPresentNs;   /* end of the last present (real or replay) -- spacing base */
static long s_statReplays, s_statSkippedWindows;
/* REAL ticks that ran with a wipe/transition on screen, and replay frames that
 * were nonetheless presented during one. A replay frame cannot contain the wipe
 * (it is drawn outside Hu3DExec), so each one is a strobe frame -- the BUG-2
 * verdict is replaysDuringWipe == 0. */
static long s_statWipeTicks, s_statReplaysDuringWipe;
static long s_statBudgetDeclines;  /* windows a replay was refused for not fitting */

/* Admission budget: the measured wall-clock cost of a WHOLE replay --
 * aurora_begin_frame() (which BLOCKS in aurora's frame-slot admission until one
 * of its two in-flight slots frees, for as long as the GPU/vsync takes), the
 * re-run body, and aurora_end_frame(). A replay is only started when the slack
 * still left before the next tick deadline covers this plus the safety margin,
 * so entering that blocking admission can never push the next simulation tick
 * late. Seeded at 0 (the first replay of a run is admitted on the margin alone,
 * which is what measures it).
 *
 * It cannot LATCH the feature off -- the failure mode of the replay-cost EMA
 * this file used to carry. Every refusal decays the budget by 1/8, so even a
 * one-off multi-millisecond stall (a compositor hitch, a driver hiccup) is
 * forgotten within a handful of windows and replays resume on their own; no
 * successful replay is needed to recover, which is exactly what the EMA got
 * wrong (its estimate only refreshed on success, so an inflated estimate
 * blocked the very replays that would have corrected it). Growth is immediate
 * (a cost above budget replaces it), shrink is gradual, so the common case
 * tracks the real cost without chasing noise. */
static int64_t s_replayBudgetNs;

static void fi_budget_observe(int64_t costNs)
{
    if (costNs > s_replayBudgetNs) {
        s_replayBudgetNs = costNs;
    } else {
        s_replayBudgetNs -= (s_replayBudgetNs - costNs) / 4;
    }
}

static void fi_budget_decay(void)
{
    s_replayBudgetNs -= s_replayBudgetNs / 8;
    if (s_replayBudgetNs < 0) s_replayBudgetNs = 0;
}

/* =======================================================================
 * Frame-boundary hooks (called from aurora_bridge.c).
 * ======================================================================= */

void mp6_fi_note_frame_begin(void)
{
    /* Deliberately empty. The hook point is kept because it is the ONE place
     * that runs right after a successful aurora_begin_frame() on a real tick;
     * the stream path used it to arm its capture sink, and the model path
     * needs nothing there (its snapshot is a frame-END operation -- it must
     * see the pose the frame just presented). Keeping the seam costs a call
     * and means a future per-tick pre-frame need has somewhere to live. */
}

void mp6_fi_note_frame_end(void)
{
    if (fi_diag() >= 2) {
        /* One QPC stamp per REAL tick present, feature on or off (the bridge
         * calls this after every aurora_end_frame) -- external capture tools
         * assign their grabs to exact tick windows with it. */
        static long s_presentStamp;
        fprintf(stderr, "[MP6-FI] present n=%ld mono_ns=%lld\n",
                ++s_presentStamp, (long long)mp6_host_monotonic_ns());
    }

    /* GATE-A instrument, every REAL tick regardless of enable: a digest of
     * live model tick+anim-clock+transform state. Comparing the stream with the
     * feature ON vs OFF proves the idle-window re-runs advanced no game state.
     * Cached-getenv no-op when MP6_FI_ANIMLOG is unset. */
    {
        static long s_animTick;
        mp6_fi_model_animlog(++s_animTick);
    }

    if (!mp6_unlocked_fps_enabled()) {
        mp6_fi_model_reset(); /* dropped feature -> no cross-discontinuity interp */
        return;
    }

    /* Snapshot tick N's transforms/cameras (rotating N-1/N) and anchor the
     * idle-window pacing clock: the real present just happened, so it is both
     * the alpha reference and slot 0 of this window's spacing grid. */
    mp6_fi_model_snapshot();
    s_lastSealNs = (int64_t)mp6_host_monotonic_ns();
    s_lastPresentNs = s_lastSealNs;
    s_replaysThisWindow = 0;
    if (mp6_fi_model_wipe_active()) s_statWipeTicks++;
    if (fi_diag()) {
        static long s_tick;
        if ((++s_tick % 300) == 0) {
            fprintf(stderr, "[FI-MODEL] tick=%ld replays=%ld skippedWindows=%ld budgetDeclines=%ld "
                            "budget=%.3f ms | wipe: ticks=%ld replaysDuringWipe=%ld\n",
                    s_tick, s_statReplays, s_statSkippedWindows, s_statBudgetDeclines,
                    (double)s_replayBudgetNs / 1e6,
                    s_statWipeTicks, s_statReplaysDuringWipe);
            fflush(stderr);
        }
    }
}

/* =======================================================================
 * Replay: one in-between frame inside the tick throttle's idle window.
 * ======================================================================= */

int mp6_fi_idle_present(int64_t remainNs, int64_t periodNs)
{
    int64_t t0, margin, deadline, need;
    double alpha;

    if (!mp6_unlocked_fps_enabled() || periodNs <= 0) return 0;
    if (!mp6_fi_model_ready()) {
        if (fi_diag()) s_statSkippedWindows++;
        return 0;
    }
    /* Wipe/transition active: the wipe is drawn by WipeExecAlways() AFTER
     * Hu3DExec in the main loop, so a replay frame physically cannot contain
     * it -- interpolating through a transition strobes the overlay on/off at
     * the refresh-minus-tick beat. Present only the real frames for those
     * ticks (plain 60Hz, exactly as before the feature existed). */
    if (mp6_fi_model_wipe_active() && !fi_nowipegate()) return 0;
    if (s_replaysThisWindow >= FI_MAX_REPLAYS_PER_WINDOW) return 0;

    /* Plain wall-clock budget -- no cost prediction, no EMA, no per-window
     * latch-able state. Present another interpolated frame only while a small
     * fixed safety margin of slack still remains before the next tick deadline;
     * remainNs is exactly that slack (deadline - now), handed in by
     * mp6_tick_throttle_wait. The throttle loop re-reads the clock and calls
     * again after each present, so this simply fills whatever slack is left and
     * an expensive present just ends the window one frame early on the next
     * call. This REPLACES the former replay-cost EMA governor, whose estimate
     * refreshed only after a successful replay: one transient present stall
     * could inflate it past the fit budget and latch the present rate to the
     * tick rate indefinitely (commit a6ed8dc's clamp/bleed were band-aids on
     * that trap). A pure wall-clock budget cannot latch by construction. */
    margin = periodNs / 8; /* ~2ms at 60Hz -- one present's worth of headroom */
    if (remainNs < margin) return 0;
    /* The caller's remainNs is a sample taken before this call; everything below
     * re-derives its slack from an ABSOLUTE deadline instead, because the two
     * steps in between (the spacing sleep, and the wait inside aurora's frame
     * admission) both consume unknown amounts of it. */
    deadline = (int64_t)mp6_host_monotonic_ns() + remainNs;
    need = margin + s_replayBudgetNs;

    /* Spread replays across the window instead of bursting them at its start:
     * with vsync OFF (Mailbox/immediate) a present returns in microseconds, so
     * without spacing all FI_MAX_REPLAYS_PER_WINDOW frames would land at
     * alpha ~= 0 and the window's tail would sit static -- the exact judder the
     * feature exists to remove. Target one present per period/(cap+1) slot (the
     * real tick present anchors slot 0); when the gap hasn't elapsed, sleep it
     * off HERE rather than declining -- a decline would fall into the throttle's
     * coarse remainder sleep and skip the rest of the window's replays entirely.
     * Under a real blocking vsync the previous present already consumed the
     * slot, so the wait is naturally zero and pacing stays purely vsync-driven. */
    {
        int64_t spacing = periodNs / (FI_MAX_REPLAYS_PER_WINDOW + 1);
        int64_t now = (int64_t)mp6_host_monotonic_ns();
        int64_t wait = (s_lastPresentNs + spacing) - now;
        if (wait > 0) {
            if ((deadline - now) - wait < need) return 0; /* no slack left after the wait */
            mp6_host_sleep_ns((uint64_t)wait);
        }
    }

    /* RE-CHECK the clock after the sleep. mp6_host_sleep_ns is a request, not a
     * promise -- OS timer granularity routinely overshoots it -- and the check
     * above was only a prediction. Without this, an oversleep walked straight
     * into the blocking frame admission below and pushed the next tick late.
     * The budget term is what keeps that admission itself from overrunning:
     * a replay is started only while the REMAINING slack still covers a whole
     * measured replay, so the next simulation deadline is never at its mercy. */
    t0 = (int64_t)mp6_host_monotonic_ns();
    if (deadline - t0 < need) {
        fi_budget_decay();
        if (fi_diag()) s_statBudgetDeclines++;
        return 0;
    }

    alpha = (double)(t0 - s_lastSealNs) / (double)periodNs;
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;

    /* Service the OS message pump once per replay frame. The bridge polls the
     * SDL event QUEUE exactly once per tick (aurora_update in VIWaitForRetrace
     * -- input latching semantics stay tick-boundary and are NOT duplicated
     * here); this only transfers pending OS messages into that queue, so
     * DWM/compositor handshakes (window drags, live thumbnails, PrintWindow
     * captures) are serviced at presentation cadence instead of stalling up to
     * a full tick period while the feature is presenting. Feature off = no
     * replays = untouched. */
    SDL_PumpEvents();

    if (!aurora_begin_frame()) {
        return 0; /* minimized/surface lost: no replay this window */
    }
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
    /* GATE-B cost: time ONLY the re-run body (lerp + Hu3DExec + restore) -- the
     * added CPU per in-between frame. aurora_end_frame's vsync-blocked present
     * is deliberately outside the timer (it is not added CPU). Reported every
     * 300 replays under MP6_FI_DIAG so the device measurement can read
     * ms/replay directly from the log. */
    {
        int64_t rb = (int64_t)mp6_host_monotonic_ns();
        mp6_fi_model_replay(alpha); /* lerp models+cameras -> re-run Hu3DExec -> restore */
        if (fi_diag()) {
            static int64_t s_sumNs, s_maxNs; static long s_n;
            int64_t d = (int64_t)mp6_host_monotonic_ns() - rb;
            s_sumNs += d; if (d > s_maxNs) s_maxNs = d; s_n++;
            if ((s_n % 300) == 0) {
                fprintf(stderr, "[FI-MODEL] replay body cost: avg=%.3f ms max=%.3f ms over %ld replays\n",
                        (double)s_sumNs / (double)s_n / 1e6, (double)s_maxNs / 1e6, s_n);
                fflush(stderr);
            }
        }
    }
    aurora_end_frame();

    mp6_present_counters_add(1, 1);
    s_replaysThisWindow++;
    s_statReplays++;
    /* Feed the admission budget with what a WHOLE replay actually cost from
     * here (t0, immediately before aurora_begin_frame's blocking admission)
     * to now -- the exact quantity the next window has to fit. */
    fi_budget_observe((int64_t)mp6_host_monotonic_ns() - t0);
    /* BUG-2 verdict counter: a replay frame that reached the screen while the
     * wipe was drawing is a strobe frame (the wipe quad is missing from it).
     * With the gate on this is unreachable and must stay 0. */
    if (mp6_fi_model_wipe_active()) s_statReplaysDuringWipe++;
    s_lastPresentNs = (int64_t)mp6_host_monotonic_ns();
    return 1;
}

/* =======================================================================
 * Savestate restore hook (platform/os/savestate.c). See the carve-out note
 * at the top of this file.
 * ======================================================================= */

void mp6_fi_savestate_reset(void)
{
    /* The TU's statics are carved out (a restore does not clobber them), so
     * this only has to put the window back into its "nothing retained yet"
     * shape. Safe to call unconditionally on any restore; single-threaded with
     * the hooks and the replay body (all on the game thread's frame boundary).
     *
     * The retained N-1/N transform snapshots describe PRE-restore frames, and
     * pairing one against the first post-restore tick would interpolate across
     * the state discontinuity. The model module's snapshot buffers are host-state
     * carved out too (never clobbered by the image sweep), so dropping the
     * retention flags is all that is needed. */
    s_replaysThisWindow = 0;
    s_lastSealNs = 0;
    s_lastPresentNs = 0;
    s_replayBudgetNs = 0; /* pre-restore costs describe a different scene */
    mp6_fi_model_reset();
}
