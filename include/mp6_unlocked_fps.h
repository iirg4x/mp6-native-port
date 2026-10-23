/* MP6 native port -- Mods-page "Unlocked FPS" presentation replay.
 *
 * Simulation remains a fixed 60 Hz.  Each real frame's complete Aurora GX
 * FIFO stream is retained; idle-window presents submit a filtered copy of the
 * bytes, rewriting only position matrices whose explicit Hu3D identity and
 * camera history match the previous real frame.  Replays do not call
 * Hu3DExec, layer hooks, model hooks, material hooks, animation code, or any
 * other game callback.  Consequently callback-owned BSS/heap state advances
 * exactly once per simulation tick.
 *
 * Identity is (camera, model slot, creation generation, matrix ordinal), not
 * command order.  Slot reuse and draw-order changes therefore snap instead of
 * cross-pairing.  Motion in model->mtx is naturally retained because the
 * actual emitted matrices are compared.  Camera cuts consider position,
 * target, up, and FOV; Freecam works through the same real-tick camera stream.
 *
 * PAIRING RULE.  A matrix is named by
 *
 *     (camera, model slot, creation generation, sub, ordinal)
 *
 * and NEVER by its position in the retained stream.  `sub` names the EMISSION
 * BRACKET: 0xFFFF for an ordinary matrix emitted inside the model bracket,
 * or the deferred draw object's PUSH-ORDER rank. HSF mesh matrix loads have
 * stronger identities: 0x4000 + authored object index for the visible draw,
 * and 0x8000 + the same index for its private AO copies (indices < 0x4000).
 * A one-shot hint labels only the mesh matrix load, not material hooks or
 * subsequent unrelated commands.
 * It is not optional.  Hu3DDraw does not emit every object where it decides to
 * draw it -- translucent / NEAR / ALTBLEND / z-compare-off objects and every
 * hook-func (particle) model are pushed onto the z-sorted DrawObjData list and
 * emitted later by Hu3DDrawPost, after mp6_fi_capture_model_end() and in DEPTH
 * order.  On the w01 board that is two thirds of a frame's position matrices.
 * Reserving identity at push time, where the model bracket is still open, and
 * ranking it by push order is what makes pairing immune to that depth sort,
 * which re-permutes emission order on every camera move.
 *
 * Authored object keys use a per-object integrity group, so an unrelated mesh
 * entering/leaving the camera cannot pin a still-visible object to tick N.
 * The visible mesh and AO copies share the same group. Missing/duplicate
 * members or different visible/private matrix payloads reject the whole
 * group; their interpolation decisions must never diverge. Hook and other
 * ordinal-based keys retain the conservative whole-model count guard.
 *
 * Layer hooks (Hu3DLayerHookSet) draw inside a camera but own no model slot;
 * they take a pseudo-slot identity above HU3D_MODEL_MAX, generation-stamped
 * from the installed function pointer.  The invariant to hold on to is that
 * EVERY position matrix emitted inside a 3D camera bracket carries a key --
 * MP6_FI_DIAG>=3's `incam` counter is zero exactly when that holds.  What is
 * left keyless is screen-space (sprites, wipes, the shadow/reflect passes),
 * which a camera move cannot tear.
 *
 * WHY UNPAIRED MATRICES ARE A VISIBLE DEFECT AND NOT MERELY A LOST
 * OPTIMISATION.  Every pos matrix is a MODELVIEW, so a camera pan moves all of
 * them together.  If some advance by alpha and some are pinned to tick N, the
 * two populations tear apart by alpha x the pan speed for exactly one presented
 * frame and snap back on the next tick -- an A-B-A flicker that has no
 * counterpart while the camera is still.  That is why the idle board and mode
 * select were clean while the board's opening pan was not; see
 * docs/history/F3_LEAF_STROBE.md section 7 for the measurement.
 *
 * DEGRADATION IS ONE-DIRECTIONAL.  Every rejection -- no identity, a group
 * whose pos-load membership changed since the previous tick, an ambiguous
 * duplicate key, an unstable camera, a pair failing the motion gate -- emits
 * stream N's own bytes, i.e. that object presents UN-INTERPOLATED for that one
 * window.  Nothing in the pairing path can remove a draw; the skip filter that
 * can is decided from the command's opcode alone, before any pairing state is
 * read.  MP6_FI_DIAG>=3 prints the per-tick split (paired / nokey / countdiff /
 * dup / unmatched), which joins 1:1 with a MP6_FRAME_DUMP index.csv row.
 *
 * Replay frame admission uses Aurora's non-blocking try-begin API. Absolute
 * deadline checks are repeated after spacing, stream rewrite, and OS-event
 * pumping, and frame-slot, mapped-staging, and render-queue permits are all
 * acquired with try operations. A replay therefore never enters a begin-frame
 * resource wait inside the simulation deadline window.
 *
 * MP6_UNLOCKED_FPS is the environment override (0 disables, any other value
 * enables); otherwise the live launcher setting is used.  MP6_FI_DIAG and
 * MP6_FI_ANIMLOG retain the existing diagnostic surfaces.
 *
 * MATRIX REWRITE CONTRACT. A replayed pos matrix is built from the tick matrix
 * B and its predecessor A as
 *
 *     O(alpha) = compose(advance(decompose(A), decompose(B), alpha))
 *                + ( B - compose(decompose(B)) )
 *
 * The trailing term is the RESIDUAL CARRY and it is not optional. TRS
 * decomposition is exact only for a similarity transform; a modelview carrying
 * shear (non-uniform parent scale times a child rotation) loses that shear in
 * the round trip, so without the carry O(0) != B and every interpolated present
 * re-poses the object and snaps back on the next tick -- a flicker whose size
 * is set by the decomposition error, not by motion, and which therefore no
 * motion gate can catch. With the carry O(0) == B exactly: a replay frame can
 * never disagree with the tick frame it was built from.
 *
 * THE THREE CHANNELS ARE GATED SEPARATELY, AND SCALE IS A HOLD RATHER THAN A
 * SNAP. A replay evaluates at t = 1+alpha, i.e. it EXTRAPOLATES the last tick's
 * motion, so each TRS channel needs a bound above which one tick of forward
 * overshoot stops being imperceptible:
 *
 *   translation  |dt| >= FI_TRANS_SNAP_U (50u)      -> snap the whole pair
 *   rotation     |qdot| <= FI_ROT_SNAP_QDOT (~10deg)-> snap the whole pair
 *   scale        max_i max(sb_i/sa_i, sa_i/sb_i) - 1 >= FI_SCALE_SNAP_RATIO
 *                -> HOLD the scale channel (alphaS = 0) and keep advancing
 *                   translation and rotation
 *
 * Scale had no gate and no diagnostic at all until the w01 dice bloom was
 * measured (src/board/dice.c DiceObjOMExec case 2: a sin-driven 1.0 -> 2.0x
 * squash over 12 ticks, whose per-tick Y hop of 38.8u sits UNDER the
 * translation gate), so the bloom extrapolated at full strength and the tick
 * that ENDS it was carried past the end of a stopped animation. The gate is a
 * per-channel hold and not a snap because translation and rotation are already
 * bounded by their own gates: snapping the whole matrix would pin the object to
 * the tick rate while the scene keeps advancing, which is the pan-phase defect
 * of docs/history/F3_LEAF_STROBE.md section 7 seen from the other side. All
 * three scale columns are held together -- the dice grows in x/z as it squashes
 * in y, so freezing one column and advancing the others would distort the shape
 * rather than freeze it.
 *
 * The scale gate is camera-independent BY PROOF: a pos matrix is camera x model
 * and the Hu3D view matrix is rigid, so the left-multiply preserves column
 * lengths and the decomposed scale is the model's own. A camera move cannot
 * move this ratio (unlike the translation gate, which sees a pan), and a hold
 * is a no-op at alpha = 0, so O(0) == B is unaffected.
 *
 * Bisect levers, diagnosis only, never set in a real run:
 *   MP6_FI_NO_INTERP=1     keep the replay cadence, submit the stream verbatim.
 *   MP6_FI_NO_RESIDUAL=1   drop the residual carry (reinstates the defect).
 *   MP6_FI_NO_SCALE_HOLD=1 drop the scale hold (reinstates the bloom overshoot).
 *   MP6_FI_SCALE_SNAP=<r>  override FI_SCALE_SNAP_RATIO for a calibration sweep.
 *   MP6_FI_RETAINED=0      decode every replay through the FIFO (no reuse).
 *   MP6_FI_RETAINED_VERIFY=1 decode every would-be reuse anyway and count
 *                          any difference from the reused result.
 *
 * Retained replay (compat/aurora/windows/0006): a replay without AO hooks is
 * handed to aurora_gx_replay_stream as one stream. The first replay of a tick
 * decodes and records it; later replays of the same tick re-emit those draws
 * with only the matrix uniforms patched, when aurora can show that equals a
 * full decode (stream bytes outside the matrix payloads, starting GX state and
 * frame position all unchanged). MP6_FI_DIAG>=1 prints the reuse census.
 * MP6_FI_DIAG>=3 reports, per replay, maxResid (the shear being carried),
 * maxA0Err (|O(0) - B|, which must stay at zero), maxScale with the model that
 * produced it, scaleHold (pairs the scale gate acted on), and a six-bucket
 * rewrite census -- rewritten / unpaired / camSnap / decompSnap / gateSnap /
 * byteEq -- which is asserted to PARTITION the position loads seen, so no
 * rewrite decision can go unnamed.
 */
