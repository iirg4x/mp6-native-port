/* MP6 native port -- model/camera identity metadata for retained GX replay.
 *
 * This file deliberately contains no replay renderer.  A replay submits bytes
 * captured from the real tick, so Hu3DExec and its arbitrary layer/model/
 * material hooks run exactly once per simulation tick.  The retained stream
 * still needs stable identities to pair matrices across draw-order changes;
 * Hu3DExec supplies that context through the small API below.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/hu3d.h"
#include "mp6_fi_model.h"

/* Generations, contexts, and camera snapshots describe the running renderer,
 * not captured game state.  In particular, restoring them would let a model
 * at a reused slot inherit motion from a different timeline. */
#include "mp6_host_section.h"

#define FI_CAM_TRANSLATION_CUT 50.0f
#define FI_CAM_TARGET_CUT      50.0f
#define FI_CAM_UP_COS_CUT      0.984807753f /* cos(10 degrees) */
#define FI_CAM_FOV_CUT          5.0f

typedef struct {
    int valid;
    Vec pos;
    Vec target;
    Vec up;
    float fov;
} FiCamera;

/* Must match hsfdraw.c's own DRAW_OBJ_MAX (the DrawObjData capacity). Sized
 * from the same pinned decomp constant; every entry point bounds-checks, so a
 * future divergence degrades to "no deferred identity" rather than a write
 * past the table. */
#define FI_DEFER_MAX 512
/* `sub` value for a matrix emitted inside the model bracket itself (the
 * immediate pass). Deferred draws use their per-model push rank, which the
 * push counter is clamped below, so the two namespaces cannot collide. */
#define FI_SUB_IMMEDIATE 0xFFFFu
/* Pseudo-slot namespace for layer hooks, disjoint from every real model slot.
 * HU3D_MODEL_MAX + HU3D_LAYER_HOOK_MAX still fits the key's int16 model
 * field. */
#define FI_HOOK_SLOT_BASE HU3D_MODEL_MAX

static uint32_t s_generation[HU3D_MODEL_MAX];
static int s_contextCamera = -1;
static int s_contextModel = -1;
static int s_contextHook = -1;
static uint32_t s_contextHookGen;
static uint16_t s_contextSub = FI_SUB_IMMEDIATE;
static uint16_t s_contextOrdinal;

/* Deferred-draw identity reserved at push time (see mp6_fi_model.h). */
static int16_t s_deferModel[FI_DEFER_MAX];
static uint16_t s_deferSub[FI_DEFER_MAX];
static uint16_t s_deferRank[HU3D_MODEL_MAX];

static FiCamera s_camPrev[HU3D_CAM_MAX];
static FiCamera s_camCur[HU3D_CAM_MAX];
static int s_haveCamPrev;
static int s_haveCamCur;

static uint32_t next_generation(uint32_t value)
{
    value++;
    return value ? value : 1u;
}

void mp6_fi_model_identity_new(int model_id)
{
    if (model_id < 0 || model_id >= HU3D_MODEL_MAX) return;
    s_generation[model_id] = next_generation(s_generation[model_id]);
}

void mp6_fi_capture_context_reset(void)
{
    s_contextCamera = -1;
    s_contextModel = -1;
    s_contextHook = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
}

void mp6_fi_capture_camera(int camera_id)
{
    s_contextCamera = (camera_id >= 0 && camera_id < HU3D_CAM_MAX) ? camera_id : -1;
    s_contextModel = -1;
    s_contextHook = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
}

void mp6_fi_capture_model_begin(int model_id)
{
    if (model_id < 0 || model_id >= HU3D_MODEL_MAX) {
        s_contextModel = -1;
    } else {
        /* Defensive bootstrap for models created before this module's hooks
         * were reached (not expected in normal startup, harmless if it is). */
        if (s_generation[model_id] == 0) s_generation[model_id] = 1;
        s_contextModel = model_id;
    }
    s_contextHook = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
}

