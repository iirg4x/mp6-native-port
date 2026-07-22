/* MP6 native port -- Unlocked FPS, MODEL-level frame interpolation (P1).
 *
 * The "SM64 way": instead of matching one tick's raw GX command stream to the
 * next and rewriting pos matrices (the STREAM path that used to live in
 * platform/gx/frame_interp.c -- fragile because a replayed frame has no object
 * identity, so a portrait that flipped and a bridge that slid could not be told
 * apart and the draw-order match flickered; deleted after this path beat it
 * on-device), this path interpolates at the Hu3D MODEL level.
 *
 * Each real tick we snapshot every LIVE model's transform (pos/rot/scale, keyed
 * by its stable Hu3DData[] slot index -- identity for free) and every live
 * camera. During mp6_tick_throttle_wait()'s idle window we advance that pose
 * FORWARD past the newest snapshot (t = 1 + alpha along the N-1 -> N delta,
 * alpha = the fraction of a tick elapsed since pose N was presented) and
 * RE-RUN Hu3DExec so the renderer re-derives normals/lighting/draw order from
 * the advanced pose -- no draw-order matching, no matrix surgery. Object
 * identity = the slot index, so a per-slot snap decision (spawn/teleport/
 * fast-flip) never touches an innocent neighbour -- and that per-slot snap is
 * what makes extrapolation safe here where it was not in the stream path.
 *
 * Correctness (GATE A -- no game desync): a re-run must advance ZERO game
 * state. The five state-advancing side effects inside Hu3DExec (modelP->tick++,
 * the Hu3DMotionNext loop, HuSprFinish, Hu3DAnimExec, and the shadow/reflect
 * EFB brackets) are wrapped in `if (!mp6_fi_replay_pass)` by the hsfman.c
 * patch, and the vertex-deform block self-skips on a re-run because its
 * HU3D_ATTR_MOT_EXEC gate is cleared ONLY in Hu3DPreProc (never called on a
 * re-run). After the re-run we restore every model's/camera's tick-N transform
 * exactly, so tick N+1's game logic proceeds byte-identically to feature-off.
 *
 * This is the ONLY interpolation path: there is no mode selector and no
 * fallback. The Mods-page "Unlocked FPS" toggle (video.unlocked_fps) drives it
 * directly; feature-off is an exact no-op (no snapshot, no re-run).
 *
 * This TU lives in platform/hsf/ (needs game/hu3d.h, COMMON_FLAGS -- like
 * mp6_freecam.c) and is host-state carved out of savestates (its snapshot
 * buffers belong to the RUNNING process, not captured game state).
 */
#ifndef MP6_FI_MODEL_H
#define MP6_FI_MODEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Replay-pass guard. 1 only while an interpolated Hu3DExec re-run is in flight
 * (set/cleared around the Hu3DExec() call inside mp6_fi_model_replay, on the
 * game thread, in the idle window). The hsfman.c patch externs this and skips
 * the five state-advancing side-effect sites when it is set. Default 0 -- so
 * every normal tick runs those sites unchanged (byte-identical off-proof). */
extern int mp6_fi_replay_pass;

/* Snapshot every live model transform + every live camera at tick N, rotating
 * the prior snapshot down to N-1 (strictly N-1/N retention). Called at
 * note_frame_end when model mode is active and the feature is enabled. */
void mp6_fi_model_snapshot(void);

/* 1 iff both the N-1 and N snapshots are populated (an interpolation can be
 * built this window). */
int mp6_fi_model_ready(void);

/* Present one in-between frame: write pose(N-1 -> N, t = 1 + alpha) -- a
 * FORWARD extrapolation past pose N, alpha bounded to [0,1] -- into every live
 * model + camera, re-run Hu3DExec under mp6_fi_replay_pass, then restore the
 * tick-N state exactly. MUST be called between aurora_begin_frame()/
 * aurora_end_frame().
 *
 * Forward, not backward: the game's own real frame presents pose N untouched by
 * this path, so interpolating BACKWARD (t in [0,1]) made every window present
 * N, then ~N-1, then catch up -- a per-tick sawtooth that judders every moving
 * model (the v212 on-device report). Overshoot is bounded by the per-slot snap
 * gates in mp6_fi_model.c, which the stream path never had. */
void mp6_fi_model_replay(double alpha);

/* 1 while the screen wipe/transition is drawing (wipeData.mode != DUMMY).
 * WipeExecAlways() draws OUTSIDE Hu3DExec (src/game/main.c), so a replay frame
 * cannot contain it and the transition would strobe; the caller skips replays
 * for those ticks. See the block comment in mp6_fi_model.c. */
int mp6_fi_model_wipe_active(void);

/* Drop the retained snapshots (feature-off transition, mode switch, or
 * savestate restore) so the next window does not interpolate across a
 * discontinuity. No-op-safe. */
void mp6_fi_model_reset(void);

/* GATE-A instrument (env MP6_FI_ANIMLOG=1): print a deterministic digest of
 * every live model's tick + motion clocks + transform once per REAL tick.
 * Comparing the digest stream with the feature ON vs OFF proves the re-runs
 * advanced no game state (no anim drift). Silent (one cached getenv) when
 * unset -- never perturbs behavior. Called every real tick from note_frame_end
 * regardless of mode. */
void mp6_fi_model_animlog(long tick);

#ifdef __cplusplus
}
#endif

#endif /* MP6_FI_MODEL_H */