#ifndef MP6_UNLOCKED_FPS_H
#define MP6_UNLOCKED_FPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mp6_unlocked_fps_enabled(void);

/* How long the real tick frame waited to open (aurora_begin_frame / the
 * Android frame gate). Replays that keep the render queue full make the
 * tick frame wait for a free slot, which delays it and makes tick frames
 * land at uneven times; frame_interp.c reserves that much of the replay
 * window so the queue has drained by the deadline. */
void mp6_fi_note_begin_wait(int64_t waitNs);

/* When this tick's game slice started (the previous VIWaitForRetrace
 * return) and ended (this one's entry); recorded with the tick frame. */
void mp6_fi_note_tick_times(int64_t gameStartNs, int64_t gameEndNs);

/* Smooth mode (MP6_FI_SMOOTH=1). Called right before the real tick frame's
 * begin; returns 1 when that frame should be hidden (aurora_hide_next_frame:
 * recorded, but passes nothing reads are dropped and nothing is presented). The display then runs one tick
 * behind: tick N's window shows replays interpolated BETWEEN ticks N-1 and N
 * (t = alpha) instead of extrapolated past N (t = 1+alpha), so nothing is
 * predicted and nothing overshoots, for one tick of extra latency. Smooth
 * mode starts once a window presents a replay and stops after 4 windows in a
 * row without one, so a scene too heavy for replays falls back to showing
 * every tick frame without flipping between the two modes tick to tick. */
