#include <stdio.h>
#include <string.h>
#include "game/hsfformat.h"
#include "be.h"

typedef struct { int objectNum; } LoadCtx;
static char *GetMotionStr(LoadCtx *ctx, u16 offset)
{
    (void)ctx;
    return offset == 0x1234 ? "camera" : "mesh";
}
static int FindObjIdxByName(LoadCtx *ctx, const char *name)
{
    (void)ctx;
    return strcmp(name, "camera") == 0 ? 3 : 9;
}
#include "board_subject.inc"
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    LoadCtx ctx = {0};
    HSF_TRACK out;
    /* Motion-only files retain a string-offset target; the camera consumer
     * identifies the track by its separate object-type index. */
    u8 track[16] = {HSF_TRACK_TRANSFORM, 0, 0x12, 0x34, 0, HSF_OBJ_CAMERA, 0, 28};
    memset(&out, 0, sizeof(out));
    subject_load(&ctx, track, &out);
    CHECK(out.target == 0x1234 && out.index == HSF_OBJ_CAMERA && out.channel == 28);
    ctx.objectNum = 4;
    subject_load(&ctx, track, &out);
    CHECK(out.target == 3 && out.index == HSF_OBJ_CAMERA && out.channel == 28);
    track[4] = 0; track[5] = HSF_OBJ_MESH;
    subject_load(&ctx, track, &out);
    CHECK(out.index == HSF_OBJ_MESH);
    track[0] = HSF_TRACK_MORPH;
    track[4] = 0x12; track[5] = 0x34;
    track[6] = 0xff; track[7] = 0xfe;
    subject_load(&ctx, track, &out);
    CHECK(out.index == 0x1234 && out.morphWeight == -2 && out.target == 3);
    puts("PASS: native motion tracks preserve index and channel unions");
    return 0;
}
