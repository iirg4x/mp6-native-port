#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "REL/m667dll/recovered.h"
#include "game/disp.h"
#include "game/init.h"
#include "game/gamework.h"
#include "game/frand.h"
#include "game/memory.h"
#include "game/mic.h"
#include "mp6_widescreen.h"

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
u16 Hu3DCameraExistF;
GXRenderModeObj mode;
GXRenderModeObj *RenderMode = &mode;
float CZoomM[HU3D_CAM_MAX];
Vec CenterM[HU3D_CAM_MAX], CRotM[HU3D_CAM_MAX];
static float scale = 1;
float mp6_widescreen_scale_factor(void) { return scale; }
HUPROCESS *lbl_1_bss_0;
M667SceneView *lbl_1_bss_2C;
M667MapView *lbl_1_bss_18;
M667PlayerView *lbl_1_bss_1C[4];
M667CameraView *lbl_1_bss_10;
LightSetupView *lbl_1_bss_14;
M667RegistryView *lbl_1_bss_C;
M667Entries *lbl_1_bss_8;
char lbl_1_data_514[] = "/mic/ctx/m667_words";
s16 GwMgNightF;

static OMOBJ objects[16], modelObjects[7];
void fn_1_38E4(OMOBJ *object);
void fn_1_5AF8(OMOBJ *object);
static int objectCount, allocations, modelCount;
static size_t sizes[32];
static _Alignas(16) unsigned char memory[32][8192];
OMOBJ *omAddObjEx(OMOBJMAN *owner, s16 priority, u16 models, u16 motions,
                 s16 group, OMOBJ_FUNC callback)
{
    assert(owner == lbl_1_bss_0 && models == 0 && motions == 0 && group == 0);
    OMOBJ *object = &objects[objectCount++];
    object->objFunc = callback;
    return object;
}
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 owner)
{
    assert(heap == HEAP_HEAP && owner == HU_MEMNUM_OVL && allocations < 32);
    assert(bytes > 0 && bytes <= 8160);
    sizes[allocations] = bytes;
    memset(memory[allocations], 0xA5, sizeof(memory[allocations]));
    return memory[allocations++] + 16;
}
static void check_guards(void)
{
    for (int a = 0; a < allocations; ++a)
        for (int i = 0; i < 16; ++i) {
            assert(memory[a][i] == 0xA5);
            assert(memory[a][sizes[a] + 16 + i] == 0xA5);
        }
}
void fn_1_5C0(M667SceneView *scene) { assert(scene == lbl_1_bss_2C); }
u32 frandom(u32 seed) { return seed; }
s32 frandmod(s32 modulus) { assert(modulus > 0); return 0; }
OSTick OSGetTick(void) { return 1; }
s32 HuMCInit(s16 result) { assert(result == 0); return 0; }
s16 HuMCContextCreate(char *path) { assert(strcmp(path, lbl_1_data_514) == 0); return -1; }
OMOBJ *fn_1_7868(const M667ModelDescription *description)
{ assert(description && modelCount < 7); return &modelObjects[modelCount++]; }

#define UNUSED(name) void name(OMOBJ *object) { __builtin_trap(); }
UNUSED(fn_1_378C) UNUSED(fn_1_3ACC) UNUSED(fn_1_3D74) UNUSED(fn_1_6988)
UNUSED(fn_1_2D3C) UNUSED(fn_1_39D4) UNUSED(fn_1_5C6C)
void fn_1_5E64(HU3D_MODEL *model, Mtx *matrix) { __builtin_trap(); }
HU3D_MODELID Hu3DHookFuncCreate(HU3D_MODEL_HOOK hook)
{ assert(hook == fn_1_5E64); return 11; }
void fn_1_5D78(void) { __builtin_trap(); }
static int initializedEffects;
#define EFFECT(name) void name(M667ModelEffectView *item, s32 event) { \
    assert(event == 0 && (uintptr_t)item % _Alignof(M667ModelEffectView) == 0); \
    memset(item, 0x42, sizeof(*item)); ++initializedEffects; }
EFFECT(fn_1_67D8) EFFECT(fn_1_6844) EFFECT(fn_1_68B0) EFFECT(fn_1_691C)
EFFECT(fn_1_6060) EFFECT(fn_1_6064) EFFECT(fn_1_61A8)
static int emittedColor, emittedPosition, ended;
void GXColor4u8(u8 r, u8 g, u8 b, u8 a)
{ assert(r == 11 && g == 22 && b == 33 && a == 44); ++emittedColor; }
void GXPosition3f32(f32 x, f32 y, f32 z)
{ assert(x == 3 && y == 5 && z == 7); ++emittedPosition; }
void GXEnd(void) { ++ended; }
void PSVECSubtract(const Vec *a, const Vec *b, Vec *out)
{ *out = (Vec){a->x-b->x, a->y-b->y, a->z-b->z}; }
static Vec modelPosition;
void Hu3DModelRotSet(HU3D_MODELID model, f32 x, f32 y, f32 z) { }
void Hu3DModelPosSetV(HU3D_MODELID model, HuVecF *position) { modelPosition = *position; }

#include "m667_subject.inc"