int mp6_fi_smooth_hide_tick(void);

/* Resolved scheduler policy, not the saved Unlocked FPS preference. Free-run
 * has no replay window, so retain no FIFO stream until pacing resumes. */
int mp6_tick_interpolation_possible(void);

/* Real-frame hooks around the Aurora begin/end pair. */
void mp6_fi_note_frame_begin(void);
void mp6_fi_note_frame_end(void);

/* Windowed GXLoadPosMtxImm bridge hook.  Records identity metadata only while
 * a real-frame stream capture is armed; raw replay never calls it. */
void mp6_fi_stream_note_pos_mtx(void);

/* Attempt one idle-window replay before the caller's absolute monotonic-clock
 * deadline. Returns one only after a frame was actually presented; zero means
 * the caller should continue its normal wait. Minus one means transient
 * nonblocking queue/staging pressure: yield and retry against the SAME
 * deadline, never advance simulation or manufacture more slack.
 * Passing the absolute deadline
 * prevents time spent between the throttle's sample and admission from being
 * accidentally added back onto the simulation window. */
int mp6_fi_idle_present(int64_t deadline_ns, int64_t period_ns);

void mp6_present_counters_add(long begins, long ends);

/* Drop retained FIFO/camera/identity history across a savestate discontinuity. */
void mp6_fi_savestate_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_UNLOCKED_FPS_H */
