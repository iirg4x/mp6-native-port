#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "REL/m643DLL/m643.h"
#include "game/memory.h"
#include "game/disp.h"
#include "game/init.h"
#include "mp6_widescreen.h"

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
u16 Hu3DCameraExistF;
GXRenderModeObj mode;
GXRenderModeObj *RenderMode = &mode;
OMOBJMAN *lbl_1_bss_0;
M643PlayerPrefix *lbl_1_bss_174[2];
s16 GwMgNightF;
s32 lbl_1_bss_C[2][39];
s32 lbl_1_bss_8;
HuVecF lbl_1_data_C4[4][39];
static float scale = 1.0f;
float mp6_widescreen_scale_factor(void) { return scale; }
void MgActorPosGet(MGACTOR *actor, HuVecF *position) { *position = actor->pos; }

static u32 modelAttrs[512];
void Hu3DModelAttrSet(HU3D_MODELID modelId, u32 attr)
{
    assert((unsigned)modelId < 512);
    modelAttrs[(unsigned)modelId] |= attr;
}
void Hu3DModelAttrReset(HU3D_MODELID modelId, u32 attr)
{
    assert((unsigned)modelId < 512);
    modelAttrs[(unsigned)modelId] &= ~attr;
}
void Hu3DCameraPosGet(int cameraBit, Vec *position, Vec *up, Vec *target)
{
    (void)cameraBit;
    *position = (Vec){0.0f, 0.0f, 0.0f};
    *up = (Vec){0.0f, 1.0f, 0.0f};
    *target = (Vec){0.0f, 0.0f, 0.0f};
}
void Hu3DShadowMultiPosSet(HuVecF *position, HuVecF *up, HuVecF *target, s16 cameraBit)
{
    (void)position;
    (void)up;
    (void)target;
    (void)cameraBit;
}

/* These callbacks are stored, not executed by the allocator/camera subject. */
#define UNUSED_CALLBACK(name) void name(OMOBJ *object) { __builtin_trap(); }
UNUSED_CALLBACK(fn_1_17A0)
UNUSED_CALLBACK(fn_1_2D90)
UNUSED_CALLBACK(fn_1_23C4)
UNUSED_CALLBACK(fn_1_4AEC)
UNUSED_CALLBACK(fn_1_83C8)
UNUSED_CALLBACK(fn_1_2838)
UNUSED_CALLBACK(fn_1_B130)
UNUSED_CALLBACK(fn_1_A138)
UNUSED_CALLBACK(fn_1_3680)
UNUSED_CALLBACK(fn_1_40FC)
void fn_1_3374(OMOBJ *object);

static OMOBJ allocatedObject;
static _Alignas(16) unsigned char allocation[1024];
static size_t requestedBytes;
static OMOBJ_FUNC expectedCallback;
OMOBJ *omAddObjEx(OMOBJMAN *manager, s16 priority, u16 models, u16 motions,
                 s16 group, OMOBJ_FUNC callback)
{
    assert(manager == lbl_1_bss_0 && models == 0 && motions == 0 && group == 0);
    assert(priority > 0 && callback == expectedCallback);
    allocatedObject.objFunc = callback;
    return &allocatedObject;
}
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 owner)
{
    assert(heap == HEAP_HEAP && owner == HU_MEMNUM_OVL);
    assert(bytes > 0 && bytes < sizeof(allocation) - 32);
    requestedBytes = bytes;
    return allocation + 16;
}

/* Actual per-owner storage is exercised in test_native_minigames. Here the
 * real combined entrypoint must dispatch both owners, once and in order. */
static int resetStage;
void mp6_m643_startup_reset(void) { assert(resetStage++ == 0); }
void mp6_m643_game_reset(void) { assert(resetStage++ == 1); }

#include "m643_subject.inc"

static void close_to(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.002f);
}

static void check_allocation(void *data, size_t size)
{
    assert(data == allocation + 16 && requestedBytes == size);
    assert(allocatedObject.data == data);
    for (size_t i = 0; i < sizeof(allocation); ++i)
        assert(allocation[i] == ((i >= 16 && i < size + 16) ? 0 : 0xA5));
    /* Writing the full record must not touch either heap guard. */
    memset(data, 0x5A, size);
    for (size_t i = 0; i < 16; ++i) {
        assert(allocation[i] == 0xA5);
        assert(allocation[size + 16 + i] == 0xA5);
    }
}

