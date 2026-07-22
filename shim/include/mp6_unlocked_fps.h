/* MP6 native port -- Mods-page "Unlocked FPS" (tick-decoupled presentation).
 *
 * Game logic stays a fixed 60Hz tick (aurora_bridge.c section 8's throttle
 * -- untouched). Presentation is decoupled: during
 * mp6_tick_throttle_wait()'s idle window -- after tick N's own present,
 * before tick N+1's deadline -- extra frames are presented with the scene
 * advanced along the last two ticks' motion.
 *
 * MECHANISM: MODEL-level interpolation, and only that
 * (shim/include/mp6_fi_model.h has the full contract; the mechanism lives
 * in platform/hsf/mp6_fi_model.c, the pacing in platform/gx/frame_interp.c).
 * Each real tick snapshots every LIVE Hu3D model's transform (pos/rot/scale,
 * keyed by its stable Hu3DData[] slot index) and every live camera. Each
 * in-between frame writes pose(N-1 -> N, t = 1 + alpha) into those slots --
 * a FORWARD extrapolation past the already-presented pose N, alpha =
 * (now - tickN_present)/tick_period bounded to [0,1] -- and RE-RUNS Hu3DExec
 * so the renderer re-derives matrices, normals, lighting and draw order from
 * the advanced pose. Afterwards every model's/camera's tick-N state is
 * restored exactly, so tick N+1's game logic proceeds byte-identically to
 * feature-off (GATE A; the hsfman.c patch's six `if (!mp6_fi_replay_pass)`
 * guards suppress the state-advancing side effects inside Hu3DExec, and
 * MP6_FI_MODEL_ASSERT=1 verifies the invariant in-process).
 *
 * Because the interpolation is per-OBJECT, a spawn/teleport/fast-flip is
 * snapped per slot (>50u or >10deg in one tick) without touching its
 * neighbours -- the property a stream-level replay could never have. 2D
 * layers draw raw immediate-mode vertices (game/sprput.c) and the screen
 * wipe is drawn outside Hu3DExec, so UI, effects and transitions stay 60Hz
 * by construction -- the settings row says so honestly, and replays are
 * suppressed outright while a wipe is up (they cannot contain it).
 *
 * HISTORY, so the shape of this file makes sense: a second, STREAM-level
 * mechanism used to exist behind a mode selector (retained GX command
 * streams + a command-stream walker + pos/nrm matrix surgery, fed by aurora
 * patch 0015's fifo drain-capture sink). It was evaluated on-device against
 * the model path and lost; it, its config/UI selector and patch 0015 have
 * all been deleted. There is no mode lever and no fallback.
 *
 * Pacing: none beyond alpha and a plain wall-clock fit check. aurora's
 * render worker (FrameSlotCount=2) backpressures begin/end pairs against
 * the vsync'd present queue, so the replay loop self-paces to the display;
 * a per-window replay cap plus an even spread across the window keep the
 * vsync-off diagnostic configuration bounded and non-bursty.
 *
 * Levers (same shape as MP6_SHADOW_QUALITY/MP6_WIDESCREEN):
 *   - Mods-page "Unlocked FPS" toggle -> video.unlocked_fps
 *     (launcher_core.cpp; launcher mode only -- automation never reads the
 *     config, docs/TESTING.md's sacred contract).
 *   - MP6_UNLOCKED_FPS env: set -> it WINS over the config ("0" forces
 *     off, anything else on); needed because automation mode never reads
 *     mp6_config.json, so no scripted gate could exercise the feature
 *     without it.
 *   - MP6_PRESENT_RATE_LOG=1 (aurora_bridge.c, beside MP6_TICK_RATE_LOG):
 *     ~5s windowed present-rate lines + a final begins/presents/ticks
 *     summary at clean shutdown -- the off-proof (present-count ==
 *     tick-count) and on-proof (present-rate >> tick-rate) instrument.
 *   - MP6_FI_DIAG=1: periodic replay/wipe diagnostics + per-replay body
 *     cost; =2 adds one QPC-stamped line per real tick present.
 *   - MP6_FI_MODEL_ASSERT=1 / MP6_FI_ANIMLOG=1 / MP6_FI_POSELOG=1 and the
 *     MP6_FI_MODEL_BACKLERP / MP6_FI_MODEL_NOWIPEGATE A/B levers: see
 *     shim/include/mp6_fi_model.h. All off by default, all no-ops unset.
 *
 * OFF (default) is an EXACT no-op: no snapshot is taken, VIWaitForRetrace
 * performs zero extra begin/end-frame pairs, and present-count ==
 * tick-count.
 *
 * Windowed (aurora) builds only: platform/gx/frame_interp.c is listed in
 * tools/build.py's PLATFORM_AURORA_ONLY (same split as aurora_bridge.c);
 * --headless never compiles any of this, so the headless gate is untouched
 * by construction. (mp6_fi_model.c itself compiles in both modes -- it
 * needs game/hu3d.h -- but nothing calls its replay path headless.) */
#ifndef MP6_UNLOCKED_FPS_H
#define MP6_UNLOCKED_FPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolved enable state: MP6_UNLOCKED_FPS env when set (parsed once),
 * else the launcher config (live -- the Mods toggle applies immediately),
 * else 0. Automation mode without the env lever always resolves 0. */
int mp6_unlocked_fps_enabled(void);

/* Frame-boundary hooks, called by aurora_bridge.c's VIWaitForRetrace:
 * ..._frame_begin right after aurora_begin_frame() succeeds (currently a
 * reserved no-op -- the seam is kept because it is the one per-tick point
 * inside an open frame); ..._frame_end right after aurora_end_frame()
 * returns (takes tick N's model/camera snapshot, rotates the N-1/N
 * retention pair, and timestamps the present for alpha + spacing). Both are
 * cheap no-ops while the feature is off. */
void mp6_fi_note_frame_begin(void);
void mp6_fi_note_frame_end(void);

/* One replay attempt inside the tick throttle's wait loop. remainNs is the
 * time left until the next tick deadline, periodNs the tick period.
 * Returns 1 if an interpolated frame was begun+presented (caller re-reads
 * the clock and loops), 0 if it declined (feature off, snapshots not ready,
 * wipe on screen, window too small, per-window cap reached) -- caller falls
 * through to its normal sleep/spin. */
int mp6_fi_idle_present(int64_t remainNs, int64_t periodNs);

/* aurora_bridge.c's present accounting (MP6_PRESENT_RATE_LOG): the replay
 * path reports each extra begin/end pair through this, the bridge counts
 * its own real frames itself. */
void mp6_present_counters_add(long begins, long ends);

/* SAVESTATE restore hook (platform/os/savestate.c). frame_interp.c's statics
 * are carved out of the savestate image (they are the RUNNING process's pacing
 * clock -- see the TU's header comment), so a load leaves them intact; this
 * re-anchors the window and drops the retained N-1/N model snapshots so the
 * first post-restore window does not interpolate across the state
 * discontinuity. No-op-safe: valid to call whether or not the feature is on. */
void mp6_fi_savestate_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_UNLOCKED_FPS_H */
