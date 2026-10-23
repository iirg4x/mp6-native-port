#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "REL/m664dll/m664dll.h"
#include "game/hsfex.h"
#include "game/hu3d.h"
#include "game/mg/actman.h"

/* Sound IDs referenced by the extracted fn_1_9704 body.  The production
 * source gets these from the m664 scene header; keep the fixture independent
 * of unrelated scene data while retaining the call/branch behavior. */
#define MSM_SE_M664_NUT_DROP 2138
#define MSM_SE_M664_NUT_IMPACT 2130
#define MSM_SE_M664_PROP_MISS 2140
#define MSM_SE_M664_HOOK_IMPACT 2139

s32 lbl_1_bss_8C0;
s16 lbl_1_bss_844[5];
s16 lbl_1_bss_7CA;
f32 lbl_1_bss_794;
s32 lbl_1_bss_744[5][4];
f32 lbl_1_bss_6A4[5][4];
s16 lbl_1_bss_81C[5][4];
s32 lbl_1_bss_6F4[5][4];
s16 lbl_1_bss_7CC[5][4][2];
char lbl_1_bss_798[5][10];
s16 lbl_1_bss_84E;
Point3d lbl_1_bss_40C;

static int polygonCalls;
static int impactSoundCalls;

void Hu3DMotionSpeedSet(HU3D_MODELID modelId, f32 speed)
{
    (void)modelId;
    (void)speed;
}

void Hu3DModelAttrSet(HU3D_MODELID modelId, u32 attr)
{
    (void)modelId;
    (void)attr;
}

void Hu3DModelAttrReset(HU3D_MODELID modelId, u32 attr)
{
    (void)modelId;
    (void)attr;
}

void Hu3DModelShadowReset(HU3D_MODELID modelId)
{
    (void)modelId;
}

f32 Hu3DMotionTimeGet(HU3D_MODELID modelId)
{
    (void)modelId;
    return 0.0f;
}

s32 Hu3DMotionEndCheck(HU3D_MODELID modelId)
{
    (void)modelId;
    return 0;
}

void Hu3DModelObjPosGet(HU3D_MODELID modelId, char *objName, HuVecF *position)
{
    (void)modelId;
    assert(objName != NULL);
    *position = (HuVecF){0.0f, 100.0f, 0.0f};
}

void Hu3DModelObjMtxGet(HU3D_MODELID modelId, char *objName, Mtx matrix)
{
    (void)modelId;
    assert(objName != NULL);
    memset(matrix, 0, sizeof(Mtx));
}

void Hu3DModelPosSetV(HU3D_MODELID modelId, HuVecF *position)
{
    (void)modelId;
    (void)position;
}

void Hu3DModelRotSetV(HU3D_MODELID modelId, HuVecF *rotation)
{
    (void)modelId;
    (void)rotation;
}

void Hu3DMtxTransGet(Mtx matrix, HuVecF *position)
{
    (void)matrix;
    *position = (HuVecF){0.0f, 0.0f, 0.0f};
}

void Hu3DMtxRotGet(Mtx matrix, HuVecF *rotation)
{
    (void)matrix;
    *rotation = (HuVecF){0.0f, 0.0f, 0.0f};
}

s16 fn_1_B910(s16 soundEffect, s16 modelId, s16 cameraMask)
{
    (void)soundEffect;
    (void)modelId;
    assert(cameraMask == 1);
    ++impactSoundCalls;
    return 0;
}

s32 frandmod(s32 limit)
{
    assert(limit > 0);
    return 0;
}

BOOL MgActorColMapPolyGet(HuVecF *pos1, HuVecF *pos2, u32 mask,
                          MGACTOR_COLMAP_POLY *outPoly)
{
    assert(mask == ~0U);
    assert(pos1 != NULL && pos2 != NULL && outPoly != NULL);
    memset(outPoly, 0x5A, sizeof(*outPoly));
    outPoly->pos = *pos1;
    outPoly->code = 0xCAFEU;
    outPoly->mdlNo = 7;
    outPoly->obj = (HSF_OBJECT *)(uintptr_t)0x1234;
    outPoly->triNo = 11;
    ++polygonCalls;
    return TRUE;
}

#include "m664_subject.inc"

int main(void)
{
    for (int board = 0; board < 5; ++board) {
        lbl_1_bss_844[board] = (s16)(10 + board);
        for (int piece = 0; piece < 4; ++piece) {
            lbl_1_bss_81C[board][piece] = (s16)(30 + board * 4 + piece);
            lbl_1_bss_6F4[board][piece] = 0;
        }
    }

    /* This is the first frame that exercises every falling-nut collision query. */
    lbl_1_bss_8C0 = 2118;
    fn_1_9704();
    assert(polygonCalls == 20);
    assert(impactSoundCalls == 20);
    for (int board = 0; board < 5; ++board)
        for (int piece = 0; piece < 4; ++piece)
            assert(lbl_1_bss_6F4[board][piece] == 2);
    return 0;
}
