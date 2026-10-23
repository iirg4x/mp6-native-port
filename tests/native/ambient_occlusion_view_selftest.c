#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "game/hu3d.h"
#include "game/init.h"
#include "mp6_ambient_occlusion.h"
#include "mp6_enhancements.h"

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
static GXRenderModeObj mode;
GXRenderModeObj *RenderMode = &mode;
static int calls;
static int decalCalls, decalCamera, decalAfter;
static int foliageCalls, foliageCamera, foliageEnabled;
static int tanfCalls;
static Mp6AoView captured;
/* Keep the production conversion observable: AO-off camera configuration must
 * not pay for projection conversion, while enabled configuration still does. */
float tanf(float radians) { ++tanfCalls; return (float)tan((double)radians); }
uint64_t mp6_host_monotonic_ns(void) { return 0; }
void mp6_ao_apply(const Mp6AoView *view) { ++calls; captured = *view; }
void mp6_ao_capture_decals(int camera, int after) { ++decalCalls; decalCamera = camera; decalAfter = after; }
void mp6_ao_foliage_render(int camera, int enabled) {
    ++foliageCalls; foliageCamera = camera; foliageEnabled = enabled;
}
#ifndef MP6_HEADLESS_BUILD
static int sceneHasDepth=1;
static unsigned char mockDepthEpoch[HU3D_CAM_MAX];
int mp6_ao_camera_has_depth(int camera) {
    return sceneHasDepth && mp6_enh_ambient_occlusion() && camera>=0 && camera<HU3D_CAM_MAX &&
           mockDepthEpoch[camera] != 0;
}
/* The view self-test links the C adapter in isolation, so provide the narrow
 * renderer-side epoch seam as a deterministic mock.  This preserves the
 * production contract being tested: begin opens one candidate epoch, end
 * consumes it exactly once, exclusion retires it, and clear starts a fresh
 * depth scope without touching other cameras. */
void mp6_ao_camera_begin(int camera) {
    if (camera < 0 || camera >= HU3D_CAM_MAX) return;
    mockDepthEpoch[camera] = 1;
    mp6_ao_camera_snapshot_reset(camera);
}
void mp6_ao_camera_consume(int camera) {
    if (camera >= 0 && camera < HU3D_CAM_MAX) mockDepthEpoch[camera] = 0;
}
void mp6_ao_camera_exclude(int camera) {
    if (camera < 0 || camera >= HU3D_CAM_MAX) return;
    mockDepthEpoch[camera] = 0;
    mp6_ao_camera_snapshot_reset(camera);
}
void mp6_ao_depth_cleared(int camera) {
    if (camera < 0 || camera >= HU3D_CAM_MAX) return;
    mockDepthEpoch[camera] = 1;
    mp6_ao_camera_snapshot_reset(camera);
}
#endif
static unsigned targetWidth=1920,targetHeight=1080;
static int targetActive=1,viewportCalls,scissorCalls;
static float viewport[6];
static u32 scissor[4];
int mp6_ao_foliage_target_size(unsigned *w,unsigned *h) {
    *w=targetWidth; *h=targetHeight; return targetActive;
}
void GXSetViewportRender(f32 x,f32 y,f32 w,f32 h,f32 n,f32 f) {
    ++viewportCalls;
    viewport[0]=x; viewport[1]=y; viewport[2]=w; viewport[3]=h; viewport[4]=n; viewport[5]=f;
}
void GXSetScissorRender(u32 x,u32 y,u32 w,u32 h) {
    ++scissorCalls; scissor[0]=x; scissor[1]=y; scissor[2]=w; scissor[3]=h;
}

