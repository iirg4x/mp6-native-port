#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "game/hu3d.h"

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
s16 Hu3DCameraNo;
HSF_SCENE FogData;
static float scale = 1;
static float positions[64][3];
static u8 alphas[64];
static int count, fogCalls;
static float mp6_widescreen_scale_factor(void) { return scale; }
static void position(float x, float y, float z)
{
    assert(count < 64);
    positions[count][0] = x; positions[count][1] = y; positions[count][2] = z;
}
static void color(int r, int g, int b, u8 a)
{
    assert(r == 200 && g == 170 && b == 120);
    alphas[count++] = a;
}
#define MTXIdentity(...) ((void)0)
#define GXLoadPosMtxImm(...) ((void)0)
#define GXSetCurrentMtx(...) ((void)0)
#define GXSetFog(...) (++fogCalls)
#define GXSetNumChans(...) ((void)0)
#define GXSetChanCtrl(...) ((void)0)
#define GXSetNumTexGens(...) ((void)0)
#define GXSetNumTevStages(...) ((void)0)
#define GXSetTevOrder(...) ((void)0)
#define GXSetTevOp(...) ((void)0)
#define GXSetCullMode(...) ((void)0)
#define GXSetZMode(test, compare, write) assert((test) && !(write))
#define GXSetZCompLoc(...) ((void)0)
#define GXSetColorUpdate(...) ((void)0)
#define GXSetAlphaUpdate(...) ((void)0)
#define GXSetAlphaCompare(...) ((void)0)
#define GXSetBlendMode(...) ((void)0)
#define GXClearVtxDesc(...) ((void)0)
#define GXSetVtxDesc(...) ((void)0)
#define GXSetVtxAttrFmt(...) ((void)0)
#define GXBegin(type, format, vertices) assert((type) == GX_QUADS && (vertices) == 4)
#define GXPosition3f32(x, y, z) position(x, y, z)
#define GXColor4u8(r, g, b, a) color(r, g, b, a)
#define GXEnd(...) ((void)0)

#include "s01_haze_subject.inc"

static void draw(float k)
{
    count = fogCalls = 0;
    scale = k;
    Hu3DCamera[0].fov = 30;
    Hu3DCamera[0].aspect = 1.2f * k;
    HU3D_CAMERA native = Hu3DCamera[0];
    mp6_s01_side_haze(NULL, NULL);
    assert(memcmp(&native, &Hu3DCamera[0], sizeof(native)) == 0);
    if (k <= 1) { assert(count == 0 && fogCalls == 0); return; }
    assert(count == 64 && fogCalls == 2);
    float inner = 7000 * tanf(30 * 0.00872664626f) * 1.2f;
    for (int i = 0; i < count; ++i) {
        assert(fabsf(positions[i][0]) >= inner - 0.01f);
        assert(fabsf(positions[i][0]) <= inner * k + 0.01f);
        assert(positions[i][2] == -7000);
        assert(alphas[i] <= 88);
    }
    for (int side = 0; side < 2; ++side) {
        int start = side * 32;
        assert(alphas[start] == 0 && alphas[start + 29] == 88);
        for (int band = 0; band < 8; ++band) {
            int i = start + band * 4;
            assert(alphas[i] <= alphas[i + 1]);
            assert(alphas[i] == alphas[i + 3]);
            assert(alphas[i + 1] == alphas[i + 2]);
            if (band < 7) {
                assert(alphas[i + 1] == alphas[i + 4]);
                assert(positions[i + 1][0] == positions[i + 4][0]);
            }
        }
    }
}

int main(void)
{
    draw(1); draw(4.0f / 3); draw(1.75f);
    float saved[64][3]; memcpy(saved, positions, sizeof(saved));
    draw(4); draw(1); draw(1.75f);
    assert(memcmp(saved, positions, sizeof(saved)) == 0);
    puts("S01 gradual side-only haze: PASS");
    return 0;
}
