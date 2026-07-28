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
 * BRACKET: 0xFFFF for a matrix emitted inside the model bracket itself,
 * otherwise the deferred draw object's PUSH-ORDER rank within that model.
 * It is not optional.  Hu3DDraw does not emit every object where it decides to
 * draw it -- translucent / NEAR / ALTBLEND / z-compare-off objects and every
 * hook-func (particle) model are pushed onto the z-sorted DrawObjData list and
 * emitted later by Hu3DDrawPost, after mp6_fi_capture_model_end() and in DEPTH
 * order.  On the w01 board that is two thirds of a frame's position matrices.
 * Reserving identity at push time, where the model bracket is still open, and
 * ranking it by push order is what makes pairing immune to that depth sort,
 * which re-permutes emission order on every camera move.
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
 * Bisect levers, diagnosis only, never set in a real run:
 *   MP6_FI_NO_INTERP=1     keep the replay cadence, submit the stream verbatim.
 *   MP6_FI_NO_RESIDUAL=1   drop the residual carry (reinstates the defect).
 * MP6_FI_DIAG>=3 reports, per replay, maxResid (the shear being carried) and
 * maxA0Err (|O(0) - B|, which must stay at zero).
 */
#ifndef MP6_UNLOCKED_FPS_H
#define MP6_UNLOCKED_FPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mp6_unlocked_fps_enabled(void);

/* Real-frame hooks around the Aurora begin/end pair. */
void mp6_fi_note_frame_begin(void);
void mp6_fi_note_frame_end(void);

/* Windowed GXLoadPosMtxImm bridge hook.  Records identity metadata only while
 * a real-frame stream capture is armed; raw replay never calls it. */
void mp6_fi_stream_note_pos_mtx(void);

/* Attempt one idle-window replay before the caller's absolute monotonic-clock
 * deadline. Returns one only after a frame was actually presented; zero means
 * the caller should continue its normal wait. Passing the absolute deadline
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
