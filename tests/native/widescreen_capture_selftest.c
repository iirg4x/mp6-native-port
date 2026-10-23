#include <assert.h>
#include "game/hu3d.h"
#include "game/disp.h"

HU3D_WATER Hu3DWaterData;
s16 Hu3DCameraBit;
static int width, mipmap, copies;
static char water[640*480*2], fileSelect[640*480*2], modeSelect[640*480*4];
static GXTexFmt expectedFormat;
void *lbl_1_bss_C = fileSelect;
static int mp6_widescreen_render_width(void) { return width; }
void GXSetTexCopySrc(u16 x,u16 y,u16 w,u16 h) { assert(!x && !y && w==width && h==480); }
void GXSetTexCopyDst(u16 w,u16 h,GXTexFmt fmt,GXBool mip) {
    assert(w==(mipmap?320:640) && h==(mipmap?240:480));
    assert(fmt==expectedFormat && mip==mipmap);
}
void GXCopyTex(void *ptr,GXBool clear) {
    assert((ptr==water || ptr==fileSelect || ptr==modeSelect) && !clear);
    ++copies;
}
#include "capture_subject.inc"

int main(void)
{
    Hu3DCameraBit=1; Hu3DWaterData.cameraBit=1; Hu3DWaterData.fbWater=water;
    for (int i=0;i<5;++i) {
        width=(int[]){640,848,1120,2560,640}[i];
        expectedFormat=GX_TF_RGB565;
        mipmap=0; fn_1_7DEC();
        for (mipmap=0;mipmap<2;++mipmap) {
            Hu3DWaterData.mipMapF=mipmap;
            CopyWaterFb(0);
        }
        mipmap=0; expectedFormat=GX_TF_RGBA8;
        mp6_mdsel_capture(modeSelect);
    }
    assert(copies==20);
    Hu3DCameraBit=2; CopyWaterFb(0);
    lbl_1_bss_C=0; fn_1_7DEC();
    assert(copies==20);
    return 0;
}
