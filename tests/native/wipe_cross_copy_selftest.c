#include <stdio.h>
#include <stdlib.h>
#define HU_FB_WIDTH 640
#define HU_FB_HEIGHT 480
#define GX_TF_RGB565 4
#define GX_TRUE 1
#define GX_FALSE 0
static int width, copies, syncs;
static struct { void *image[1]; } wipeData;
static void check(int ok) { if (!ok) { puts("FAIL: cross-copy capture"); exit(1); } }
static int mp6_widescreen_render_width(void) { return width; }
static void GXSetTexCopySrc(int x,int y,int w,int h) {
    check(x==0 && y==0 && w==width && h==480);
}
static void GXSetTexCopyDst(int w,int h,int format,int mip) {
    check(w==320 && h==240 && format==GX_TF_RGB565 && mip==GX_TRUE);
}
static void GXCopyTex(void *image,int clear) {
    check(image==wipeData.image[0] && clear==GX_FALSE); ++copies;
}
static void GXPixModeSync(void) { ++syncs; }
#include "board_subject.inc"
int main(void) {
    int widths[]={640,848,1152,640};
    char backing[320*240*2];
    wipeData.image[0]=backing;
    for (unsigned i=0;i<sizeof(widths)/sizeof(widths[0]);++i) {
        width=widths[i]; subject_capture();
    }
    check(copies==4 && syncs==4);
    puts("PASS: live width, fixed allocation, repeated resize");
    return 0;
}