/* Set the expected callback from the expression itself, through a wrapper. */
static void *allocate_checked(s32 priority, u32 bytes, OMOBJ_FUNC callback)
{
    expectedCallback = callback;
    return fn_1_16C(priority, bytes, callback);
}
#define CHECK_ALLOCATION(type, call) do { \
    memset(allocation, 0xA5, sizeof(allocation)); \
    check_allocation((call), sizeof(type)); \
} while (0)

int main(void)
{
    for (int visit = 0; visit < 3; ++visit) {
        resetStage = 0;
        mp6_m643_reset();
        assert(resetStage == 2);
    }
    _Static_assert(sizeof(void *) == 8, "exercise native 64-bit work records");
    _Static_assert(sizeof(M643PlayerPrefix) > 60, "runner contains native pointers");
    _Static_assert(sizeof(M643TeamScenePrefix) > 172, "cannon contains native pointers");
    _Static_assert(sizeof(M643CameraEffectWork) > 104, "rescue contains a native pointer");
    _Static_assert(sizeof(M643ShotPrefix) > 148, "shot contains a native pointer");
#define fn_1_16C allocate_checked
#include "m643_allocations.inc"
#undef fn_1_16C

    expectedCallback = NULL;
    assert(fn_1_16C(1, 0, NULL) == NULL && allocatedObject.data == NULL);
    MGACTOR actors[2] = {0};
    MGPLAYER players[2] = {0};
    M643PlayerPrefix runners[2] = {0};
    struct { uint64_t before; M643ViewControlWork work; uint64_t after; } controls[2];
    OMOBJ objects[2] = {0};
    for (int team = 0; team < 2; ++team) {
        players[team].actor = &actors[team];
        runners[team].player = &players[team];
        lbl_1_bss_174[team] = &runners[team];
        objects[team].data = &controls[team].work;
    }
    for (int ratio = 0; ratio < 3; ++ratio) {
        mode.fbWidth = (int[]){640, 848, 1120}[ratio];
        scale = (float)mode.fbWidth / 640;
        mode.efbHeight = mode.xfbHeight = 480;
        Hu3DCameraAllKill();
        Hu3DCameraCreate(3);
        for (int team = 0; team < 2; ++team) {
            memset(&controls[team], 0, sizeof(controls[team]));
            controls[team].before = controls[team].after = UINT64_C(0x12345678ABCDEF09);
            controls[team].work.teamIndex = team;
            fn_1_3374(&objects[team]);
            assert(objects[team].objFunc == fn_1_3680);
            close_to(Hu3DCamera[team].viewportW, 640 * scale);
            close_to(Hu3DCamera[team].aspect, lbl_1_rodata_E8[team][0] * scale);
        }
        for (int tick = 1; tick <= 60; ++tick) {
            for (int team = 0; team < 2; ++team) {
                actors[team].pos = (Vec){-2000 + tick * 80, 10 + team, 30 + team};
                fn_1_3A98(&objects[team]);
                float t = tick / 60.0f;
                float height = (1-t)*lbl_1_rodata_68[team][0][3] + t*lbl_1_rodata_68[team][1][3];
                float y = (1-t)*lbl_1_rodata_68[team][0][1] + t*lbl_1_rodata_68[team][1][1];
                close_to(Hu3DCamera[team].viewportW, 640*scale);
                close_to(Hu3DCamera[team].viewportH, height);
                close_to(Hu3DCamera[team].viewportY, y);
                close_to(Hu3DCamera[team].aspect, (team ? 640.0f/310 : 640.0f/height)*scale);
                close_to(Hu3DCamera[team].target.x, fminf(actors[team].pos.x + 500, 2060));
                close_to(Hu3DCamera[team].target.z, actors[team].pos.z);
                /* The game truncates each interpolated argument at the
                 * unsigned GX API boundary.  The shared mapper then clips
                 * the native interval to the 640-pixel framebuffer and maps
                 * both endpoints, so a split that starts off-screen must not
                 * be compared with an unclipped width*scale value. */
                unsigned nativeX = (1-t)*lbl_1_rodata_A8[team][0][0] +
                                   t*lbl_1_rodata_A8[team][1][0];
                unsigned nativeWidth = (1-t)*lbl_1_rodata_A8[team][0][2] +
                                       t*lbl_1_rodata_A8[team][1][2];
                if (nativeX >= 640) {
                    nativeWidth = 0;
                } else if (nativeWidth > 640 - nativeX) {
                    nativeWidth = 640 - nativeX;
                }
                unsigned expectedScissorX =
                    (unsigned)((double)nativeX * scale + 0.5);
                unsigned expectedScissorRight =
                    (unsigned)((double)(nativeX + nativeWidth) * scale + 0.5);
                assert(Hu3DCamera[team].scissorX == expectedScissorX);
                assert(Hu3DCamera[team].scissorW ==
                       expectedScissorRight - expectedScissorX);
                assert(Hu3DCamera[team].scissorY + Hu3DCamera[team].scissorH <= 480);
                assert(controls[team].work.phaseFrames == tick);
                assert(controls[team].before == UINT64_C(0x12345678ABCDEF09));
                assert(controls[team].after == UINT64_C(0x12345678ABCDEF09));
                if (tick == 60) assert(objects[team].objFunc == fn_1_40FC);
            }
            /* Reapply must not double-widen or freeze the animated vertical geometry. */
            HU3D_CAMERA before[2] = {Hu3DCamera[0], Hu3DCamera[1]};
            mp6_widescreen_camera_reapply();
            assert(memcmp(before, Hu3DCamera, sizeof(before)) == 0);
        }
        assert(Hu3DCamera[0].scissorY == 0 && Hu3DCamera[0].scissorH == 238);
        assert(Hu3DCamera[1].scissorY == 242 && Hu3DCamera[1].scissorH == 238);
    }

    /* Execute the real per-frame platform visibility callback. Native 4:3
     * keeps its strict +/-1300 window; widescreen admits the extra authored
     * platforms but still hides one beyond the widened boundary. */
    M643PlatformWork visibilityWork = {0};
    OMOBJ visibilityObject = {0};
    visibilityWork.teamIndex = 0;
    visibilityObject.data = &visibilityWork;
    lbl_1_bss_8 = 0;
    GwMgNightF = 0;
    for (int index = 0; index < 39; ++index) {
        lbl_1_data_C4[0][index] = (HuVecF){-1900.0f + index * 100.0f, 0.0f, 0.0f};
        if (index == 38) lbl_1_data_C4[0][index].x = 2400.0f;
        visibilityWork.loweredModels[index] = (HU3D_MODELID)(100 + index);
        visibilityWork.raisedModels[index] = (HU3D_MODELID)(200 + index);
        lbl_1_bss_C[0][index] = 1;
    }
    for (int pass = 0; pass < 2; ++pass) {
        scale = pass == 0 ? 1.0f : 1.75f;
        memset(modelAttrs, 0, sizeof(modelAttrs));
        fn_1_2A98(&visibilityObject);
        for (int index = 0; index < 39; ++index) {
            int visible = fabsf(lbl_1_data_C4[0][index].x) < 1300.0f * scale;
            assert(((modelAttrs[100 + index] & HU3D_ATTR_DISPOFF) != 0) == !visible);
            assert(((modelAttrs[200 + index] & HU3D_ATTR_DISPOFF) != 0) == !visible);
        }
        if (pass == 0) {
            assert((modelAttrs[100] & HU3D_ATTR_DISPOFF) != 0);
            assert((modelAttrs[200] & HU3D_ATTR_DISPOFF) != 0);
            assert((modelAttrs[107] & HU3D_ATTR_DISPOFF) == 0);
            assert((modelAttrs[131] & HU3D_ATTR_DISPOFF) == 0);
        } else {
            assert((modelAttrs[137] & HU3D_ATTR_DISPOFF) == 0);
            assert((modelAttrs[238] & HU3D_ATTR_DISPOFF) != 0);
        }
    }
    return 0;
}
