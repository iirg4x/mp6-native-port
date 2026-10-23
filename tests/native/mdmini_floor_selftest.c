#include <assert.h>
#include <string.h>
#include "game/hu3d.h"
#include "board_subject.inc"

static void check_floor(char *assetName)
{
    u8 pixels[256 * 256 * 2];
    HuVec2f uv[64];
    HSF_BUFFER st = {0};
    HSF_BITMAP bitmap = {0};
    HSF_ATTRIBUTE attribute = {0};
    HSF_OBJECT object = {0};
    HSF_DATA hsf = {0};
    bitmap.name = assetName;
    bitmap.data = pixels;
    bitmap.dataFmt = GX_TF_RGB565;
    bitmap.sizeX = bitmap.sizeY = 256;
    attribute.bitmap = &bitmap;
    attribute.maxLod = 1; /* Native asset value; ignored without HSF_FLAG_MIPMAP. */
    st.count = 64;
    st.data = uv;
    object.type = HSF_OBJ_MESH;
    object.name = "yuka";
    object.mesh.attribute = &attribute;
    object.mesh.st = &st;
    hsf.objectNum = 1;
    hsf.object = &object;
    for (int i=0;i<64;++i) { uv[i].x = i/63.0f; uv[i].y = i/63.0f; }

    /* A different asset, a mipmapped bitmap or an unexpected layout must
     * not be modified using this texture's measured crop. */
    bitmap.name = "ys77_floor";
    assert(!mp6_ws_minigame_floor_tile(&hsf, "yuka"));
    bitmap.name = assetName;
    bitmap.maxLod = 1;
    assert(!mp6_ws_minigame_floor_tile(&hsf, "yuka"));
    bitmap.maxLod = 0;
    attribute.flag = HSF_FLAG_MIPMAP;
    assert(!mp6_ws_minigame_floor_tile(&hsf, "yuka"));
    attribute.flag = 0;
    st.count = 63;
    assert(!mp6_ws_minigame_floor_tile(&hsf, "yuka"));
    st.count = 64;
    assert(!mp6_ws_minigame_floor_tile(&hsf, "not-yuka"));
    assert(bitmap.data == pixels && bitmap.sizeY == 256);
    assert(attribute.wrapS == 0 && attribute.wrapT == 0);
    assert(uv[63].x == 1.0f && uv[63].y == 1.0f);

    assert(mp6_ws_minigame_floor_tile(&hsf, "yuka"));
    assert(bitmap.data == pixels + 256*192*2);
    assert(bitmap.sizeX == 256 && bitmap.sizeY == 64);
    assert((u8 *)bitmap.data + 256*64*2 == pixels + sizeof(pixels));
    assert(attribute.wrapS == GX_REPEAT && attribute.wrapT == GX_REPEAT);
    for (int i=0;i<64;++i) {
        assert(uv[i].x == i/63.0f);
        assert(uv[i].y == 4.0f*(i/63.0f));
    }
    /* No double crop if a caller tries registration twice. */
    assert(!mp6_ws_minigame_floor_tile(&hsf, "yuka"));
}

int main(void)
{
    check_floor("ys80_floor");
    check_floor("ys78_floor");
    return 0;
}
