#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define HU_FB_WIDTH 640
#define HU_FB_HEIGHT 480
typedef struct { int scissorX, scissorY, scissorW, scissorH; } HUSPRITE;
static int enabled, width=640, rect[4];
static int mp6_widescreen_enabled(void) { return enabled; }
static int mp6_widescreen_render_width(void) { return width; }
static void GXSetScissor(int x, int y, int w, int h) {
    rect[0]=x; rect[1]=y; rect[2]=w; rect[3]=h;
}
#include "board_subject.inc"
static void check(HUSPRITE s,int x,int y,int w,int h) {
    mp6SprScissorSet(&s);
    if (rect[0]!=x || rect[1]!=y || rect[2]!=w || rect[3]!=h) {
        puts("FAIL sprite scissor");
        exit(1);
    }
}
int main(void) {
    HUSPRITE full={0,0,640,480}, table={138,90,425,300};
    check(full,0,0,640,480); check(table,138,90,425,300);
    enabled=1; width=854;
    check(full,0,0,854,480); check(table,245,90,425,300);
    width=853;
    check(table,244,90,426,300);
    check((HUSPRITE){138,90,0,300},0,0,0,0);
    check((HUSPRITE){138,90,425,0},0,0,0,0);
    width=1440; check(full,0,0,1440,480); check(table,538,90,425,300);
    width=320; check(full,0,0,320,480); check(table,0,90,320,300);
    check((HUSPRITE){0,0,40,40},0,0,0,40);
    check((HUSPRITE){600,0,40,40},320,0,0,40);
    check((HUSPRITE){138,-10,425,600},0,0,320,480);
    enabled=0; check(table,138,90,425,300);
    puts("PASS sprite scissor");
    return 0;
}
