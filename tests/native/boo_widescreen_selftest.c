#define assert(c) do { if (!(c)) { printf("FAIL: %s\n",#c); return 1; } } while (0)
#include <math.h>
#include <stdio.h>
typedef float Mtx[3][4];
typedef struct {int copyF,screenWidth,screenHeight,textureWidth,textureHeight,activeF; void *textureData;} Work;
typedef struct {float fov,aspect;} Camera;
static Camera Hu3DCamera[2]; static int Hu3DCameraNo;
static int width,srcW,dstW,dstH,copies; static float aspectUsed,fovUsed;
static const float lbl_802C4288=0,lbl_802C42E0=30,lbl_802C4368=1.2f;
static const float lbl_802C42D0=.5f,lbl_802C436C=-.5f;
#define GX_FALSE 0
#define GX_TF_RGB565 4
#define TRUE 1
typedef unsigned short u16;
static int mp6_widescreen_render_width(void){return width;}
#define GXDrawDone() ((void)0)
#define GXSetTexCopySrc(x,y,w,h) (srcW=(w))
#define GXSetTexCopyDst(w,h,f,m) (dstW=(w),dstH=(h))
#define GXCopyTex(p,c) (++copies)
#define GXPixModeSync() ((void)0)
#define C_MTXLightPerspective(m,f,a,s,t,u,v) (fovUsed=(f),aspectUsed=(a))
#include "board_subject.inc"
int main(void){
    Work work={0,640,480,320,240,1,0};
    const int widths[]={640,864,1152,640};
    for(int i=0;i<4;i++){
        width=widths[i]; work.copyF=0;
        subject_capture(&work); subject_capture(&work);
        assert(srcW==width && dstW==320 && dstH==240 && copies==i+1);
        Hu3DCameraNo=i%2; Hu3DCamera[Hu3DCameraNo]=(Camera){37,1.2f*width/640.f};
        subject_project();
        assert(fovUsed==37 && fabsf(aspectUsed-1.2f*width/640.f)<.0001f);
    }
    Hu3DCamera[Hu3DCameraNo]=(Camera){0,0}; subject_project();
    assert(fovUsed==30 && aspectUsed==1.2f);
    puts("Boo live capture and camera projection: PASS");
    return 0;
}