int main(void) {
    Mp6EnhValues settings;
    mp6_enh_defaults(&settings);
    assert(settings.ambientOcclusion == 0);
    mode.fbWidth = 1280;
    mode.efbHeight = 480;
    HU3D_CAMERA *c = &Hu3DCamera[0];
    c->fov = 60; c->aspect = 8.0f/3.0f; c->near = 20; c->far = 10000;
    c->viewportX = 320; c->viewportY = 120; c->viewportW = 640; c->viewportH = 240;
    c->scissorX = 400; c->scissorY = 160; c->scissorW = 400; c->scissorH = 160;
    c->viewportNear = 0.1f; c->viewportFar = 0.9f;
    mp6_ao_foliage_camera(0);
#ifdef MP6_HEADLESS_BUILD
    assert(viewportCalls==0 && scissorCalls==0);
#else
    assert(viewportCalls==1 && scissorCalls==1);
    assert(viewport[0]==480 && viewport[1]==270 && viewport[2]==960 && viewport[3]==540);
    assert(viewport[4]==.1f && viewport[5]==.9f);
    assert(scissor[0]==600 && scissor[1]==360 && scissor[2]==600 && scissor[3]==360);
    /* A non-integral render scale must cover both edge pixels, as on the EFB. */
    c->scissorX=1; c->scissorY=1; c->scissorW=1; c->scissorH=1;
    mp6_ao_foliage_camera(0);
    assert(scissor[0]==1 && scissor[1]==2 && scissor[2]==2 && scissor[3]==3);
    c->scissorX=1200; c->scissorY=400; c->scissorW=400; c->scissorH=200;
    mp6_ao_foliage_camera(0);
    assert(scissor[0]==1800 && scissor[1]==900 && scissor[2]==120 && scissor[3]==180);
    targetWidth=1280; targetHeight=480;
    mp6_ao_foliage_camera(0);
    assert(viewport[0]==320 && viewport[1]==120 && viewport[2]==640 && viewport[3]==240);
    targetWidth=3840; targetHeight=2160;
    c->viewportX=c->viewportY=c->scissorX=c->scissorY=0;
    c->viewportW=c->scissorW=1280; c->viewportH=c->scissorH=480;
    mp6_ao_foliage_camera(0);
    assert(viewport[2]==3840 && viewport[3]==2160 && scissor[2]==3840 && scissor[3]==2160);
    c->viewportX=320; c->viewportY=120; c->viewportW=640; c->viewportH=240;
    int previous=viewportCalls;
    targetActive=0; mp6_ao_foliage_camera(0); targetActive=1;
    mp6_ao_foliage_camera(-1); mp6_ao_foliage_camera(HU3D_CAM_MAX);
    RenderMode=NULL; mp6_ao_foliage_camera(0); RenderMode=&mode;
    mode.fbWidth=0; mp6_ao_foliage_camera(0); mode.fbWidth=1280;
    assert(viewportCalls==previous);
    c->scissorX=400; c->scissorY=160; c->scissorW=400; c->scissorH=160;
#endif
    mp6_ao_camera_end(0);
    assert(calls == 0);
    mp6_ao_decals(0, 0); mp6_ao_decals(0, 1);
    assert(decalCalls == 0);
    mp6_ao_camera_begin(0);
    const int tanfBeforeOffConfigure = tanfCalls;
    mp6_ao_camera_configured(0);
    assert(tanfCalls == tanfBeforeOffConfigure);
    settings.ambientOcclusion = 1;
    mp6_enh_set_values(&settings);
#ifndef MP6_HEADLESS_BUILD
    /* A Party 3D pass snapshots its projection at camera entry.  Mutating
     * Hu3DCamera after the model draw must not make AO use the following
     * orthographic/UI camera. */
    mp6_ao_camera_begin(0);
    mp6_ao_camera_end(0);
    assert(calls == 0 && foliageEnabled == 0); /* no-view scope discards, never submits */
    const int foliageBeforeParty = foliageCalls;
    mp6_ao_camera_begin(0);
    const int tanfBeforeOnConfigure = tanfCalls;
    mp6_ao_camera_configured(0);
    assert(tanfCalls == tanfBeforeOnConfigure + 1);
    c->fov = 120; c->aspect = 0.75f; c->near = 2; c->far = 3;
    c->viewportX = 7; c->viewportY = 8; c->viewportW = 9; c->viewportH = 10;
    c->scissorX = 11; c->scissorY = 12; c->scissorW = 13; c->scissorH = 14;
    c->viewportNear = 0.2f; c->viewportFar = 0.3f;
    mp6_ao_camera_end(0);
    assert(calls == 1 && foliageCalls == foliageBeforeParty + 1 &&
           foliageCamera == 0 && foliageEnabled == 1);
    assert(fabsf(captured.projection[0] - 0.57735027f) < 1e-6f);
    assert(captured.projection[1] == 8.0f/3.0f && captured.projection[2] == 20 && captured.projection[3] == 10000);
    assert(captured.viewport[0] == .25f && captured.viewport[1] == .25f &&
           captured.viewport[2] == .5f && captured.viewport[3] == .5f);
    assert(captured.scissor[0] == 400.0f/1280 && captured.scissor[3] == 160.0f/480);
    assert(captured.depthRange[0] == .1f && captured.depthRange[1] == .9f);
    /* One depth epoch admits one composite.  A repeated end hook and the
     * following 2D feedback scope must not apply AO a second time. */
    mp6_ao_camera_end(0);
    assert(calls == 1);
    mp6_ao_camera_exclude(0);
    mp6_ao_camera_exclude(0); /* repeated UI boundary remains idempotent */
    mp6_ao_camera_end(0);
    assert(calls == 1);
    /* A clear flushes any still-pending 3D epoch before retiring its depth.
     * The post-clear UI/feedback segment must not replay that same AO. */
    mp6_ao_camera_begin(0);
    mp6_ao_camera_configured(0);
    mp6_ao_depth_about_to_clear(0);
    assert(calls == 2);
    mp6_ao_depth_cleared(0);
    mp6_ao_camera_end(0);
    assert(calls == 2);
    /* A clear retires the old epoch; newly drawn Party geometry is eligible
     * again and still uses the view captured at its own begin. */
    c->fov = 72; c->aspect = 1.5f; c->near = 10; c->far = 5000;
    c->viewportX = 20; c->viewportY = 30; c->viewportW = 800; c->viewportH = 400;
    c->scissorX = 25; c->scissorY = 35; c->scissorW = 700; c->scissorH = 300;
    c->viewportNear = 0.05f; c->viewportFar = 0.95f;
    mp6_ao_camera_begin(0);
    mp6_ao_camera_configured(0);
    c->fov = 135; c->aspect = 0.25f; c->near = 1; c->far = 2;
    mp6_ao_camera_end(0);
    assert(calls == 3 && captured.camera == 0);
    assert(fabsf(captured.projection[0] - tanf(72.0f * 0.00872664626f)) < 1e-6f);
    assert(captured.projection[1] == 1.5f && captured.projection[2] == 10 && captured.projection[3] == 5000);
    assert(captured.viewport[0] == 20.0f/1280 && captured.viewport[1] == 30.0f/480 &&
           captured.viewport[2] == 800.0f/1280 && captured.viewport[3] == 400.0f/480);
    /* Invalid views remain a no-work path, even after a valid epoch. */
    c->fov = -1; mp6_ao_camera_begin(0); mp6_ao_camera_configured(0); mp6_ao_camera_end(0);
    c->fov = 72; c->near = 0; mp6_ao_camera_begin(0); mp6_ao_camera_configured(0); mp6_ao_camera_end(0);
    c->near = 10; c->far = 2; mp6_ao_camera_begin(0); mp6_ao_camera_configured(0); mp6_ao_camera_end(0);
    c->far = 5000; c->viewportH = 0; mp6_ao_camera_begin(0); mp6_ao_camera_configured(0); mp6_ao_camera_end(0);
    assert(calls == 3);
    /* A second real camera follows the same rule, and a clear-only camera
     * cannot admit AO until geometry starts a fresh epoch. */
    c->fov = 72; c->aspect = 1.5f; c->near = 10; c->far = 5000;
    c->viewportX = 20; c->viewportY = 30; c->viewportW = 800; c->viewportH = 400;
    c->viewportH = 400; c->scissorX = 25; c->scissorY = 35; c->scissorW = 700; c->scissorH = 300;
    c->viewportNear = 0.05f; c->viewportFar = 0.95f;
    Hu3DCamera[3] = Hu3DCamera[0];
    mp6_ao_camera_begin(3); mp6_ao_camera_configured(3); mp6_ao_camera_end(3);
    assert(calls == 4 && captured.camera == 3);
    sceneHasDepth = 0;
    mp6_ao_camera_begin(3); mp6_ao_camera_configured(3); mp6_ao_camera_end(3);
    assert(calls == 4); /* clear/UI-only camera must not schedule AO */
    sceneHasDepth = 1;
    mp6_ao_depth_cleared(3);
    mp6_ao_camera_begin(3); mp6_ao_camera_configured(3); mp6_ao_camera_end(3);
    assert(calls == 5 && captured.camera == 3);
#else
    mp6_ao_camera_end(0);
#endif
    mp6_ao_decals(0, 0); mp6_ao_decals(0, 1);
    mp6_ao_decals(-1, 0); mp6_ao_decals(HU3D_CAM_MAX, 1);
#ifdef MP6_HEADLESS_BUILD
    assert(calls == 0);
    assert(decalCalls == 0);
#else
    assert(calls == 5);
    assert(captured.camera == 3);
    assert(decalCalls == 2 && decalCamera == 0 && decalAfter == 1);
    mp6_ao_camera_end(-1); mp6_ao_camera_end(HU3D_CAM_MAX);
#endif
    settings.ambientOcclusion = 42;
    mp6_enh_set_values(&settings);
    assert(mp6_enh_ambient_occlusion() == 0);
    int disabledCalls = calls;
#ifndef MP6_HEADLESS_BUILD
    const int tanfBeforeDisabledConfigure = tanfCalls;
    mp6_ao_camera_begin(0); mp6_ao_camera_configured(0); mp6_ao_camera_end(0);
    assert(tanfCalls == tanfBeforeDisabledConfigure);
#endif
    assert(calls == disabledCalls); /* disabled AO performs no scene work */
    for (int preset = 1; preset <= 3; ++preset) {
        mp6_enh_preset_values(preset, &settings);
        assert(settings.ambientOcclusion == 0);
    }
    puts("AO view mapping, validation, opt-in presets and headless isolation: PASS");
}
