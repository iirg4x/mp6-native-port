#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "REL/m649Dll/module_types.h"
#include "game/disp.h"
#include "game/init.h"
#include "mp6_widescreen.h"

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
u16 Hu3DCameraExistF;
GXRenderModeObj mode;
GXRenderModeObj *RenderMode = &mode;
float CZoomM[HU3D_CAM_MAX];
Vec CenterM[HU3D_CAM_MAX], CRotM[HU3D_CAM_MAX];
OMOBJ *lbl_1_bss_2F8, *lbl_1_bss_2FC, *lbl_1_bss_300, *lbl_1_bss_304;
OMOBJ *lbl_1_bss_5A0, *lbl_1_bss_5A8, *lbl_1_bss_59C, *lbl_1_bss_39C[128];
HUPROCESS *lbl_1_bss_5A4;
M649PlayerState *lbl_1_bss_314[4];
s32 lbl_1_bss_310, lbl_1_bss_8, lbl_1_bss_2C[4];
s32 lbl_1_bss_6C, lbl_1_bss_68;
u32 lbl_1_bss_1C[4];
f32 lbl_1_bss_0, lbl_1_bss_4;
M649PropParam lbl_1_data_248[12] = {{.count = -1}};

static float scale = 1.0f;
float mp6_widescreen_scale_factor(void) { return scale; }
static OMOBJ allocatedObject;
static _Alignas(16) unsigned char allocation[4096];
static size_t requestedBytes;
static OMOBJMAN manager;
OMOBJ *omAddObjEx(OMOBJMAN *owner, s16 priority, u16 models, u16 motions,
                 s16 group, OMOBJ_FUNC callback)
{
    assert(owner == &manager && callback != NULL && group == -1);
    allocatedObject = (OMOBJ){0};
    allocatedObject.objFunc = callback;
    return &allocatedObject;
}
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 owner)
{
    assert(heap == HEAP_HEAP && owner == HU_MEMNUM_OVL);
    assert(bytes > 0 && bytes < sizeof(allocation) - 32);
    requestedBytes = bytes;
    memset(allocation, 0xA5, sizeof(allocation));
    return allocation + 16;
}
static void guards(size_t bytes)
{
    assert(allocatedObject.data == allocation + 16 && requestedBytes == bytes);
    for (size_t i = 0; i < 16; ++i) {
        assert(allocation[i] == 0xA5);
        assert(allocation[bytes + 16 + i] == 0xA5);
    }
}

/* These callbacks are installed, not executed by the allocation test. */
#define UNUSED_CALLBACK(name) void name(OMOBJ *object) { __builtin_trap(); }
UNUSED_CALLBACK(fn_1_6A88)
UNUSED_CALLBACK(fn_1_9014)
UNUSED_CALLBACK(fn_1_A2AC)
void fn_1_6A9C(HUPROCESS *process, M649PropParam *param) { __builtin_trap(); }
u32 frand(void) { return 1; }
static int effectSetup;
void fn_1_904C(OMOBJ *object) { assert(effectSetup++ == 0 && object == &allocatedObject); }
void fn_1_941C(OMOBJ *object) { assert(effectSetup++ == 1 && object == &allocatedObject); }
void fn_1_9934(OMOBJ *object) { assert(effectSetup++ == 2 && object == &allocatedObject); }

void fn_1_15A8(OMOBJ *object);
void fn_1_19BC(OMOBJ *object);
void fn_1_1A14(s16 layer) { __builtin_trap(); }
void fn_1_32EC(OMOBJ *object);
void fn_1_3354(OMOBJ *object);
void fn_1_33AC(OMOBJ *object);
void fn_1_29E4(u32 cameras, float x, float y, float width, float height);
void fn_1_2A64(u32 cameras, float x, float y, float width, float height);
void fn_1_512C(void *state);

/* Engine boundaries are mocked; the work records, allocator sizes, update
 * functions and actual shared widescreen camera implementation are not. */
