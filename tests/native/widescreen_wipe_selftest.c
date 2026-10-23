#include <assert.h>
#include <math.h>
#include <string.h>
#include "game/wipe.h"
#include "game/hu3d.h"
#include "game/sprite.h"
#include "game/disp.h"

WIPEWORK wipeData;
BOOL wipeFadeInF;
static int omSysExitReq, animFrees, rawFrees;
static ANIMDATA *wipeLoadAnim;
static float wipeLoadTime;
static int width, copies, syncs, count, formats;
static float tx, ty, sx, sy, points[20][2];
static unsigned char image[320*240*2];
static int mp6_widescreen_render_width(void) { return width; }
static void WipeGXInit(void) { tx=ty=0; sx=sy=1; count=formats=0; }
static BOOL WipeNormalFade(void) { return FALSE; }
void GXSetTexCopySrc(u16 x,u16 y,u16 w,u16 h) { assert(!x && !y && w==width && h==480); }
void GXSetTexCopyDst(u16 w,u16 h,GXTexFmt fmt,GXBool mip) {
    assert(w==320 && h==240 && fmt==GX_TF_RGB565 && mip);
}
void GXCopyTex(void *ptr,GXBool clear) { assert(ptr==image && !clear); ++copies; }
void GXPixModeSync(void) { ++syncs; }
static void pos(float x,float y) {
    assert(count<20); points[count][0]=x*sx+tx; points[count++][1]=y*sy+ty;
}
static void fmt(GXAttr attr, GXCompType type) {
    if (attr==GX_VA_POS) { assert(type==GX_S16 || type==GX_F32); ++formats; }
}
#undef MTXTrans
#define MTXTrans(m,x,y,z) (tx=(x),ty=(y))
#define mtxScaleCat(m,x,y,z) (sx=(x),sy=(y))
#define GXPosition2s16(x,y) pos((s16)(x),(s16)(y))
#define GXPosition2f32(x,y) pos((x),(y))
#define GXSetVtxAttrFmt(v,a,c,t,f) fmt((a),(t))
#define GXLoadPosMtxImm(...) ((void)0)
#define GXClearVtxDesc(...) ((void)0)
#define GXSetVtxDesc(...) ((void)0)
#define GXSetNumTexGens(...) ((void)0)
#define GXSetNumTevStages(...) ((void)0)
#define GXSetTexCoordGen(...) ((void)0)
#define GXSetTevOrder(...) ((void)0)
#define GXSetTevColor(...) ((void)0)
#define GXSetTevColorIn(...) ((void)0)
#define GXSetTevColorOp(...) ((void)0)
#define GXSetTevAlphaIn(...) ((void)0)
#define GXSetTevAlphaOp(...) ((void)0)
#define GXBegin(...) ((void)0)
#define GXEnd(...) ((void)0)
#define GXTexCoord2f32(...) ((void)0)
#define HuSprTexLoad(...) ((void)0)
#define HuMemDirectFree(p) (++rawFrees)
#define HuSprAnimKill(p) (++animFrees)
#define HuSprAnimRead(...) ((void *)image)
#define WipeColorSet(...) ((void)0)
#define WipeLoadKill(...) ((void)0)
#define HuSin(x) sin((x)*3.141592653589793/180.0)
#define HuCos(x) cos((x)*3.141592653589793/180.0)
#include "wipe_subject.inc"

int main(void)
{
    float native[20][2];
    for (int type=0;type<WIPE_TYPE_MAX;type++) {
        int anim=(type==7 || type>=9);
        wipeData.type=type|WIPE_TYPE_FBKEEP;
        wipeData.image[0]=wipeData.image[1]=image;
        rawFrees=animFrees=0;
        WipeImageRelease(0); WipeImageRelease(0);
        assert(!wipeData.image[0] && animFrees==anim && rawFrees==!anim);
        WipeImageRelease(1); WipeImageRelease(1);
        assert(!wipeData.image[1] && animFrees==anim+1 && rawFrees==!anim);
    }
    /* WipeCreate must classify the outgoing image before changing type. */
    wipeData.mode=WIPE_MODE_END; wipeData.type=WIPE_TYPE_STAR;
    wipeData.image[0]=image; rawFrees=animFrees=0;
    WipeCreate(WIPE_MODE_IN,WIPE_TYPE_NORMAL,45);
    assert(animFrees==1 && !rawFrees && !wipeData.image[0]);
    wipeData.mode=WIPE_MODE_END; wipeData.type=WIPE_TYPE_CROSS;
    wipeData.image[0]=image;
    WipeCreate(WIPE_MODE_IN,WIPE_TYPE_CROSS_COPY,45);
    assert(wipeData.image[0]==image && !rawFrees && animFrees==1);
    wipeData.type=WIPE_TYPE_STAR;
    for (int mode=WIPE_MODE_IN;mode<=WIPE_MODE_OUT;mode++) {
        for (int tick=1;tick<60;tick+=7) {
            for (int aspect=0;aspect<4;aspect++) {
                width=(int[]){640,848,1120,2560}[aspect];
                WipeCaptureFrame(image);
                wipeData.mode=mode; wipeData.time=tick; wipeData.maxTime=60;
                wipeData.image[0]=image;
                assert(WipeImage(0));
                assert(count==20 && formats==2);
                if (!aspect) memcpy(native,points,sizeof(native));
                for (int v=0;v<4;v++) {
                    assert(fabsf(points[v][0]-native[v][0]-(width-640)/2.0f)<0.001f);
                    assert(points[v][1]==native[v][1]);
                }
                assert(points[4][0]<=0 && points[5][0]>=width);
                assert(points[8][0]<=0 && points[9][0]>=width);
                assert(points[12][0]<=0 && points[17][0]>=width);
            }
        }
    }
    assert(copies==72 && syncs==copies);
    wipeData.mode=WIPE_MODE_IN; wipeData.time=wipeData.maxTime=60;
    wipeData.image[0]=image; rawFrees=animFrees=0;
    assert(!WipeImage(0) && !wipeData.image[0] && animFrees==1 && !rawFrees);
    ANIMDATA anim={0}; ANIMBMP bmp={0};
    bmp.sizeX=80; bmp.sizeY=24; anim.bmp=&bmp; wipeLoadAnim=&anim;
    for (int aspect=0;aspect<3;aspect++) {
        width=(int[]){640,848,1120}[aspect]; wipeLoadTime=0;
        WipeLoadDraw(); assert(count==4 && formats==1);
        assert(points[0][0]==width-240 && points[1][0]==width-80);
        assert(points[0][1]==350 && points[2][1]==398);
    }
    return 0;
}
