/* MP6 native port -- freecam (runtime "Mods" page toggle).
 *
 * A basic fly camera that OVERRIDES the game's main 3D camera (camera 0)
 * at the one late point every game-side camera write has already landed:
 * the top of Hu3DExec() (game/hsfman.c, via patches/decomp), which runs
 * AFTER HuPrcCall(1) -- i.e. after omOutView()/Hu3DCameraPosSet() and any
 * scene-specific camera updater -- and BEFORE Hu3DCameraSet() consumes the
 * camera fields to build the frame's projection/view matrices. The game's
 * own camera state is never suppressed: the game keeps writing its camera
 * every tick, freecam just re-writes pos/up/target afterwards. Toggling
 * off restores the last game-written pose once (scenes that only set the
 * camera at load keep it; per-tick writers overwrite it next tick anyway).
 *
 * Split across two TUs, matching the port's established dual-header rule:
 *   - platform/hsf/mp6_freecam.c (decomp headers; BOTH build modes): the
 *     camera write + pose integration. Statics live in the savestate
 *     host carve-out (mp6_host_section.h) -- freecam is a debugging/UX
 *     tool of the RUNNING process, never part of captured game state.
 *   - platform/gx/freecam_input.c (aurora headers; windowed builds only):
 *     turns SDL keyboard/mouse/touch/gamepad state into per-tick deltas
 *     and pushes them through mp6_freecam_host_input(). The headless
 *     build never compiles it and never needs it (freecam can only be
 *     enabled from the launcher UI, which headless does not link).
 *
 * Automation contract (docs/TESTING.md): the ONLY way to enable freecam is
 * the in-game Mods page (launcher mode). Disabled, mp6_freecam_apply() is
 * one branch on a false flag -- no getenv, no log, no camera read -- so
 * every automation/headless run is byte-identical.
 */
#ifndef MP6_FREECAM_H
#define MP6_FREECAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* UI toggle (Mods page). Enabling seeds the fly pose from the game camera
 * on the next apply; disabling restores the game's last written pose. */
void mp6_freecam_set_enabled(int enabled);
int mp6_freecam_enabled(void);

/* Called once per tick from the top of Hu3DExec() (hsfman.c patch), after
 * game logic, before camera consumption. No-op while disabled. */
void mp6_freecam_apply(void);

/* Per-tick input, pushed by the host collector (freecam_input.c) BEFORE
 * the tick's game logic runs (VIWaitForRetrace). Units:
 *   lookYawDeg/lookPitchDeg -- degrees to rotate this tick,
 *   moveRight/moveUp/moveFwd -- world units along the camera axes,
 *   dolly -- world units along forward (wheel/pinch).
 * Accumulates; consumed and cleared by mp6_freecam_apply(). */
void mp6_freecam_host_input(float lookYawDeg, float lookPitchDeg,
                            float moveRight, float moveUp, float moveFwd,
                            float dolly);

#ifdef __cplusplus
}
#endif

#endif /* MP6_FREECAM_H */