void mp6_fi_capture_layer_hook_begin(int hook_slot, const void *hook_fn)
{
    s_contextModel = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
    if (hook_slot < 0 || hook_slot >= HU3D_LAYER_HOOK_MAX || hook_fn == NULL) {
        s_contextHook = -1;
        return;
    }
    s_contextHook = hook_slot;
    /* Generation from the installed function itself: a slot that changes hook
     * changes generation, so the new hook cannot pair against the old one's
     * matrices. Forced nonzero so it never reads as "no generation". */
    s_contextHookGen = (uint32_t)((uintptr_t)hook_fn >> 4) | 1u;
}

void mp6_fi_capture_model_end(void)
{
    s_contextModel = -1;
    s_contextHook = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
}

void mp6_fi_capture_defer_reset(void)
{
    int i;
    for (i = 0; i < FI_DEFER_MAX; ++i) s_deferModel[i] = -1;
    memset(s_deferRank, 0, sizeof(s_deferRank));
}

void mp6_fi_capture_defer_push(int draw_obj_index)
{
    int model;
    if (draw_obj_index < 0 || draw_obj_index >= FI_DEFER_MAX) return;
    model = s_contextModel;
    if (model < 0 || model >= HU3D_MODEL_MAX) {
        /* Pushed outside any model bracket (the shadow/reflect passes reach
         * Hu3DDraw without one). No identity is better than a wrong one. */
        s_deferModel[draw_obj_index] = -1;
        s_deferSub[draw_obj_index] = FI_SUB_IMMEDIATE;
        return;
    }
    s_deferModel[draw_obj_index] = (int16_t)model;
    s_deferSub[draw_obj_index] = s_deferRank[model];
    /* Clamp one below FI_SUB_IMMEDIATE so a pathological model can never
     * manufacture an immediate-pass `sub` and alias a bracketed matrix. */
    if (s_deferRank[model] < (uint16_t)(FI_SUB_IMMEDIATE - 1u)) s_deferRank[model]++;
}

void mp6_fi_capture_defer_begin(int draw_obj_index)
{
    s_contextOrdinal = 0;
    s_contextHook = -1;
    if (draw_obj_index < 0 || draw_obj_index >= FI_DEFER_MAX ||
        s_deferModel[draw_obj_index] < 0) {
        s_contextModel = -1;
        s_contextSub = FI_SUB_IMMEDIATE;
        return;
    }
    s_contextModel = s_deferModel[draw_obj_index];
    s_contextSub = s_deferSub[draw_obj_index];
}

int mp6_fi_capture_camera_id(void)
{
    return s_contextCamera;
}

int mp6_fi_capture_context_next(int *camera_id, int *model_id,
                                uint32_t *generation, uint16_t *sub,
                                uint16_t *ordinal)
{
    int slot;
    uint32_t gen;
    if (s_contextCamera < 0) return 0;
    if (s_contextModel >= 0) {
        slot = s_contextModel;
        gen = s_generation[s_contextModel];
    } else if (s_contextHook >= 0) {
        slot = FI_HOOK_SLOT_BASE + s_contextHook;
        gen = s_contextHookGen;
    } else {
        return 0;
    }
    if (camera_id) *camera_id = s_contextCamera;
    if (model_id) *model_id = slot;
    if (generation) *generation = gen;
    if (sub) *sub = s_contextSub;
    if (ordinal) *ordinal = s_contextOrdinal;
    if (s_contextOrdinal != UINT16_MAX) s_contextOrdinal++;
    return 1;
}

void mp6_fi_model_snapshot(void)
{
    int i;
    memcpy(s_camPrev, s_camCur, sizeof(s_camPrev));
    s_haveCamPrev = s_haveCamCur;

    for (i = 0; i < HU3D_CAM_MAX; ++i) {
        const HU3D_CAMERA *camera = &Hu3DCamera[i];
        FiCamera *out = &s_camCur[i];
        if (camera->fov == -1.0f) {
            out->valid = 0;
            continue;
        }
        out->valid = 1;
        out->pos = camera->pos;
        out->target = camera->target;
        out->up = camera->up;
        out->fov = camera->fov;
    }
    s_haveCamCur = 1;
}

