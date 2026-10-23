/* MP6 native port -- opt-in X travel for scene-owned animated HSF objects.
 *
 * A scene root can own a normal HSF POSX motion track while a neighboring
 * backdrop widens around a pivot.  Rewriting that root's live keys once is
 * not enough for a resizable window, and rebuilding a curve array every frame
 * would both allocate and lose the native motion data's ownership.  This
 * helper keeps one small host snapshot per registered object and derives the
 * live base position/curve values from that immutable snapshot only when the
 * widescreen scale changes.
 *
 * The registration is deliberately opt-in and exact: it uses the named object
 * in the model's own HSF object array and the embedded HSF_DATA->motion track
 * array.  HSF_DATA->motionNum is the embedded motion's track count in this
 * native loader; it is not an array length of HSF_MOTION records.
 */
#include "game/hu3d.h"
#include "mp6_widescreen.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MP6_WIDESCREEN_MOTION_MAX 64
#define MP6_WIDESCREEN_MOTION_NON_HSF_ATTR \
    (HU3D_ATTR_HOOKFUNC | HU3D_ATTR_PARTICLE | HU3D_ATTR_CAMERA)

typedef struct {
    HSF_TRACK *track;
    u16 target;
    u16 channel;
    u16 curveType;
    u16 keyCount;
    size_t bytes;
    void *liveData;
    void *nativeKeys;
    float nativeConst;
} MP6WidescreenMotionTrack;

typedef struct {
    int active;
    int16_t modelId;
    HSF_DATA *hsf;
    HSF_MOTION *motion;
    HSF_OBJECT *object;
    HSF_TRACK *trackBase;
    float pivotX;
    float nativeBaseX;
    s16 trackCount;
    s16 motionTrackCount;
    MP6WidescreenMotionTrack *tracks;
} MP6WidescreenMotionEntry;

static MP6WidescreenMotionEntry
    g_mp6WidescreenMotion[MP6_WIDESCREEN_MOTION_MAX];
static s16 g_mp6WidescreenMotionNum;
static int g_mp6WidescreenMotionHaveScale;
static float g_mp6WidescreenMotionLastScale;

static int mp6_widescreen_motion_id_valid(int16_t modelId)
{
    return Hu3DData != NULL && modelId >= 0 && modelId < HU3D_MODEL_MAX;
}

static int mp6_widescreen_motion_object_live(const HSF_DATA *hsf,
                                             const HSF_OBJECT *object)
{
    s32 i;

    if (hsf == NULL || object == NULL || hsf->object == NULL ||
        hsf->objectNum <= 0) {
        return 0;
    }
    for (i = 0; i < hsf->objectNum; ++i) {
        if (&hsf->object[i] == object) {
            return object->name != NULL;
        }
    }
    return 0;
}

static int mp6_widescreen_motion_entry_live(
    const MP6WidescreenMotionEntry *entry)
{
    HU3D_MODEL *model;

    if (entry == NULL || !entry->active ||
        !mp6_widescreen_motion_id_valid(entry->modelId)) {
        return 0;
    }
    model = &Hu3DData[entry->modelId];
    if ((model->attr & MP6_WIDESCREEN_MOTION_NON_HSF_ATTR) != 0 ||
        model->hsf != entry->hsf || entry->hsf == NULL ||
        entry->hsf->motion != entry->motion ||
        !mp6_widescreen_motion_object_live(entry->hsf, entry->object)) {
        return 0;
    }
    if (entry->motion == NULL || entry->motion->track != entry->trackBase ||
        entry->hsf->motionNum != entry->motionTrackCount) {
        return 0;
    }
    for (s16 i = 0; i < entry->trackCount; ++i) {
        const MP6WidescreenMotionTrack *snapshot = &entry->tracks[i];
        const HSF_TRACK *track = snapshot->track;
        if (track == NULL || track->type != HSF_TRACK_TRANSFORM ||
            track->target != snapshot->target ||
            track->channel != snapshot->channel ||
            track->curveType != snapshot->curveType ||
            track->numKeyframes != snapshot->keyCount ||
            (snapshot->curveType != HSF_CURVE_CONST &&
             track->data != snapshot->liveData)) {
            return 0;
        }
    }
    return entry->trackCount == 0 || entry->motion->track != NULL;
}

