#include <assert.h>
#include <math.h>
#include <string.h>
#include "game/hu3d.h"
#include "game/disp.h"
#include "game/init.h"
#include "mp6_widescreen.h"

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
u16 Hu3DCameraExistF;
GXRenderModeObj mode;
GXRenderModeObj *RenderMode = &mode;
static float scale = 1.0f;
float mp6_widescreen_scale_factor(void) { return scale; }
#include "camera_subject.inc"

static void close_to(float actual, float expected) { assert(fabsf(actual - expected) < 0.001f); }

int main(void)
{
    memset(&mode, 0, sizeof(mode));
    mode.fbWidth = 640; mode.efbHeight = mode.xfbHeight = 480;
    /* A shared camera alternates fixed texture and screen passes every frame.
     * The target owns aspect, viewport AND scissor; no global opt-out leaks. */
    Hu3DCameraCreate(3);
    Hu3DCameraPerspectiveSet(3,45,20,8000,1.2f);
    Hu3DCameraViewportSet(3,0,0,640,480,0,1);
    Hu3DCameraScissorSet(3,0,0,640,480);
    for (int tick=0;tick<100;++tick) {
        scale=(float)(int[]){640,848,1120}[tick%3]/640;
        mp6_widescreen_camera_native_target(1,1);
        Hu3DCameraPerspectiveSet(1,45,20,8000,1.2f);
        Hu3DCameraViewportSet(1,0,0,640,480,0,1);
        Hu3DCameraScissorSet(1,0,0,640,480);
        mp6_widescreen_camera_reapply();
        close_to(Hu3DCamera[0].aspect,1.2f);
        close_to(Hu3DCamera[0].viewportW,640);
        assert(Hu3DCamera[0].scissorW==640);
        close_to(Hu3DCamera[1].aspect,1.2f*scale);
        mp6_widescreen_camera_native_target(1,0);
        Hu3DCameraPerspectiveSet(1,30,20,15000,1.2f);
        close_to(Hu3DCamera[0].aspect,1.2f*scale);
        close_to(Hu3DCamera[0].viewportW,640*scale);
        assert(Hu3DCamera[0].scissorW==(unsigned)(640*scale+.5f));
    }
    mp6_widescreen_camera_native_target(1,1);
    Hu3DCameraKill(1); Hu3DCameraCreate(1);
    scale=1.75f; mp6_widescreen_camera_reapply();
    close_to(Hu3DCamera[0].viewportW,1120);
    /* Bus cutouts follow world-projected positions on a centered native
     * composition. They are not split-screen panels and must not widen. */
    Hu3DCameraCreate(0x7f);
    mp6_widescreen_camera_centered_scissor(0x3e, 1);
    for (int tick=0; tick<100; ++tick) {
        scale=(float)(int[]){640,848,1120}[tick%3]/640;
        for (int i=1; i<=5; ++i) {
            unsigned x=40+90*(i-1), w=i==5 ? 90 : 64;
            Hu3DCameraScissorSet(1<<i,x,96,w,64);
            mp6_widescreen_camera_reapply();
            assert(Hu3DCamera[i].scissorX==x+(unsigned)(320*(scale-1)+.5f));
            assert(Hu3DCamera[i].scissorW==w);
            assert(Hu3DCamera[i].scissorY==96 && Hu3DCamera[i].scissorH==64);
            close_to(Hu3DCamera[i].viewportW,640*scale);
            close_to(Hu3DCamera[i].aspect,HU_DISP_ASPECT*scale);
        }
        Hu3DCameraScissorSet(1,40,96,64,64);
        assert(Hu3DCamera[0].scissorX==(unsigned)(40*scale+.5f));
        assert(Hu3DCamera[0].scissorW==(unsigned)(104*scale+.5f)-Hu3DCamera[0].scissorX);
        mp6_widescreen_camera_native_target(2,1);
        assert(Hu3DCamera[1].scissorX==40 && Hu3DCamera[1].scissorW==64);
        mp6_widescreen_camera_native_target(2,0);
        assert(Hu3DCamera[1].scissorX==40+(unsigned)(320*(scale-1)+.5f));
    }
    mp6_widescreen_camera_centered_scissor(2,0);
    assert(Hu3DCamera[1].scissorX==(unsigned)(40*scale+.5f));
    Hu3DCameraKill(4); Hu3DCameraCreate(4);
    Hu3DCameraScissorSet(4,40,96,64,64);
    assert(Hu3DCamera[2].scissorX==(unsigned)(40*scale+.5f));
    Hu3DCameraAllKill();
    /* Travelling cutouts are clipped AFTER centering, not at native X=0/640. */
    Hu3DCameraCreate(32);
    scale=1.75f;
    mp6_widescreen_camera_cutout(32,-100,96,90,128);
    assert(Hu3DCamera[5].scissorX==140 && Hu3DCamera[5].scissorW==90);
    scale=1; mp6_widescreen_camera_reapply();
    assert(Hu3DCamera[5].scissorX==0 && Hu3DCamera[5].scissorW==0);
    scale=1.75f; mp6_widescreen_camera_reapply();
    assert(Hu3DCamera[5].scissorX==140 && Hu3DCamera[5].scissorW==90);
    mp6_widescreen_camera_cutout(32,-260,-30,90,128);
    assert(Hu3DCamera[5].scissorX==0 && Hu3DCamera[5].scissorW==70);
    assert(Hu3DCamera[5].scissorY==0 && Hu3DCamera[5].scissorH==98);
    mp6_widescreen_camera_cutout(32,850,470,90,128);
    assert(Hu3DCamera[5].scissorX==1090 && Hu3DCamera[5].scissorW==30);
    assert(Hu3DCamera[5].scissorY==470 && Hu3DCamera[5].scissorH==10);
    mp6_widescreen_camera_cutout(32,NAN,INFINITY,90,128);
    assert(Hu3DCamera[5].scissorW==0 && Hu3DCamera[5].scissorH==0);
    Hu3DCameraAllKill();
    /* M602's three 192x288 views populate a fixed 576x288 atlas. The fourth
     * camera displays that texture on the wide stage and must remain wide. */
    Hu3DCameraCreate(15);
    mp6_widescreen_camera_native_target(7,1);
    for (int i=0;i<3;++i) {
        Hu3DCameraPerspectiveSet(1<<i,20,10,5000,2.0f/3);
        Hu3DCameraViewportSet(1<<i,192*i,0,192,288,0,1);
        Hu3DCameraScissorSet(1<<i,192*i,0,192,288);
    }
    for (int tick=0;tick<100;++tick) {
        scale=(float)(int[]){640,848,1120}[tick%3]/640;
        mp6_widescreen_camera_reapply();
        for (int i=0;i<3;++i) {
            close_to(Hu3DCamera[i].aspect,2.0f/3);
            close_to(Hu3DCamera[i].viewportX,192*i);
            close_to(Hu3DCamera[i].viewportW,192);
            assert(Hu3DCamera[i].scissorX==192*i && Hu3DCamera[i].scissorW==192);
        }
        close_to(Hu3DCamera[3].viewportW,640*scale);
    }
    /* Every bit, including masks used by four-player minigames. */
    for (int ratio = 0; ratio < 3; ++ratio) {
        scale = (float)(int[]){640, 848, 1120}[ratio] / 640;
        mode.fbWidth = (u16)(640 * scale);
        Hu3DCameraCreate(0xffff);
        for (int i=0; i<HU3D_CAM_MAX; ++i) {
            close_to(Hu3DCamera[i].viewportW, 640 * scale);
            close_to(Hu3DCamera[i].aspect, HU_DISP_ASPECT * scale);
        }
        Hu3DCameraPerspectiveSet(15, 40, 10, 15000, 1.1428572f);
        for (int i=0; i<4; ++i) {
            Hu3DCameraViewportSet(1<<i, (i%2)*320, (i/2)*240, 320, 240, 0.1f, 0.9f);
            Hu3DCameraScissorSet(1<<i, (i%2)*320, (i/2)*240, 320, 240);
        }
        assert(Hu3DCamera[0].scissorX+Hu3DCamera[0].scissorW==Hu3DCamera[1].scissorX);
        assert(Hu3DCamera[1].scissorX+Hu3DCamera[1].scissorW==mode.fbWidth);
        close_to(Hu3DCamera[1].viewportX, 320*scale);
        close_to(Hu3DCamera[3].viewportY, 240);
        close_to(Hu3DCamera[3].viewportNear, 0.1f);
        Hu3DCameraScissorSet(2, 640, 0, 0, 0);
        mp6_widescreen_camera_reapply();
        assert(Hu3DCamera[1].scissorW == 0 && Hu3DCamera[1].scissorH == 0);
        /* M618 winner transition: losing rectangles reach -4 after the
         * inset subtraction. Test all four winner quadrants at every aspect. */
        for (int winner=0; winner<4; ++winner) {
            for (int step=0; step<=60; ++step) {
                unsigned int sx=(winner%2 == 0) ? 320+(320*step)/60 : 320-(320*step)/60;
                unsigned int sy=(winner/2 == 0) ? 240+(240*step)/60 : 240-(240*step)/60;
                for (int i=0; i<4; ++i) {
                    unsigned int x=(i%2) ? sx+2 : 2, y=(i/2) ? sy+2 : 2;
                    unsigned int w=((i%2) ? 640-sx : sx)-4;
                    unsigned int h=((i/2) ? 480-sy : sy)-4;
                    Hu3DCameraScissorSet(1<<i,x,y,w,h);
                    assert(Hu3DCamera[i].scissorX+Hu3DCamera[i].scissorW<=mode.fbWidth);
                    assert(Hu3DCamera[i].scissorY+Hu3DCamera[i].scissorH<=480);
                    if(step==60 && i!=winner) assert(!Hu3DCamera[i].scissorW || !Hu3DCamera[i].scissorH);
                    if(step==60 && i==winner) assert(Hu3DCamera[i].scissorW>0 && Hu3DCamera[i].scissorH==476);
                }
                mp6_widescreen_camera_reapply();
            }
        }
        Hu3DCameraScissorSet(1,630,470,100,100);
        assert(Hu3DCamera[0].scissorX+Hu3DCamera[0].scissorW==mode.fbWidth);
        assert(Hu3DCamera[0].scissorH==10);
        Hu3DCameraKill(-2);
        mp6_widescreen_camera_reapply();
        for (int i=1;i<HU3D_CAM_MAX;++i) assert(Hu3DCamera[i].fov == -1);
        /* Reuse, live camera animation, and native identity on disabling. */
        Hu3DCameraCreate(2);
        mp6_widescreen_camera_widen(2, 30, 20, 10000, 1.2f, 480);
        Hu3DCameraPerspectiveSet(2, 55, 7, 12000, 1.1f);
        Hu3DCamera[1].fov = 63;
        Hu3DCamera[1].pos.x = 123;
        scale = 1;
        for (int j=0;j<100;++j) mp6_widescreen_camera_reapply();
        close_to(Hu3DCamera[1].fov, 63);
        close_to(Hu3DCamera[1].near, 7);
        close_to(Hu3DCamera[1].far, 12000);
        close_to(Hu3DCamera[1].pos.x, 123);
        close_to(Hu3DCamera[1].aspect, 1.1f);
        close_to(Hu3DCamera[1].viewportW, 640);
        assert(Hu3DCamera[1].scissorW == 640);
        Hu3DCameraAllKill();
        assert(Hu3DCameraExistF == 0);
    }
    /* The native field-rendering height conversion happens exactly once. */
    mode.xfbHeight = 240;
    Hu3DCameraCreate(1);
    Hu3DCameraViewportSet(1, -160, 20, 640, 480, 0, 1);
    scale = 1.75f;
    for (int i=0;i<100;++i) mp6_widescreen_camera_reapply();
    close_to(Hu3DCamera[0].viewportX, -280);
    close_to(Hu3DCamera[0].viewportH, 240);
    return 0;
}
