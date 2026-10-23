#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game/hu3d.h"
#include "mp6_widescreen.h"

HU3D_MODEL *Hu3DData;

static HU3D_MODEL g_models[HU3D_MODEL_MAX];
static HSF_DATA g_hsfs[10];
static HSF_DATA g_hsfNative[10];
static unsigned char g_arenaSentinel[256];
static unsigned char g_arenaNative[sizeof(g_arenaSentinel)];
static float g_scale = 1.0f;
static int g_nextLink;
static int g_linkCalls;
static int g_killCalls;
static int g_cameraCalls;
static int g_layerCalls;
static int g_attrSetCalls;
static int g_attrResetCalls;
static int g_nestedForget;
static int g_nestedForgetCalls;
static int g_linkFailure;

void mp6_widescreen_tile_x(int16_t modelId, float period,
                           float nativeHalfView);
void mp6_widescreen_tile_forget(int16_t modelId);

float mp6_widescreen_scale_factor(void)
{
    return g_scale;
}

HU3D_MODELID Hu3DModelLink(HU3D_MODELID ownerId)
{
    HU3D_MODELID linkId;
    HU3D_MODEL *source;
    HU3D_MODEL *copy;

    ++g_linkCalls;
    if (g_linkFailure || g_nextLink >= HU3D_MODEL_MAX) {
        return HU3D_MODELID_NONE;
    }
    linkId = (HU3D_MODELID)g_nextLink++;
    source = &Hu3DData[ownerId];
    copy = &Hu3DData[linkId];
    memset(copy, 0, sizeof(*copy));
    copy->hsf = source->hsf;
    copy->hsfLink = source->hsf;
    copy->linkMdlId = ownerId;
    copy->cameraBit = source->cameraBit;
    copy->layerNo = source->layerNo;
    copy->pos = source->pos;
    copy->rot = source->rot;
    copy->scale = source->scale;
    copy->motId = source->motId;
    copy->motWork = source->motWork;
    copy->motAttr = source->motAttr;
    return linkId;
}

void Hu3DModelKill(HU3D_MODELID modelId)
{
    ++g_killCalls;
    if (g_nestedForget) {
        ++g_nestedForgetCalls;
        /* Simulate a teardown callback re-entering the owner's forget path. */
        mp6_widescreen_tile_forget(0);
    }
    Hu3DData[modelId].hsf = NULL;
    Hu3DData[modelId].hsfLink = NULL;
    Hu3DData[modelId].linkMdlId = HU3D_MODELID_NONE;
}

void Hu3DModelCameraSet(HU3D_MODELID modelId, u16 cameraBit)
{
    ++g_cameraCalls;
    Hu3DData[modelId].cameraBit = cameraBit;
}

void Hu3DModelLayerSet(HU3D_MODELID modelId, s16 layerNo)
{
    ++g_layerCalls;
    Hu3DData[modelId].layerNo = layerNo;
}

void Hu3DModelAttrSet(HU3D_MODELID modelId, u32 attr)
{
    ++g_attrSetCalls;
    Hu3DData[modelId].attr |= attr;
}

void Hu3DModelAttrReset(HU3D_MODELID modelId, u32 attr)
{
    ++g_attrResetCalls;
    Hu3DData[modelId].attr &= ~attr;
}

static void setup_source(int id)
{
    memset(&g_models[id], 0, sizeof(g_models[id]));
    memset(&g_hsfs[id], 0, sizeof(g_hsfs[id]));
    g_models[id].hsf = &g_hsfs[id];
    g_models[id].linkMdlId = HU3D_MODELID_NONE;
    g_models[id].pos = (HuVecF){100.0f + id, 3.0f, 7.0f};
    g_models[id].rot = (HuVecF){0.0f, 0.0f, 0.0f};
    g_models[id].scale = (HuVecF){1.0f, 1.0f, 1.0f};
    g_models[id].cameraBit = (u16)(1u << (id & 3));
    g_models[id].layerNo = (s16)(4 + id);
    g_models[id].motId = (HU3D_MOTIONID)(20 + id);
    g_models[id].motWork.time = 12.5f + id;
    g_models[id].motWork.speed = 0.5f;
    g_models[id].motWork.start = 0.0f;
    g_models[id].motWork.end = 40.0f;
    g_models[id].motAttr = HU3D_MOTATTR_LOOP;
    memcpy(&g_hsfNative[id], &g_hsfs[id], sizeof(g_hsfs[id]));
}

static void check_source_native(int id)
{
    assert(memcmp(&g_hsfs[id], &g_hsfNative[id], sizeof(g_hsfs[id])) == 0);
}

