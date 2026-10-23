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
_Static_assert(FI_DEFER_MAX < 0x4000, "deferred ranks overlap authored object identities");
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
static int s_objectHintPending;
static int s_objectHintIndex = -1;

/* Deferred-draw identity reserved at push time (see mp6_fi_model.h). */
static int16_t s_deferModel[FI_DEFER_MAX];
static uint16_t s_deferSub[FI_DEFER_MAX];
static uint16_t s_deferRank[HU3D_MODEL_MAX];

static FiCamera s_camPrev[HU3D_CAM_MAX];
static FiCamera s_camCur[HU3D_CAM_MAX];
static int s_haveCamPrev;
static int s_haveCamCur;
/* 0 unknown, 1 unstable, 2 stable; snapshots cannot change within a replay window. */
static unsigned char s_camStable[HU3D_CAM_MAX];

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
    s_objectHintPending = 0;
    s_contextCamera = -1;
    s_contextModel = -1;
    s_contextHook = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
}

void mp6_fi_capture_camera(int camera_id)
{
    s_objectHintPending = 0;
    s_contextCamera = (camera_id >= 0 && camera_id < HU3D_CAM_MAX) ? camera_id : -1;
    s_contextModel = -1;
    s_contextHook = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
}

void mp6_fi_capture_model_begin(int model_id)
{
    s_objectHintPending = 0;
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
    s_objectHintPending = 0;
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

int mp6_fi_object_index(int model_id, const void *object)
{
    const HSF_DATA *hsf;
    uintptr_t base, address, offset;
    if (!Hu3DData || model_id < 0 || model_id >= HU3D_MODEL_MAX || !object ||
        (Hu3DData[model_id].attr & HU3D_ATTR_HOOKFUNC)) return -1;
    hsf = Hu3DData[model_id].hsf;
    if (!hsf || !hsf->object || hsf->objectNum <= 0) return -1;
    base = (uintptr_t)hsf->object;
    address = (uintptr_t)object;
    if (address < base) return -1;
    offset = address - base;
    if (offset % sizeof(HSF_OBJECT)) return -1;
    offset /= sizeof(HSF_OBJECT);
    if (offset >= (uintptr_t)hsf->objectNum || offset >= 0x4000u) return -1;
    return (int)offset;
}

void mp6_fi_capture_object_matrix(int model_id, int object_index)
{
    const HSF_DATA *hsf = NULL;
    s_objectHintPending = 1;
    s_objectHintIndex = -1;
    if (!Hu3DData || s_contextCamera < 0 || s_contextModel != model_id ||
        model_id < 0 || model_id >= HU3D_MODEL_MAX ||
        (Hu3DData[model_id].attr & HU3D_ATTR_HOOKFUNC)) return;
    hsf = Hu3DData[model_id].hsf;
    if (!hsf || !hsf->object || object_index < 0 ||
        object_index >= hsf->objectNum || object_index >= 0x4000) return;
    s_objectHintIndex = object_index;
}

void mp6_fi_capture_auxiliary_begin(int model_id, int object_index)
{
    mp6_fi_capture_model_begin(model_id);
    /* A separate stable sub-key for the same mesh's AO coverage draw. */
    if (object_index<0 || object_index>=0x4000) s_contextModel=-1;
    else s_contextSub=(uint16_t)(0x8000u+(unsigned)object_index);
}

void mp6_fi_capture_model_end(void)
{
    s_objectHintPending = 0;
    s_contextModel = -1;
    s_contextHook = -1;
    s_contextSub = FI_SUB_IMMEDIATE;
    s_contextOrdinal = 0;
}

void mp6_fi_capture_defer_reset(void)
{
    int i;
    s_objectHintPending = 0;
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
    /* Never enter the authored-object namespace, even under malformed pushes.
     * A saturated rank creates duplicates, which the pairer rejects. */
    if (s_deferRank[model] < 0x3FFFu) s_deferRank[model]++;
}

void mp6_fi_capture_defer_begin(int draw_obj_index)
{
    s_objectHintPending = 0;
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
    int objectHint = s_objectHintPending;
    int objectIndex = s_objectHintIndex;
    s_objectHintPending = 0;
    if (objectHint && objectIndex < 0) return 0;
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
    if (sub) *sub = objectHint ? (uint16_t)(0x4000u + (unsigned)objectIndex) : s_contextSub;
    if (ordinal) *ordinal = objectHint ? 0 : s_contextOrdinal;
    if (!objectHint && s_contextOrdinal != UINT16_MAX) s_contextOrdinal++;
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
    memset(s_camStable, 0, sizeof(s_camStable));
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

static int fi_camera_stable_uncached(int camera_id)
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

int mp6_fi_model_camera_stable(int camera_id)
{
    if (camera_id < 0 || camera_id >= HU3D_CAM_MAX) return 0;
    if (!s_camStable[camera_id])
        s_camStable[camera_id] = 1 + fi_camera_stable_uncached(camera_id);
    return s_camStable[camera_id] == 2;
}

/* ---- CPU-deformed vertex buffers (include/mp6_fi_model.h) ---- */
#define MP6_FI_DEFORMED_MAX 1024
static Mp6FiDeformed s_deformed[MP6_FI_DEFORMED_MAX];
static int s_deformedCount;

/* Open-addressed index of s_deformed by data pointer, so registering a
 * tick's buffers stays linear in their number. A slot belongs to the
 * current tick only while its stamp equals s_deformedStamp. */
#define MP6_FI_DEFORMED_SLOTS (MP6_FI_DEFORMED_MAX * 2)
static struct {
    uint32_t stamp;
    int index;
} s_deformedSlot[MP6_FI_DEFORMED_SLOTS];
static uint32_t s_deformedStamp = 1;

static void fi_deformed_add(void *data, uint32_t bytes)
{
    uint32_t h;
    if (data == NULL || bytes == 0) return;
    h = (uint32_t)(((uintptr_t)data >> 4) * 2654435761u) & (MP6_FI_DEFORMED_SLOTS - 1);
    while (s_deformedSlot[h].stamp == s_deformedStamp) {
        Mp6FiDeformed *e = &s_deformed[s_deformedSlot[h].index];
        if (e->data == data) {
            if (bytes > e->bytes) e->bytes = bytes;
            return;
        }
        h = (h + 1) & (MP6_FI_DEFORMED_SLOTS - 1);
    }
    if (s_deformedCount < MP6_FI_DEFORMED_MAX) {
        s_deformedSlot[h].stamp = s_deformedStamp;
        s_deformedSlot[h].index = s_deformedCount;
        s_deformed[s_deformedCount].data = data;
        s_deformed[s_deformedCount].bytes = bytes;
        s_deformedCount++;
    }
}

void mp6_fi_note_deformed(void *hsfp)
{
    HSF_DATA *hsf = (HSF_DATA *)hsfp;
    int i;
    if (hsf == NULL) return;
    /* Deformers write each mesh object's own buffers (EnvelopeExec.c uses
     * object->mesh.vertex/normal); register those first. */
    for (i = 0; i < hsf->objectNum && hsf->object != NULL; ++i) {
        const HSF_OBJECT *obj = &hsf->object[i];
        if (obj->type != HSF_OBJ_MESH) continue;
        if (obj->mesh.vertex != NULL && obj->mesh.vertex->count > 0)
            fi_deformed_add(obj->mesh.vertex->data, (uint32_t)obj->mesh.vertex->count * (uint32_t)sizeof(HuVecF));
        if (obj->mesh.cenvNum && obj->mesh.normal != NULL && obj->mesh.normal->count > 0)
            fi_deformed_add(obj->mesh.normal->data, (uint32_t)obj->mesh.normal->count * (uint32_t)sizeof(HuVecF));
    }
    for (i = 0; i < hsf->vertexNum && hsf->vertex != NULL; ++i)
        if (hsf->vertex[i].count > 0)
            fi_deformed_add(hsf->vertex[i].data, (uint32_t)hsf->vertex[i].count * (uint32_t)sizeof(HuVecF));
    /* Envelope skinning also rewrites normals; hsfdraw.c switches them to
     * float for exactly these models. Other normals stay packed and static. */
    if (hsf->cenvNum)
        for (i = 0; i < hsf->normalNum && hsf->normal != NULL; ++i)
            if (hsf->normal[i].count > 0)
                fi_deformed_add(hsf->normal[i].data, (uint32_t)hsf->normal[i].count * (uint32_t)sizeof(HuVecF));
}

int mp6_fi_deformed_list(const Mp6FiDeformed **out)
{
    *out = s_deformed;
    return s_deformedCount;
}

void mp6_fi_deformed_reset(void)
{
    s_deformedCount = 0;
    if (++s_deformedStamp == 0) { /* wrapped: no old slot may look current */
        memset(s_deformedSlot, 0, sizeof(s_deformedSlot));
        s_deformedStamp = 1;
    }
}

void mp6_fi_model_reset(void)
{
    int i;
    s_haveCamPrev = 0;
    s_haveCamCur = 0;
    memset(s_camStable, 0, sizeof(s_camStable));
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