static void mp6_widescreen_motion_snapshot_free(
    MP6WidescreenMotionEntry *entry)
{
    s16 i;

    if (entry == NULL) {
        return;
    }
    if (entry->tracks != NULL) {
        for (i = 0; i < entry->trackCount; ++i) {
            free(entry->tracks[i].nativeKeys);
            entry->tracks[i].nativeKeys = NULL;
        }
        free(entry->tracks);
    }
    entry->tracks = NULL;
    entry->trackCount = 0;
}

static void mp6_widescreen_motion_entry_clear(
    MP6WidescreenMotionEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    mp6_widescreen_motion_snapshot_free(entry);
    memset(entry, 0, sizeof(*entry));
}

static MP6WidescreenMotionEntry *mp6_widescreen_motion_find(
    int16_t modelId, HSF_DATA *hsf, HSF_OBJECT *object)
{
    s16 i;

    for (i = 0; i < g_mp6WidescreenMotionNum; ++i) {
        MP6WidescreenMotionEntry *entry = &g_mp6WidescreenMotion[i];
        if (entry->active && entry->modelId == modelId &&
            entry->hsf == hsf && entry->object == object) {
            return entry;
        }
    }
    return NULL;
}

static MP6WidescreenMotionEntry *mp6_widescreen_motion_slot(void)
{
    s16 i;

    for (i = 0; i < g_mp6WidescreenMotionNum; ++i) {
        if (!g_mp6WidescreenMotion[i].active) {
            return &g_mp6WidescreenMotion[i];
        }
    }
    if (g_mp6WidescreenMotionNum >= MP6_WIDESCREEN_MOTION_MAX) {
        return NULL;
    }
    return &g_mp6WidescreenMotion[g_mp6WidescreenMotionNum++];
}

static int mp6_widescreen_motion_any_active(void)
{
    s16 i;

    for (i = 0; i < g_mp6WidescreenMotionNum; ++i) {
        if (g_mp6WidescreenMotion[i].active) {
            return 1;
        }
    }
    return 0;
}

/* A model with hsfLink/linkMdlId owns shared motion storage.  The helper
 * refuses it rather than taking a snapshot whose writes would affect another
 * linked model.  Also refuse an already-published HSF pointer in another live
 * model slot: shared HSF motion is not independently writable. */
static int mp6_widescreen_motion_shared_owner(int16_t modelId,
                                              HSF_DATA *hsf)
{
    s16 i;

    if (Hu3DData == NULL || hsf == NULL) {
        return 1;
    }
    if ((Hu3DData[modelId].attr & MP6_WIDESCREEN_MOTION_NON_HSF_ATTR) != 0 ||
        Hu3DData[modelId].linkMdlId >= 0) {
        return 1;
    }
    for (i = 0; i < HU3D_MODEL_MAX; ++i) {
        HU3D_MODEL *other;

        if (i == modelId) {
            continue;
        }
        other = &Hu3DData[i];
        if ((other->attr & MP6_WIDESCREEN_MOTION_NON_HSF_ATTR) != 0) {
            continue;
        }
        if (other->hsf == hsf ||
            (other->linkMdlId != HU3D_MODELID_NONE &&
             other->hsfLink == hsf) ||
            (other->hsf != NULL && other->hsf->motion == hsf->motion)) {
            return 1;
        }
    }
    for (i = 0; i < g_mp6WidescreenMotionNum; ++i) {
        MP6WidescreenMotionEntry *entry = &g_mp6WidescreenMotion[i];
        if (entry->active && entry->hsf == hsf && entry->modelId != modelId) {
            return 1;
        }
    }
    return 0;
}