static void check_link_phase(int16_t id, int16_t owner, float expectedX,
                             const HU3D_MODEL *source)
{
    const HU3D_MODEL *copy = &g_models[id];
    assert(copy->hsf == source->hsf);
    assert(copy->hsfLink == source->hsf);
    assert(copy->linkMdlId == owner);
    assert(fabsf(copy->pos.x - expectedX) < 0.0001f);
    assert(copy->pos.y == source->pos.y && copy->pos.z == source->pos.z);
    assert(copy->cameraBit == source->cameraBit);
    assert(copy->layerNo == source->layerNo);
    assert(copy->motId == source->motId);
    assert(memcmp(&copy->motWork, &source->motWork,
                  sizeof(copy->motWork)) == 0);
    assert(copy->motAttr == source->motAttr);
}

int main(void)
{
    int16_t links[4];
    int oldLinkCalls;
    int oldKills;
    int id;

    memset(g_models, 0, sizeof(g_models));
    memset(g_hsfs, 0, sizeof(g_hsfs));
    memset(g_hsfNative, 0, sizeof(g_hsfNative));
    memset(g_arenaSentinel, 0xA5, sizeof(g_arenaSentinel));
    memcpy(g_arenaNative, g_arenaSentinel, sizeof(g_arenaSentinel));
    Hu3DData = g_models;
    for (id = 0; id < HU3D_MODEL_MAX; ++id) {
        g_models[id].linkMdlId = HU3D_MODELID_NONE;
    }
    setup_source(0);

    /* Native aspect creates no companion and does not touch the HSF/arena. */
    g_scale = 1.0f;
    g_nextLink = 100;
    g_linkCalls = g_cameraCalls = g_layerCalls = 0;
    mp6_widescreen_tile_x(0, 50.0f, 100.0f);
    assert(g_linkCalls == 0 && g_cameraCalls == 0 && g_layerCalls == 0);
    check_source_native(0);
    assert(memcmp(g_arenaSentinel, g_arenaNative,
                  sizeof(g_arenaSentinel)) == 0);

    /* Wide view needs two copies on each side. Links share HSF storage but
     * copy the source's current animation phase and layer/camera state. */
    g_scale = 2.0f;
    mp6_widescreen_tile_x(0, 50.0f, 100.0f);
    assert(g_linkCalls == 4 && g_cameraCalls == 4 && g_layerCalls == 4);
    links[0] = 100;
    links[1] = 101;
    links[2] = 102;
    links[3] = 103;
    check_link_phase(links[0], 0, 50.0f, &g_models[0]);
    check_link_phase(links[1], 0, 150.0f, &g_models[0]);
    check_link_phase(links[2], 0, 0.0f, &g_models[0]);
    check_link_phase(links[3], 0, 200.0f, &g_models[0]);
    check_source_native(0);

    /* Returning to native aspect hides (rather than destroys) links so a
     * resize back to wide can reuse them without allocating a new layer. */
    g_scale = 1.0f;
    mp6_widescreen_tile_x(0, 50.0f, 100.0f);
    for (id = 0; id < 4; ++id) {
        assert((g_models[links[id]].attr & HU3D_ATTR_DISPOFF) != 0);
    }
    oldLinkCalls = g_linkCalls;
    g_scale = 2.0f;
    mp6_widescreen_tile_x(0, 50.0f, 100.0f);
    assert(g_linkCalls == oldLinkCalls);
    for (id = 0; id < 4; ++id) {
        assert((g_models[links[id]].attr & HU3D_ATTR_DISPOFF) == 0);
    }

    /* Teardown retires the layer before Hu3DModelKill; nested forget calls
     * therefore cannot recurse into the same link list. */
    g_nestedForget = 1;
    oldKills = g_killCalls;
    mp6_widescreen_tile_forget(0);
    g_nestedForget = 0;
    assert(g_killCalls == oldKills + 4);
    assert(g_nestedForgetCalls == 4);
    for (id = 0; id < 4; ++id) {
        assert(g_models[links[id]].hsf == NULL);
        assert(g_models[links[id]].hsfLink == NULL);
    }
    check_source_native(0);
    assert(memcmp(g_arenaSentinel, g_arenaNative,
                  sizeof(g_arenaSentinel)) == 0);
    oldKills = g_killCalls;
    mp6_widescreen_tile_forget(0);
    assert(g_killCalls == oldKills);

    /* Invalid inputs and source state reject without consuming a layer. */
    setup_source(9);
    oldLinkCalls = g_linkCalls;
    mp6_widescreen_tile_x(-1, 50.0f, 100.0f);
    mp6_widescreen_tile_x(HU3D_MODEL_MAX, 50.0f, 100.0f);
    mp6_widescreen_tile_x(9, 0.0f, 100.0f);
    mp6_widescreen_tile_x(9, 50.0f, 0.0f);
    g_models[9].hsf = NULL;
    mp6_widescreen_tile_x(9, 50.0f, 100.0f);
    g_models[9].hsf = &g_hsfs[9];
    g_models[9].linkMdlId = 8;
    mp6_widescreen_tile_x(9, 50.0f, 100.0f);
    g_models[9].linkMdlId = HU3D_MODELID_NONE;
    g_models[9].attr = HU3D_ATTR_CAMERA;
    mp6_widescreen_tile_x(9, 50.0f, 100.0f);
    g_models[9].attr = HU3D_ATTR_HOOK;
    mp6_widescreen_tile_x(9, 50.0f, 100.0f);
    g_models[9].attr = 0;
    g_models[9].rot.x = 1.0f;
    mp6_widescreen_tile_x(9, 50.0f, 100.0f);
    g_models[9].rot.x = 0.0f;
    g_scale = NAN;
    mp6_widescreen_tile_x(9, 50.0f, 100.0f);
    g_scale = 2.0f;
    assert(g_linkCalls == oldLinkCalls);
    check_source_native(9);

    /* A nonfinite aspect must not reserve one of the bounded layers. */
    oldLinkCalls = g_linkCalls;
    for (id = 1; id <= 8; ++id) {
        setup_source(id);
        mp6_widescreen_tile_x((int16_t)id, 50.0f, 100.0f);
    }
    assert(g_linkCalls == oldLinkCalls + 32);
    for (id = 1; id <= 8; ++id) mp6_widescreen_tile_forget((int16_t)id);
    mp6_widescreen_tile_forget(9);

    /* A failed Hu3DModelLink must likewise leave its layer slot reusable. */
    setup_source(9);
    g_linkFailure = 1;
    oldLinkCalls = g_linkCalls;
    mp6_widescreen_tile_x(9, 50.0f, 100.0f);
    assert(g_linkCalls == oldLinkCalls + 1);
    g_linkFailure = 0;
    oldLinkCalls = g_linkCalls;
    for (id = 1; id <= 8; ++id) {
        setup_source(id);
        mp6_widescreen_tile_x((int16_t)id, 50.0f, 100.0f);
    }
    assert(g_linkCalls == oldLinkCalls + 32);
    for (id = 1; id <= 8; ++id) mp6_widescreen_tile_forget((int16_t)id);
    mp6_widescreen_tile_forget(9);
    check_source_native(9);

    /* Exhaust all bounded layer slots, then verify a new model fails without
     * mutating its source slot. */
    for (id = 1; id <= 8; ++id) {
        setup_source(id);
        mp6_widescreen_tile_x((int16_t)id, 50.0f, 100.0f);
    }
    setup_source(9);
    oldLinkCalls = g_linkCalls;
    {
        HU3D_MODEL before = g_models[9];
        mp6_widescreen_tile_x(9, 50.0f, 100.0f);
        assert(g_linkCalls == oldLinkCalls);
        assert(memcmp(&g_models[9], &before, sizeof(before)) == 0);
    }
    check_source_native(9);
    assert(memcmp(g_arenaSentinel, g_arenaNative,
                  sizeof(g_arenaSentinel)) == 0);

    for (id = 1; id <= 8; ++id) mp6_widescreen_tile_forget((int16_t)id);
    setup_source(0);
    g_scale = 1.5f;
    int mirrorLeft = g_nextLink;
    mp6_widescreen_tile_mirror_x(0, -20.0f, 60.0f, 100.0f);
    check_link_phase(mirrorLeft, 0, 60.0f, &g_models[0]);
    check_link_phase(mirrorLeft + 1, 0, 220.0f, &g_models[0]);
    assert(g_models[mirrorLeft].scale.x == -1.0f);
    assert(g_models[mirrorLeft + 1].scale.x == -1.0f);
    assert(g_models[mirrorLeft].attr & HU3D_ATTR_CULL_FRONT);
    /* Reflected endpoints meet the original at both off-center seams. */
    assert(g_models[mirrorLeft].pos.x - (-20.0f) == g_models[0].pos.x - 20.0f);
    assert(g_models[mirrorLeft + 1].pos.x - 60.0f == g_models[0].pos.x + 60.0f);
    g_models[0].attr |= HU3D_ATTR_CULL_FRONT;
    mp6_widescreen_tile_mirror_x(0, -20.0f, 60.0f, 100.0f);
    assert(!(g_models[mirrorLeft].attr & HU3D_ATTR_CULL_FRONT));
    g_scale = 1.0f;
    mp6_widescreen_tile_mirror_x(0, -20.0f, 60.0f, 100.0f);
    assert(g_models[mirrorLeft].attr & HU3D_ATTR_DISPOFF);
    check_source_native(0);
    mp6_widescreen_tile_forget(0);

    puts("widescreen tile native probe: PASS");
    return 0;
}
