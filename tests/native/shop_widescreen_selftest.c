#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "game/board/window.h"

#undef assert
#define assert(condition) do { if (!(condition)) { \
    printf("FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

#define SHOP_SELECT_WINDOW_SPACING 576.0f
static float aspect_scale;
static s16 actual_id, actual_x, actual_y;
float mp6_widescreen_scale_factor(void) { return aspect_scale; }
void mbWinPosSet(s16 id, s16 x, s16 y) {
    actual_id = id; actual_x = x; actual_y = y;
}
#include "board_subject.inc"

int main(void) {
    const float scales[] = {1.0f, 4.0f/3.0f, 1.75f, 8.0f/3.0f};
    for (int a=0; a<4; a++) {
        aspect_scale = scales[a];
        float edge = (576.0f * aspect_scale - 576.0f) * 0.5f;
        for (int selected=0; selected<3; selected++) {
            for (int slot=0; slot<3; slot++) {
                HuVecF base = {32.0f+576.0f*slot, 340.0f, 0.0f};
                ev_ShopDescPosSet(7, &base, -576.0f*selected, slot);
                assert(actual_id == 7 && actual_y == 340);
                if (slot==selected) assert(actual_x == 32);
                if (slot>selected) assert(actual_x >= 576.0f+edge);
                if (slot<selected) assert(actual_x+512 <= -edge);
            }
        }
        /* Preserve the smooth transition, including live aspect changes. */
        for (int frame=0; frame<=20; frame++) {
            float motion = -576.0f*sinf(frame/20.0f*1.57079632679f);
            for (int slot=0; slot<3; slot++) {
                HuVecF base = {32.0f+576.0f*slot, 340.0f, 0.0f};
                ev_ShopDescPosSet(3, &base, motion, slot);
                float expected = 32.0f+(576.0f*slot+motion)*aspect_scale;
                assert(fabsf(actual_x-expected) <= 1.01f);
                if (aspect_scale==1.0f) assert(actual_x==(s16)(base.x+motion));
            }
        }
    }
    puts("PASS shop widescreen carousel");
}
