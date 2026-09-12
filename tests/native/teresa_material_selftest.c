#include <stdio.h>
#include "game/hu3d.h"
#include "game/object.h"
static int base_setup;
static int mp6_widescreen_render_width(void) { return 864; }
#define GXDrawDone(...) ((void)0)
#define GXSetTexCopySrc(...) ((void)0)
#define GXSetTexCopyDst(...) ((void)0)
#define GXCopyTex(...) ((void)0)
#define GXPixModeSync(...) ((void)0)
#define Hu3DTevStageNoTexSet(d,m) (base_setup=0)
#define Hu3DTevStageTexSet(d,m) (base_setup=1)
#include "board_subject.inc"
#define CHECK(c) do {if (!(c)) {printf("FAIL %d: %s\n",__LINE__,#c);return 1;}} while(0)
int main(void) {
    TERESA_FADE_WORK work={0};
    HSF_MATERIAL material={0};
    HU3D_DRAW_OBJ draw={0};
    teresaFadeWork=&work;
    for (int copied=0;copied<2;copied++) {
        work.copyF=copied;
        for (int attrs=0;attrs<=3;attrs++) {
            base_setup=-1; material.attrNum=attrs;
            ev_CapTeresaFadeMatHook(&draw,&material);
            CHECK(base_setup==(attrs==0 ? 0 : attrs==1 ? -1 : 1));
        }
    }
    puts("Boo fade resets non-fading material state: PASS");
    return 0;
}