static int mp6_widescreen_motion_key_shape(const HSF_TRACK *track,
                                           size_t *bytes)
{
    size_t keyCount;
    size_t stride;

    if (track == NULL || bytes == NULL) {
        return 0;
    }
    if (track->curveType == HSF_CURVE_CONST) {
        /* CONST stores its single value in the track union rather than a
         * keyframe array.  numKeyframes/data are intentionally ignored. */
        *bytes = 0;
        return 1;
    }
    keyCount = (size_t)track->numKeyframes;
    switch (track->curveType) {
    case HSF_CURVE_STEP:
    case HSF_CURVE_LINEAR:
        stride = sizeof(float) * 2u;
        break;
    case HSF_CURVE_BEZIER:
        stride = sizeof(float) * 4u;
        break;
    default:
        return 0;
    }
    if (keyCount != 0 && track->data == NULL) {
        return 0;
    }
    if (keyCount > SIZE_MAX / stride) {
        return 0;
    }
    *bytes = keyCount * stride;
    return 1;
}

static void mp6_widescreen_motion_discard_tracks(
    MP6WidescreenMotionTrack *tracks, s16 count)
{
    s16 i;

    if (tracks == NULL) {
        return;
    }
    for (i = 0; i < count; ++i) {
        free(tracks[i].nativeKeys);
    }
    free(tracks);
}

static float mp6_widescreen_motion_value(float native, float pivot, float k)
{
    return pivot + k * (native - pivot);
}

static void mp6_widescreen_motion_apply(
    MP6WidescreenMotionEntry *entry, float k)
{
    s16 i;

    if (k == 1.0f) {
        /* Byte-copy at native scale: preserve the original float bits and
         * restore every key exactly, including signed zero. */
        memcpy(&entry->object->mesh.base.pos.x, &entry->nativeBaseX,
               sizeof(entry->nativeBaseX));
        for (i = 0; i < entry->trackCount; ++i) {
            MP6WidescreenMotionTrack *snapshot = &entry->tracks[i];
            if (snapshot->curveType == HSF_CURVE_CONST) {
                memcpy(&snapshot->track->value, &snapshot->nativeConst,
                       sizeof(snapshot->nativeConst));
            } else if (snapshot->bytes != 0) {
                memcpy(snapshot->track->data, snapshot->nativeKeys,
                       snapshot->bytes);
            }
        }
        return;
    }

    entry->object->mesh.base.pos.x =
        mp6_widescreen_motion_value(entry->nativeBaseX, entry->pivotX, k);
    for (i = 0; i < entry->trackCount; ++i) {
        MP6WidescreenMotionTrack *snapshot = &entry->tracks[i];
        size_t key;

        if (snapshot->curveType == HSF_CURVE_CONST) {
            snapshot->track->value = mp6_widescreen_motion_value(
                snapshot->nativeConst, entry->pivotX, k);
            continue;
        }
        if (snapshot->curveType == HSF_CURVE_STEP ||
            snapshot->curveType == HSF_CURVE_LINEAR) {
            float (*dst)[2] = (float (*)[2])snapshot->track->data;
            const float (*native)[2] = (const float (*)[2])snapshot->nativeKeys;
            for (key = 0; key < snapshot->keyCount; ++key) {
                dst[key][0] = native[key][0];
                dst[key][1] = mp6_widescreen_motion_value(
                    native[key][1], entry->pivotX, k);
            }
        } else {
            float (*dst)[4] = (float (*)[4])snapshot->track->data;
            const float (*native)[4] = (const float (*)[4])snapshot->nativeKeys;
            for (key = 0; key < snapshot->keyCount; ++key) {
                dst[key][0] = native[key][0];
                dst[key][1] = mp6_widescreen_motion_value(
                    native[key][1], entry->pivotX, k);
                /* Bezier tangent components are derivatives/offsets, so the
                 * pivot translation cancels and only the scale is applied. */
                dst[key][2] = k * native[key][2];
                dst[key][3] = k * native[key][3];
            }
        }
    }
}