void PSVECSubtract(const Vec *a, const Vec *b, Vec *out)
{ *out = (Vec){a->x - b->x, a->y - b->y, a->z - b->z}; }
void PSVECNormalize(const Vec *a, Vec *out)
{
    float length = sqrtf(a->x*a->x + a->y*a->y + a->z*a->z);
    *out = (Vec){a->x/length, a->y/length, a->z/length};
}
static int fogLayers;
void Hu3DFogSet(float start, float end, u8 r, u8 g, u8 b)
{ assert(start == 10 && end == 100 && r == 1 && g == 2 && b == 3); }
void Hu3DLayerHookSet(s16 layer, HU3D_LAYER_HOOK callback)
{ assert(layer == 2 && callback == fn_1_1A14); fogLayers++; }
static int callbackCalls;
static void owner_callback(OMOBJ *object)
{ assert(object == &allocatedObject); callbackCalls++; }
static int panCalls, volumeCalls;
s32 HuAudFXStatusGet(s32 handle) { return MSM_SE_PAUSEIN; }
s32 fn_1_3630(u32 cameras, Vec *position) { assert(position != NULL); return 80; }
s32 fn_1_3850(u32 cameras, Vec *position) { assert(position != NULL); return 90; }
void HuAudFXPanning(s32 handle, s16 pan) { assert(pan == 80); panCalls++; }
s32 HuAudFXVolSet(s32 handle, s16 volume) { assert(volume == 90); volumeCalls++; return 0; }
static int timerValue;
s32 MgSeqTimerValueGet(void) { return timerValue; }
static int sequenceAdvances;
u32 MgSeqModeNext(void) { sequenceAdvances++; return 0; }
u16 MgSeqModeChangeOff(void) { return 0; }
static MGSCORE scoreSentinel;
static int scorePositionCalls;
void MgScoreBoxPosSet(HUSPR_GROUPID box, float x, float y)
{ assert(box == 42 && x == 50 && y == 60); scorePositionCalls++; }
void MgScorePosSet(MGSCORE *score, float x, float y)
{ assert(score == &scoreSentinel && x == 57 && y == 60); scorePositionCalls++; }
void fn_1_211C(OMOBJ *object, u32 camera, s32 index) { __builtin_trap(); }
static int shadowSetups;
void fn_1_2590(s32 cameras, Vec *direction, Vec *target, float fov, float radius)
{ assert(cameras == 15 && radius == 500); shadowSetups++; }
void Hu3DShadowColSet(u8 r, u8 g, u8 b) {}

/* Real per-owner data is mutated/restored separately in test_native_minigames. */
static int resetStage;
void mp6_m649_main_reset(void) { assert(resetStage++ == 0); }
void mp6_m649_effect_reset(void) { assert(resetStage++ == 1); }
void mp6_m649_prop_reset(void) { assert(resetStage++ == 2); }
void mp6_m649_score_reset(void) { assert(resetStage++ == 3); }

#include "m649_subject.inc"

static void near(float actual, float expected) { assert(fabsf(actual - expected) < 0.003f); }

