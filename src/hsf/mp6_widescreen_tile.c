/* Scene-owned horizontal repeats for authored, two-tile parallax strips.
 * Links share geometry/textures, but own their pose/material state. Only the
 * newly exposed margins draw companions; native artwork and motion stay intact.
 */
#include "game/hu3d.h"
#include "mp6_widescreen.h"
#include <math.h>
#include <string.h>

#define TILE_LAYERS 8
#define TILE_NEIGHBORS 16

typedef struct {
    HSF_DATA *hsf;
    int16_t owner;
    int16_t links[TILE_NEIGHBORS * 2];
    int count;
    float period;
    float nativeHalfView;
    float centerX;
    int mirror;
} TileLayer;

static TileLayer layers[TILE_LAYERS];

/* Call after the scene advances the source's top-level position. Both source
 * and companions then enter Hu3DMotionCalc with exactly the same phase.
 * The opt-in strip is rigid, uses primary motion only, and does not mutate
 * its HSF/materials after linking or use a custom model matrix. */
static void tile_apply(int16_t modelId, float period, float nativeHalfView,
                       int mirror, float centerX)
{
    TileLayer *entry = NULL;
    TileLayer *empty = NULL;
    if (modelId < 0 || modelId >= HU3D_MODEL_MAX || !Hu3DData ||
        !isfinite(period) || period <= 0 ||
        !isfinite(nativeHalfView) || nativeHalfView <= 0 ||
        !isfinite(centerX)) return;
    HU3D_MODEL *source = &Hu3DData[modelId];
    if (!source->hsf || source->linkMdlId != HU3D_MODELID_NONE ||
        (source->attr & (HU3D_ATTR_HOOKFUNC | HU3D_ATTR_PARTICLE |
                         HU3D_ATTR_CAMERA | HU3D_ATTR_SHADOW | HU3D_ATTR_HOOK))) return;
    /* Translation-only repeats: rotating/scaling the period would require a
     * different tiling axis. Never silently approximate it. */
    if (source->rot.x != 0 || source->rot.y != 0 || source->rot.z != 0 ||
        source->scale.x != 1 || source->scale.y != 1 || source->scale.z != 1)
        return;
    float scale = mp6_widescreen_scale_factor();
    if (!isfinite(scale) || scale <= 0) return;
    for (int i = 0; i < TILE_LAYERS; ++i) {
        if (layers[i].hsf == source->hsf && layers[i].owner == modelId)
            entry = &layers[i];
        if (!layers[i].hsf && !empty) empty = &layers[i];
    }
    if (!entry) {
        if (!empty) return;
        entry = empty;
        memset(entry, 0, sizeof(*entry));
        entry->hsf = source->hsf;
        entry->owner = modelId;
        entry->period = period;
        entry->nativeHalfView = nativeHalfView;
        entry->centerX = centerX;
        entry->mirror = mirror;
    }
    if (entry->period != period || entry->nativeHalfView != nativeHalfView ||
        entry->centerX != centerX || entry->mirror != mirror) return;
    int neighbors = scale <= 1 ? 0 :
        (int)ceilf(fminf(TILE_NEIGHBORS,
                         nativeHalfView * (scale - 1) / period));
    int wanted = neighbors * 2;
    while (entry->count < wanted) {
        HU3D_MODELID link = Hu3DModelLink(modelId);
        if (link == HU3D_MODELID_NONE) break;
        entry->links[entry->count++] = link;
        Hu3DModelCameraSet(link, source->cameraBit);
        Hu3DModelLayerSet(link, source->layerNo);
    }
    /* A failed first allocation must not consume a bounded registry slot. */
    if (wanted && !entry->count) {
        memset(entry, 0, sizeof(*entry));
        return;
    }
    for (int i = 0; i < entry->count; ++i) {
        int16_t id = entry->links[i];
        HU3D_MODEL *copy = &Hu3DData[id];
        if (!copy->hsf || copy->hsfLink != entry->hsf ||
            copy->linkMdlId != modelId) continue;
        if (i >= wanted || (source->attr & HU3D_ATTR_DISPOFF)) {
            Hu3DModelAttrSet(id, HU3D_ATTR_DISPOFF);
            continue;
        }
        Hu3DModelAttrReset(id, HU3D_ATTR_DISPOFF);
        copy->cameraBit = source->cameraBit;
        if (copy->layerNo != source->layerNo)
            Hu3DModelLayerSet(id, source->layerNo);
        copy->pos = source->pos;
        int distance = i / 2 + 1;
        int reflected = mirror && (distance & 1);
        copy->pos.x += ((i & 1) ? 1 : -1) * distance * period;
        if (reflected) copy->pos.x += 2.0f * centerX;
        copy->scale.x = reflected ? -1.0f : 1.0f;
        /* Reflection reverses winding, not visibility. Preserve the owner's
         * cull policy, reversing only its front/back selection. */
        copy->attr = (copy->attr & ~HU3D_ATTR_CULL_FRONT) |
            ((source->attr & HU3D_ATTR_CULL_FRONT) ^
             (reflected ? HU3D_ATTR_CULL_FRONT : 0));
        copy->motId = source->motId;
        copy->motWork = source->motWork;
        copy->motAttr = source->motAttr;
    }
}

void mp6_widescreen_tile_x(int16_t modelId, float period, float nativeHalfView)
{
    tile_apply(modelId, period, nativeHalfView, 0, 0.0f);
}

void mp6_widescreen_tile_mirror_x(int16_t modelId, float left, float right,
                                  float nativeHalfView)
{
    if (!isfinite(left) || !isfinite(right) || right <= left) return;
    tile_apply(modelId, right - left, nativeHalfView, 1, (left + right) * 0.5f);
}

void mp6_widescreen_tile_forget(int16_t modelId)
{
    for (int i = 0; i < TILE_LAYERS; ++i) {
        TileLayer *entry = &layers[i];
        if (!entry->hsf || entry->owner != modelId) continue;
        /* Retire before killing links: model teardown calls us recursively. */
        HSF_DATA *hsf = entry->hsf;
        entry->hsf = NULL;
        for (int j = 0; j < entry->count; ++j) {
            int16_t id = entry->links[j];
            if (Hu3DData && Hu3DData[id].hsf && Hu3DData[id].hsfLink == hsf &&
                Hu3DData[id].linkMdlId == modelId) Hu3DModelKill(id);
        }
        entry->count = 0;
    }
}