void mp6_widescreen_motion_x(int16_t modelId, const char *objName,
                             float pivotX)
{
    HU3D_MODEL *model;
    HSF_DATA *hsf;
    HSF_MOTION *motion;
    HSF_OBJECT *object = NULL;
    MP6WidescreenMotionTrack *tracks = NULL;
    MP6WidescreenMotionEntry *existing;
    MP6WidescreenMotionEntry *slot;
    s32 objectIndex = -1;
    s16 trackCount;
    s16 matchingCount = 0;
    s16 snapshotCount = 0;
    s16 i;
    int hadActive;

    if (!mp6_widescreen_motion_id_valid(modelId) || objName == NULL ||
        !isfinite(pivotX)) {
        return;
    }
    model = &Hu3DData[modelId];
    if ((model->attr & MP6_WIDESCREEN_MOTION_NON_HSF_ATTR) != 0) {
        return;
    }
    hsf = model->hsf;
    if (hsf == NULL || hsf->object == NULL || hsf->objectNum <= 0 ||
        hsf->motion == NULL || mp6_widescreen_motion_shared_owner(modelId, hsf)) {
        return;
    }

    for (i = 0; i < hsf->objectNum; ++i) {
        if (hsf->object[i].name != NULL &&
            strcmp(hsf->object[i].name, objName) == 0) {
            object = &hsf->object[i];
            objectIndex = i;
            break;
        }
    }
    if (object == NULL || objectIndex < 0 || object->type == HSF_OBJ_CAMERA ||
        object->type == HSF_OBJ_LIGHT) {
        return;
    }
    motion = hsf->motion;
    trackCount = hsf->motionNum;
    if (trackCount < 0 || motion->numTracks < 0 ||
        motion->numTracks != trackCount ||
        (trackCount > 0 && motion->track == NULL)) {
        return;
    }

    /* A duplicate registration is an idempotent no-op.  In particular it
     * never snapshots already-scaled live keys as a new native baseline. */
    existing = mp6_widescreen_motion_find(modelId, hsf, object);
    if (existing != NULL) {
        if (mp6_widescreen_motion_entry_live(existing)) {
            return;
        }
        mp6_widescreen_motion_entry_clear(existing);
    }
    hadActive = mp6_widescreen_motion_any_active();

    /* First validate every matching POSX track and count only the snapshots
     * this object owns.  HSF_DATA->motionNum is the embedded track count;
     * motion->numTracks must agree before either pass walks the array. */
    for (i = 0; i < trackCount; ++i) {
        HSF_TRACK *track = &motion->track[i];
        size_t bytes = 0;

        if (track->type != HSF_TRACK_TRANSFORM ||
            track->target != (u16)objectIndex ||
            track->channel != HSF_CHANNEL_POSX) {
            continue;
        }
        if (!mp6_widescreen_motion_key_shape(track, &bytes)) {
            return;
        }
        ++matchingCount;
    }
    if (matchingCount > 0) {
        if ((size_t)matchingCount > SIZE_MAX / sizeof(*tracks)) {
            return;
        }
        tracks = (MP6WidescreenMotionTrack *)calloc(
            (size_t)matchingCount, sizeof(*tracks));
        if (tracks == NULL) {
            return;
        }
    }

    /* Snapshot the prevalidated matching tracks before publishing anything
     * into the bounded registry. Any allocation failure remains transactional
     * because only this temporary array has been touched. */
    for (i = 0; i < trackCount; ++i) {
        HSF_TRACK *track = &motion->track[i];
        size_t bytes = 0;

        if (track->type != HSF_TRACK_TRANSFORM ||
            track->target != (u16)objectIndex ||
            track->channel != HSF_CHANNEL_POSX) {
            continue;
        }
        if (!mp6_widescreen_motion_key_shape(track, &bytes)) {
            mp6_widescreen_motion_discard_tracks(tracks, snapshotCount);
            return;
        }
        tracks[snapshotCount].track = track;
        tracks[snapshotCount].target = track->target;
        tracks[snapshotCount].channel = track->channel;
        tracks[snapshotCount].curveType = track->curveType;
        tracks[snapshotCount].keyCount = track->numKeyframes;
        tracks[snapshotCount].bytes = bytes;
        tracks[snapshotCount].liveData = track->data;
        if (track->curveType == HSF_CURVE_CONST) {
            memcpy(&tracks[snapshotCount].nativeConst, &track->value,
                   sizeof(track->value));
        } else if (bytes != 0) {
            tracks[snapshotCount].nativeKeys = malloc(bytes);
            if (tracks[snapshotCount].nativeKeys == NULL) {
                mp6_widescreen_motion_discard_tracks(tracks, snapshotCount);
                return;
            }
            memcpy(tracks[snapshotCount].nativeKeys, track->data, bytes);
        }
        ++snapshotCount;
    }

    slot = mp6_widescreen_motion_slot();
    if (slot == NULL) {
        mp6_widescreen_motion_discard_tracks(tracks, snapshotCount);
        return;
    }
    if (slot->active) {
        /* The slot allocator only returns inactive entries, but keep this
         * cleanup adjacent to publication so future allocator changes cannot
         * leak a stale host snapshot. */
        mp6_widescreen_motion_entry_clear(slot);
    }
    slot->active = 1;
    slot->modelId = modelId;
    slot->hsf = hsf;
    slot->motion = motion;
    slot->object = object;
    slot->trackBase = motion->track;
    slot->pivotX = pivotX;
    slot->nativeBaseX = object->mesh.base.pos.x;
    slot->trackCount = snapshotCount;
    slot->motionTrackCount = trackCount;
    slot->tracks = tracks;

    /* Registration is live even at k==1: apply now so a later aspect change
     * has an owned native baseline without requiring a scene reload. */
    {
        float k = mp6_widescreen_scale_factor();
        if (isfinite(k)) {
            mp6_widescreen_motion_apply(slot, k);
            /* If this is the first live entry, the immediate registration
             * application establishes the global gate.  With other entries
             * alive, leave their last-scale record untouched so a pending
             * aspect change still causes the next reapply to visit all of
             * them. */
            if (!hadActive) {
                g_mp6WidescreenMotionHaveScale = 1;
                g_mp6WidescreenMotionLastScale = k;
            }
        }
    }
}

void mp6_widescreen_motion_reapply(void)
{
    float k;
    s16 i;

    k = mp6_widescreen_scale_factor();
    if (!isfinite(k)) {
        return;
    }
    if (g_mp6WidescreenMotionHaveScale &&
        g_mp6WidescreenMotionLastScale == k) {
        return;
    }
    g_mp6WidescreenMotionHaveScale = 1;
    g_mp6WidescreenMotionLastScale = k;

    for (i = 0; i < g_mp6WidescreenMotionNum; ++i) {
        MP6WidescreenMotionEntry *entry = &g_mp6WidescreenMotion[i];
        if (!entry->active) {
            continue;
        }
        if (!mp6_widescreen_motion_entry_live(entry)) {
            mp6_widescreen_motion_entry_clear(entry);
            continue;
        }
        mp6_widescreen_motion_apply(entry, k);
    }
}

void mp6_widescreen_motion_forget(int16_t modelId)
{
    s16 i;

    for (i = 0; i < g_mp6WidescreenMotionNum; ++i) {
        MP6WidescreenMotionEntry *entry = &g_mp6WidescreenMotion[i];
        if (entry->active && entry->modelId == modelId) {
            mp6_widescreen_motion_entry_clear(entry);
        }
    }
}