int main(void)
{
    _Static_assert(sizeof(void *) == 8, "exercise full native pointer widths");
    _Static_assert(sizeof(M649AudioWork) > 1544, "audio entries hold native position pointers");
    _Static_assert(sizeof(M649EffectOwner) > 8, "two animation pointers");
    _Static_assert(sizeof(M649ScoreWorkView) > 84, "four score pointers");
    for (int visit = 0; visit < 3; ++visit) {
        resetStage = 0; mp6_m649_reset(); assert(resetStage == 4);
    }

    Vec position = {0, 0, 0}, target = {0, 0, 10};
    GXColor color = {4, 5, 6, 7};
    fn_1_14A0(&manager, &position, &target, &color);
    M649LightWork *light = allocatedObject.data;
    assert(lbl_1_bss_304 == &allocatedObject && light->light == -1);
    fn_1_1718(owner_callback); fn_1_15A8(&allocatedObject);
    assert(callbackCalls == 1 && light->callback == owner_callback);
    near(light->direction.z, 1); assert(light->color.a == 7);
    guards(sizeof(*light));

    M649FogParam fog[] = {{2, 1, GX_FOG_PERSP_EXP, 10, 100, {1,2,3,4}}, {.layer = -1}};
    fn_1_18BC(&manager, fog);
    M649FogWork *fogWork = allocatedObject.data;
    fn_1_1B08(owner_callback); fn_1_19BC(&allocatedObject);
    assert(callbackCalls == 2 && fogLayers == 1 && fogWork->parameters == fog);
    near(fogWork->factor, 1); guards(sizeof(*fogWork));

    M649AudioConfigView config[] = {{.cameras = 15}, {0}};
    fn_1_3250(&manager, config);
    M649AudioWork *audio = allocatedObject.data;
    assert(lbl_1_bss_2F8 == &allocatedObject && audio->config == config);
    for (int i = 0; i < 64; ++i) {
        assert(audio->entries[i].handle == -1);
        audio->entries[i] = (M649AudioEntry){i, 10, 1, 1, 1, &position};
    }
    fn_1_347C(owner_callback); fn_1_32EC(&allocatedObject);
    assert(callbackCalls == 3 && panCalls == 64 && volumeCalls == 64);
    assert(audio->config == config && audio->callback == owner_callback);
    for (int i = 0; i < 64; ++i) assert(audio->entries[i].position == &position);
    guards(sizeof(*audio));

    fn_1_8F58(&manager); guards(sizeof(M649EffectOwner));
    assert(effectSetup == 3 && lbl_1_bss_5A4 == &manager && lbl_1_bss_5A0 == &allocatedObject);
    M649EffectOwner *effects = allocatedObject.data;
    effects->animations[0] = (ANIMDATA *)&position;
    effects->animations[1] = (ANIMDATA *)&target;
    guards(sizeof(*effects));
    memset(lbl_1_bss_39C, 0xA5, sizeof(lbl_1_bss_39C));
    fn_1_6990(&manager); guards(sizeof(M649Scene));
    assert(((M649Scene *)allocatedObject.data)->direction == 1);
    for (int i = 0; i < 128; ++i) assert(lbl_1_bss_39C[i] == NULL);

    fn_1_A220(&manager); guards(sizeof(M649ScoreWorkView));
    M649ScoreWorkView *scores = allocatedObject.data;
    fn_1_A600();
    for (int i = 0; i < 4; ++i) {
        scores->entries[i].score = &scoreSentinel;
        fn_1_A5A4(i, i * 40); assert(fn_1_A644(i) == (i == 3 ? 99 : i * 40));
    }
    scores->entries[3].x = 50; scores->entries[3].y = 60; scores->entries[3].box = 42;
    fn_1_A528(&scores->entries[3]); assert(scorePositionCalls == 2);
    guards(sizeof(*scores));

    M649PlayerState players[4] = {0};
    for (int i = 0; i < 4; ++i) lbl_1_bss_314[i] = &players[i];
    fn_1_5044(NULL);
    for (int i = 0; i < 4; ++i) { assert(lbl_1_bss_314[i] == NULL); lbl_1_bss_314[i] = &players[i]; }
    for (int tick = 0; tick < 10; ++tick) fn_1_5094(NULL);
    for (int i = 0; i < 4; ++i) assert(players[i].updateCount == 10);
    assert(lbl_1_bss_310 == 1);
    const float delays[] = {60, 36, 24, 18}, errors[] = {.95f, .8f, .6f, .5f};
    const int targets[] = {7, 12, 19, 23};
    for (int i = 0; i < 4; ++i) {
        near(lbl_1_data_1C4[i].firstDelay, delays[i]); near(lbl_1_data_1C4[i].error, errors[i]);
        assert(lbl_1_data_1C4[i].targetScore == targets[i]);
    }
    timerValue = 17; assert(fn_1_1244() == 17);
    for (int i = 0; i < 4; ++i) { lbl_1_bss_2C[i] = -1; lbl_1_bss_1C[i] = i; }
    assert(fn_1_127C(9) == 0); lbl_1_bss_2C[1] = 1;
    assert(fn_1_127C(1) == 1 && fn_1_127C(0) == -1 && fn_1_127C(9) == 0);

    OM_CAMERA_VIEW start = {{1,2,3}, {4,5,6}, 100}, end = {{11,22,33}, {44,55,66}, 200}, result;
    fn_1_BE0(&start, &end, &result, .25f);
    near(result.center.x, 3.5f); near(result.center.y, 7); near(result.center.z, 10.5f);
    near(result.rot.x, 14); near(result.rot.y, 17.5f); near(result.rot.z, 21); near(result.zoom, 125);

    Mtx transform = {{2,0,0,10}, {0,3,0,20}, {0,0,4,30}}, translated;
    PSMTXTransApply(transform, translated, 5, 6, 7);
    near(translated[0][3], 15); near(translated[1][3], 26); near(translated[2][3], 37);
    assert(translated[0][0] == 2 && translated[1][1] == 3 && translated[2][2] == 4);
    PSMTXTransApply(translated, translated, -5, -6, -7);
    assert(memcmp(transform, translated, sizeof(transform)) == 0);

    /* All four cameras pass through the real module update and actual shared
     * Hu3D setters/mapper, including empty opening spans and the real sequence
     * callback's opening wait, 60-tick split, and final delay. */
    for (int ratio = 0; ratio < 3; ++ratio) {
        mode.fbWidth = (int[]){640, 848, 1120}[ratio];
        mode.efbHeight = mode.xfbHeight = 480;
        scale = (float)mode.fbWidth / 640;
        M649CameraWork cameraWork = {.cameras = 15};
        OMOBJ cameraObject = {.data = &cameraWork};
        lbl_1_bss_2FC = &cameraObject;
        Hu3DCameraCreate(15); fn_1_2780(fn_1_C78);
        for (int i = 0; i < 4; ++i) {
            cameraWork.entries[i].view = lbl_1_data_0;
            cameraWork.entries[i].previousView = end;
            cameraWork.entries[i].phase = 2;
            cameraWork.entries[i].aspect = 1.2f;
        }
        Hu3DCameraPerspectiveSet(15, 45, 20, 2000, 1.2f);
        lbl_1_bss_4 = 640; lbl_1_bss_0 = 480;
        lbl_1_bss_8 = 0;
        sequenceAdvances = 0;
        for (int tick = 0; tick <= 240; ++tick) {
            fn_1_36C(MGSEQ_MODE_FADEIN, tick);
            fn_1_1EF4(&cameraObject);
            for (int i = 0; i < 4; ++i) {
                M649CameraEntry *entry = &cameraWork.entries[i];
                HU3D_CAMERA *camera = &Hu3DCamera[i];
                assert(camera->scissorX + camera->scissorW <= mode.fbWidth);
                assert(camera->scissorY + camera->scissorH <= 480);
                if (tick == 0 && (i & 1)) assert(camera->scissorW == 0);
                if (tick == 0 && (i & 2)) assert(camera->scissorH == 0);
                near(camera->viewportX, entry->viewportX * scale);
                near(camera->viewportW, entry->viewportWidth * scale);
                near(camera->aspect, 1.2f * scale);
                near(CenterM[i].x, entry->view.center.x);
                if (tick == 240) {
                    near(entry->scissorWidth, 318); near(entry->scissorHeight, 238);
                    near(entry->scissorX, (i & 1) ? 322 : 0);
                    near(entry->scissorY, (i & 2) ? 242 : 0);
                    near(entry->view.zoom, 200); near(entry->view.center.z, 33);
                }
            }
        }
        assert(sequenceAdvances > 0 && lbl_1_bss_8 == 1);
        Hu3DCameraAllKill();
    }
    assert(shadowSetups == 3);
    return 0;
}