static float vec_delta_sq(const Vec *a, const Vec *b)
{
    float x = b->x - a->x;
    float y = b->y - a->y;
    float z = b->z - a->z;
    return x * x + y * y + z * z;
}

static int up_vector_stable(const Vec *a, const Vec *b)
{
    float aa = a->x * a->x + a->y * a->y + a->z * a->z;
    float bb = b->x * b->x + b->y * b->y + b->z * b->z;
    float dot;
    if (!(aa > 1.0e-12f) || !(bb > 1.0e-12f)) return 0;
    dot = (a->x * b->x + a->y * b->y + a->z * b->z) / sqrtf(aa * bb);
    return isfinite(dot) && dot > FI_CAM_UP_COS_CUT;
}

int mp6_fi_model_camera_stable(int camera_id)
{
    const FiCamera *a;
    const FiCamera *b;
    float fovDelta;
    if (!s_haveCamPrev || !s_haveCamCur || camera_id < 0 || camera_id >= HU3D_CAM_MAX) return 0;
    a = &s_camPrev[camera_id];
    b = &s_camCur[camera_id];
    if (!a->valid || !b->valid) return 0;
    /* Write these as positive `<` requirements: NaN must mean "unstable",
     * not slip through a `>=` rejection whose comparison is false. */
    if (!(vec_delta_sq(&a->pos, &b->pos) <
          FI_CAM_TRANSLATION_CUT * FI_CAM_TRANSLATION_CUT)) return 0;
    if (!(vec_delta_sq(&a->target, &b->target) <
          FI_CAM_TARGET_CUT * FI_CAM_TARGET_CUT)) return 0;
    if (!up_vector_stable(&a->up, &b->up)) return 0;
    fovDelta = b->fov - a->fov;
    if (fovDelta < 0.0f) fovDelta = -fovDelta;
    return fovDelta < FI_CAM_FOV_CUT;
}

void mp6_fi_model_reset(void)
{
    int i;
    s_haveCamPrev = 0;
    s_haveCamCur = 0;
    mp6_fi_capture_context_reset();
    mp6_fi_capture_defer_reset();

    /* The live Hu3D array may have changed wholesale on a restore without any
     * create call.  Renew every occupied slot so no pre-restore pairing key can
     * match the new timeline even if an allocator reused the same addresses. */
    if (Hu3DData) {
        for (i = 0; i < HU3D_MODEL_MAX; ++i) {
            if (Hu3DData[i].hsf) {
                s_generation[i] = next_generation(s_generation[i]);
            }
        }
    }
}

static int animlog_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("MP6_FI_ANIMLOG");
        enabled = (env && *env && *env != '0') ? 1 : 0;
    }
    return enabled;
}

static void fnv_f32(unsigned int *hash, float value)
{
    const unsigned char *p = (const unsigned char *)&value;
    int i;
    for (i = 0; i < 4; ++i) {
        *hash ^= p[i];
        *hash *= 16777619u;
    }
}

void mp6_fi_model_animlog(long tick)
{
    unsigned int hash = 2166136261u;
    int live = 0;
    int i;
    if (!animlog_enabled() || !Hu3DData) return;
    for (i = 0; i < HU3D_MODEL_MAX; ++i) {
        HU3D_MODEL *model = &Hu3DData[i];
        if (!model->hsf) continue;
        live++;
        hash ^= model->tick; hash *= 16777619u;
        fnv_f32(&hash, model->motWork.time);
        fnv_f32(&hash, model->motOvlWork.time);
        fnv_f32(&hash, model->motShiftWork.time);
        fnv_f32(&hash, model->motShapeWork.time);
        fnv_f32(&hash, model->pos.x); fnv_f32(&hash, model->pos.y); fnv_f32(&hash, model->pos.z);
        fnv_f32(&hash, model->rot.x); fnv_f32(&hash, model->rot.y); fnv_f32(&hash, model->rot.z);
        fnv_f32(&hash, model->scale.x); fnv_f32(&hash, model->scale.y); fnv_f32(&hash, model->scale.z);
    }
    fprintf(stderr, "[FI-ANIMLOG] tick=%ld live=%d digest=%08x\n", tick, live, hash);
    fflush(stderr);
}