int main(void)
{
    /* Real scene allocator and initializer, with typed guarded allocations. */
    fn_1_768();
    assert(sizes[0] == sizeof(M667SceneView));
    objects[0].objFunc(&objects[0]);
    const size_t expected[] = {sizeof(M667SceneView), sizeof(M667CameraView),
        sizeof(LightSetupView), sizeof(M667MapView), sizeof(M667PlayerView),
        sizeof(M667PlayerView), sizeof(M667PlayerView), sizeof(M667PlayerView),
        sizeof(M667RegistryView), sizeof(M667Entries)};
    assert(allocations == 10 && modelCount == 5);
    for (int i = 0; i < 10; ++i) assert(sizes[i] == expected[i]);
    for (int i = 0; i < 4; ++i) {
        assert(lbl_1_bss_1C[i]->index == i);
        MGACTOR_CORRECT_HOOK hook = fn_1_1DB8;
        hook(NULL, (intptr_t)lbl_1_bss_1C[i]);
        assert(lbl_1_bss_1C[i]->flags == 2);
    }
    /* Pool sizes and active iteration use the same native node/payload sizes. */
    fn_1_5AF8(&objects[8]);
    assert(initializedEffects == 135 && lbl_1_bss_C->model == 11);
    for (int type = 0; type < 7; ++type) {
        M667Pool *pool = lbl_1_bss_C->entries[type].pool;
        assert(sizes[10 + type] == sizeof(M667Pool) + lbl_1_data_494[type].count *
            (sizeof(M667PoolNode) + sizeof(M667ModelEffectView)));
        assert(pool->free == NULL && fn_1_7BA8(pool) == NULL);
        int visited = 0;
        for (void *item = fn_1_7C8C(pool); item; item = fn_1_7CC8(pool, item)) {
            assert(++visited <= lbl_1_data_494[type].count);
            assert(((M667ModelEffectView *)item)->flags == 0x4242);
        }
        assert(visited == lbl_1_data_494[type].count);
    }
    check_guards();
    /* Full descriptor lists preserve resource IDs, signed indices and slots. */
    const int models[2][7] = {{4,1,4,20,7,8,4}, {4,1,4,20,4,7,3}};
    const int motions[2][7] = {{4,0,4,5,3,2,1}, {4,0,4,5,1,1,0}};
    for (int night = 0; night < 2; ++night) {
        GwMgNightF = night;
        for (int kind = 0; kind < 7; ++kind) {
            const M667ModelDescription *desc = fn_1_8F58(kind);
            assert(desc->dataId >> 16 == 126 && desc->modelIndex == -1);
            int modelSlots, motionSlots;
            fn_1_7350((void *)desc, &modelSlots, &motionSlots);
            assert(modelSlots == models[night][kind]);
            assert(motionSlots == motions[night][kind]);
        }
    }
    assert(lbl_1_data_3C4[0].r == 255 && lbl_1_data_3C4[0].b == 0);
    assert(lbl_1_data_3C4[1].r == 0 && lbl_1_data_3C4[1].b == 255);
    fn_1_6C60((GXColor){11,22,33,44}); fn_1_6C6C(3,5,7); fn_1_6C5C();
    assert(emittedColor == 1 && emittedPosition == 1 && ended == 1);
    /* Exercise both the map's interpolation caller and its actual callee. */
    lbl_1_bss_18->records[0] = (M667MapModel){.elapsedFrames=0, .index=0, .durationFrames=4};
    for (int tick = 1; tick <= 4; ++tick) {
        fn_1_1338(0);
        assert(fabsf(modelPosition.x - (-1800 + 900 * tick)) < .001f);
        assert(modelPosition.y == 50 && modelPosition.z == -1300);
    }
    assert(fn_1_8E8C(9) == 3 && fn_1_8E8C(0) == 0 && fn_1_8E8C(-1) == -1);
    M667AIView ai = {.attackWeights={30,70,-1}};
    M667PlayerView target = {.mapX=20, .mapZ=20};
    M667PlayerView *player = lbl_1_bss_1C[0];
    player->ai = &ai; player->aiIntervalVariance = 10; player->aiActionInterval = 30;
    player->mapSlotIndex = 0;
    lbl_1_bss_18->currentPlayer = &target;
    lbl_1_bss_18->slots[0].x = lbl_1_bss_18->slots[0].z = 0;
    player->aiMoveTimer = 5; player->aiAttackTimer = 0;
    fn_1_5898(&objects[4]); assert(player->controlBits == 0 && player->aiMoveTimer == 4);
    target.mapX = 1; target.mapZ = 0;
    player->aiMoveTimer = 5; player->aiAttackTimer = 0;
    fn_1_5898(&objects[4]); assert(player->controlBits == 256);
    player->aiMoveTimer = 0; player->aiAttackTimer = 0;
    fn_1_5898(&objects[4]); assert(player->controlBits == (256 | 2));
    for (int ratio = 0; ratio < 3; ++ratio) {
        mode.fbWidth = (int[]){640,848,1120}[ratio];
        mode.efbHeight = mode.xfbHeight = 480;
        scale = (float)mode.fbWidth / 640;
        fn_1_38E4(&objects[1]);
        assert(Hu3DCamera[0].viewportW == mode.fbWidth && Hu3DCamera[0].viewportH == 480);
        assert(fabsf(Hu3DCamera[0].aspect - 1.2f * scale) < .0001f);
        assert(Hu3DCamera[0].near == 20 && Hu3DCamera[0].far == 15000);
        assert(lbl_1_bss_10->motionIndex == -1);
        Hu3DCameraAllKill();
    }
    check_guards();
    return 0;
}
