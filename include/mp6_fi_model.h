/* MP6 native port -- identity metadata for side-effect-free frame replay.
 *
 * Unlocked-FPS replays never call Hu3DExec.  The real tick's GX stream is
 * retained and submitted again, with selected matrix payloads rewritten.
 * This module supplies the information a byte stream does not carry itself:
 * stable model generations, the active model/camera draw context, and the
 * previous/current camera poses used to reject camera cuts.  All state here is
 * host render state and is carved out of savestates.
 */
#ifndef MP6_FI_MODEL_H
#define MP6_FI_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A slot generation changes on every model/link/hook-model creation.  Pairing
 * keys include it, so a killed model and a new model reusing the same slot can
 * never inherit each other's matrix history. */
void mp6_fi_model_identity_new(int model_id);

/* Hu3DExec brackets real draws with this context.  The GXLoadPosMtxImm bridge
 * consumes one ordinal for each matrix load and records
 * (camera, slot, generation, sub, ordinal) beside the retained FIFO stream. */
void mp6_fi_capture_context_reset(void);
void mp6_fi_capture_camera(int camera_id);
void mp6_fi_capture_model_begin(int model_id);
void mp6_fi_capture_model_end(void);
void mp6_fi_capture_auxiliary_begin(int model_id, int object_index);
int mp6_fi_capture_context_next(int *camera_id, int *model_id,
                                uint32_t *generation, uint16_t *sub,
                                uint16_t *ordinal);

/* The open camera bracket, or -1.  Recorded on matrices that have no model
 * identity too, so the pairing diagnostics can tell an unidentified draw that
 * is still INSIDE a 3D camera (a layer hook -- it moves with a pan, so pinning
 * it to tick N is visible) from one outside every camera (sprites, wipes, the
 * shadow/reflect passes -- screen-space, so pinning it is not). */
int mp6_fi_capture_camera_id(void);

/* DEFERRED-DRAW IDENTITY (the pan-phase defect, docs/history/F3_LEAF_STROBE.md
 * section 7).  Hu3DDraw does not emit every object where it decides to draw it:
 * translucent / NEAR / ALTBLEND / z-compare-off objects and every hook-func
 * (particle) model are pushed onto the z-sorted DrawObjData list and emitted
 * later by Hu3DDrawPost -- which runs AFTER mp6_fi_capture_model_end() and in
 * DEPTH order, not push order.  On the w01 board that is two thirds of the
 * frame's position matrices, and without identity none of them can ever pair:
 * they are pinned to tick N on every interpolated present while the bracketed
 * third advances, which is invisible with a still camera and is the whole
 * artifact during a pan.
 *
 * Identity is therefore RESERVED at push time, where the model bracket is still
 * open, and re-installed at emit time:
 *   reset  -- Hu3DDrawPreInit, once per (camera, layer) batch
 *   push   -- each DrawObjIdx++ site, binding that slot to the open bracket's
 *             model and to a per-model PUSH-ORDER rank (`sub`)
 *   begin  -- Hu3DDrawPost, per draw object, keyed by its DrawObjData index
 * `sub` is push-order, so the depth sort permuting the emission order cannot
 * disturb it; a model whose deferred membership actually changes still fails
 * the group-integrity census in frame_interp.c and snaps for that one tick. */
void mp6_fi_capture_defer_reset(void);
void mp6_fi_capture_defer_push(int draw_obj_index);
void mp6_fi_capture_defer_begin(int draw_obj_index);

/* LAYER-HOOK IDENTITY.  A layer hook (Hu3DLayerHookSet) draws inside a camera
 * bracket but owns no model slot: HuSprLayerHook, the water / framebuffer-copy
 * passes, Hu3DZClear.  Several of those emit world-space matrices (hsfanim.c
 * loads Hu3DCameraMtx itself as the modelview), so they move with a pan exactly
 * like a model does and must pair for the same reason.  Their identity lives in
 * a pseudo-slot namespace above HU3D_MODEL_MAX and is generation-stamped from
 * the hook function pointer, so installing a different hook in the same slot
 * cannot inherit the previous one's motion.  Closed with
 * mp6_fi_capture_model_end() like any other bracket. */
void mp6_fi_capture_layer_hook_begin(int hook_slot, const void *hook_fn);

/* Rotate the real-tick camera snapshots after the real present.  A matrix pair
 * is eligible only when its camera stayed live and position, target, and up
 * all remained below their cut thresholds. */
void mp6_fi_model_snapshot(void);
int mp6_fi_model_camera_stable(int camera_id);

/* Drop camera history and invalidate every live slot generation after a
 * feature transition or savestate restore. */
void mp6_fi_model_reset(void);

/* Existing diagnostics retained for automated state-drift comparisons. */
void mp6_fi_model_animlog(long tick);

#ifdef __cplusplus
}
#endif

#endif /* MP6_FI_MODEL_H */
