/* The board preserves native framing inputs; shared Hu3D mapping is executed
 * separately by widescreen_camera_selftest.c, including CAM2 and resize. */
#include "game/board/camera.h"
#include "game/hu3d.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
static int width, wide;
static float aspect, viewport;
static unsigned scissor, camera_bits;
static int mp6_widescreen_enabled(void) { return wide; }
static int mp6_widescreen_render_width(void) { return width; }
static float mp6_widescreen_scale_factor(void) { return (float)width / 640.0f; }
#define Hu3DCameraPerspectiveSet(bits,f,n,z,a) (camera_bits=(bits),aspect=(a))
#define Hu3DCameraViewportSet(bits,x,y,w,h,n,f) (viewport=(w))
#define Hu3DCameraScissorSet(bits,x,y,w,h) (scissor=(w))
#include "board_subject.inc"

#define CHECK(c) do {if (!(c)) {printf("FAIL line %d: %s\n",__LINE__,#c);return 1;}} while(0)
int main(void) {
    MBCAMERA camera={0}, original;
    camera.bit=HU3D_CAM0|HU3D_CAM1;
    camera.aspect=1.2f;
    camera.fov=25;
    camera.near=10;
    camera.far=20000;
    camera.viewportW=640;
    camera.viewportH=480;
    original=camera;
    Hu3DCamera[2].viewportH=480;
    Hu3DCamera[2].scissorH=480;
    const int sizes[]={640,854,1120,1600,640};
    for (unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        width=sizes[i];wide=1;
        for (int repeat=0;repeat<4;repeat++) subject(&camera);
        CHECK(camera_bits==(HU3D_CAM0|HU3D_CAM1));
        CHECK(fabsf(aspect-1.2f)<0.000001f);
        CHECK(viewport==640 && scissor==0);
        CHECK(Hu3DCamera[2].viewportH==480 && Hu3DCamera[2].scissorH==480);
        CHECK(memcmp(&camera,&original,sizeof(camera))==0);
    }
    wide=0;width=640;
    subject(&camera);
    CHECK(viewport==640 && aspect==1.2f);
    CHECK(memcmp(&camera,&original,sizeof(camera))==0);
    puts("SNPC board camera: native auto-fit inputs, no per-tick scissor overwrite: PASS");
    return 0;
}
