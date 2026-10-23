#include <stdint.h>
#include <math.h>
#include "game/hu3d.h"
#include "game/init.h"
#include "mp6_ambient_occlusion.h"
#include "mp6_enhancements.h"

#ifndef MP6_HEADLESS_BUILD
/* Aurora's FIFO-retained physical-pixel commands. The retail GX scissor
 * encoding wraps at 1706 pixels (+342 bias in an 11-bit field). */
extern void GXSetViewportRender(f32,f32,f32,f32,f32,f32);
extern void GXSetScissorRender(u32,u32,u32,u32);
#endif

#ifdef MP6_HEADLESS_BUILD
/* Common decomp objects are shared by the graphical/headless link recipes. */
int mp6_ao_foliage_begin(int camera) { (void)camera; return 0; }
void mp6_ao_foliage_end(int camera) { (void)camera; }
void mp6_ao_camera_begin(int camera) { (void)camera; }
void mp6_ao_camera_configured(int camera) { (void)camera; }
void mp6_ao_camera_exclude(int camera) { (void)camera; }
void mp6_ao_depth_cleared(int camera) { (void)camera; }
int mp6_ao_camera_has_depth(int camera) { (void)camera; return 0; }
void mp6_ao_camera_snapshot_reset(int camera) { (void)camera; }
#endif

#ifndef MP6_HEADLESS_BUILD
/* A Hu3D camera is mutable game state.  Selector code can change it after a
 * world-space draw and before the camera-end hook, so AO must retain the
 * configuration that was actually installed by Hu3DCameraSet.  The full
 * camera copy is also used only to replay retained foliage with the same
 * projection; it is restored immediately after that private pass. */
static Mp6AoView mp6AoSavedView[HU3D_CAM_MAX];
static HU3D_CAMERA mp6AoSavedCamera[HU3D_CAM_MAX];
static unsigned char mp6AoSavedViewValid[HU3D_CAM_MAX];

static int mp6_ao_view_from_camera(int camera, const HU3D_CAMERA *c, Mp6AoView *view)
{
    float width, height;
    if (camera < 0 || camera >= HU3D_CAM_MAX || !c || !view || !RenderMode ||
        !RenderMode->fbWidth || !RenderMode->efbHeight ||
        c->fov <= 0 || c->fov >= 179 || c->near <= 0 || c->far <= c->near ||
        c->aspect <= 0 || c->viewportW <= 0 || c->viewportH <= 0 ||
        c->viewportFar <= c->viewportNear) return 0;
    width = RenderMode->fbWidth;
    height = RenderMode->efbHeight;
    view->projection[0] = tanf(c->fov * 0.00872664626f);
    view->projection[1] = c->aspect;
    view->projection[2] = c->near;
    view->projection[3] = c->far;
    view->viewport[0] = c->viewportX / width;
    view->viewport[1] = c->viewportY / height;
    view->viewport[2] = c->viewportW / width;
    view->viewport[3] = c->viewportH / height;
    view->scissor[0] = c->scissorX / width;
    view->scissor[1] = c->scissorY / height;
    view->scissor[2] = c->scissorW / width;
    view->scissor[3] = c->scissorH / height;
    view->depthRange[0] = c->viewportNear;
    view->depthRange[1] = c->viewportFar;
    view->camera = camera;
    return 1;
}

void mp6_ao_camera_snapshot_reset(int camera)
{
    if (camera < 0 || camera >= HU3D_CAM_MAX) return;
    mp6AoSavedViewValid[camera] = 0;
}

void mp6_ao_camera_configured(int camera)
{
    if (camera < 0 || camera >= HU3D_CAM_MAX) return;
    mp6AoSavedViewValid[camera] = 0;
    /* Keep the disabled path free of camera copies.  A later mid-frame enable
     * must remain fail-closed until the next explicit camera boundary. */
    if (!mp6_enh_ambient_occlusion()) return;
    mp6AoSavedCamera[camera] = Hu3DCamera[camera];
    mp6AoSavedViewValid[camera] =
        (unsigned char)mp6_ao_view_from_camera(camera, &Hu3DCamera[camera],
                                                &mp6AoSavedView[camera]);
}
#endif

void mp6_ao_foliage_camera(int camera)
{
#ifndef MP6_HEADLESS_BUILD
    unsigned width,height;
    if (camera<0 || camera>=HU3D_CAM_MAX || !RenderMode ||
        !RenderMode->fbWidth || !RenderMode->efbHeight ||
        !mp6_ao_foliage_target_size(&width,&height)) return;
    const HU3D_CAMERA *c=&Hu3DCamera[camera];
    const float sx=(float)width/RenderMode->fbWidth;
    const float sy=(float)height/RenderMode->efbHeight;
    /* Aurora scales logical GX coordinates on the EFB, but offscreen passes
     * already use target pixels. Reissuing the unscaled camera there shrinks
     * and displaces the foliage mask, and compares it with unrelated depth.
     * Match the EFB viewport and its outward-rounded/clamped scissor exactly. */
    GXSetViewportRender(c->viewportX*sx,c->viewportY*sy,c->viewportW*sx,c->viewportH*sy,
                  c->viewportNear,c->viewportFar);
    const float left=fminf(width,fmaxf(0,floorf(c->scissorX*sx)));
    const float top=fminf(height,fmaxf(0,floorf(c->scissorY*sy)));
    const float right=fminf(width,fmaxf(left,ceilf((c->scissorX+c->scissorW)*sx)));
    const float bottom=fminf(height,fmaxf(top,ceilf((c->scissorY+c->scissorH)*sy)));
    GXSetScissorRender((u32)left,(u32)top,(u32)(right-left),(u32)(bottom-top));
#else
    (void)camera;
#endif
}

#ifndef MP6_HEADLESS_BUILD
/* A/B diagnostics (MP6_AO_AB): game-thread time in the decal brackets and in
 * camera-end foliage replay + apply, read and reset by mp6_ao_ab_stats. */
extern uint64_t mp6_host_monotonic_ns(void);
unsigned long long mp6AoCpuDecalNs, mp6AoCpuFoliageNs, mp6AoCpuApplyNs;
#endif

void mp6_ao_decals(int camera, int after)
{
#ifndef MP6_HEADLESS_BUILD
    if (mp6_enh_ambient_occlusion() && camera >= 0 && camera < HU3D_CAM_MAX) {
        const uint64_t t0 = mp6_host_monotonic_ns();
        mp6_ao_capture_decals(camera, after);
        mp6AoCpuDecalNs += mp6_host_monotonic_ns() - t0;
    }
#else
    (void)camera; (void)after;
#endif
}

void mp6_ao_depth_about_to_clear(int camera)
{
#ifndef MP6_HEADLESS_BUILD
    /* Hu3DZClear is a layer hook.  Its clear quad destroys the current
     * receiver depth, so flush exactly this epoch before the first clear
     * command.  The end hook consumes the epoch and therefore cannot shade
     * it again after the clear or on an empty clear layer. */
    if (mp6_enh_ambient_occlusion()) mp6_ao_camera_end(camera);
#else
    (void)camera;
#endif
}

void mp6_ao_camera_end(int camera)
{
#ifndef MP6_HEADLESS_BUILD
    const Mp6AoView *view = NULL;
    /* Admit only cameras with scene depth after their last clear. Retained FI
     * frames replay the admitted markers directly, without re-testing epochs. */
    const int hasDepth=mp6_ao_camera_has_depth(camera);
    if (camera < 0 || camera >= HU3D_CAM_MAX) return;
    if (mp6AoSavedViewValid[camera]) {
        view = &mp6AoSavedView[camera];
    }
    if (!hasDepth) {
        if (mp6_enh_ambient_occlusion()) mp6_ao_foliage_render(camera, 0);
        return;
    }
    /* Claim this epoch before any foliage/composite work.  A clear hook or
     * selector-end hook can call us again, but it must not shade twice. */
    mp6_ao_camera_consume(camera);
    if (!view) {
        mp6_ao_foliage_render(camera, 0);
        return;
    }
    {
        const HU3D_CAMERA liveCamera = Hu3DCamera[camera];
        const uint64_t t0 = mp6_host_monotonic_ns();
        uint64_t t1;
        Hu3DCamera[camera] = mp6AoSavedCamera[camera];
        mp6_ao_foliage_render(camera, 1);
        Hu3DCamera[camera] = liveCamera;
        t1 = mp6_host_monotonic_ns();
        mp6_ao_apply(view);
        mp6AoCpuFoliageNs += t1 - t0;
        mp6AoCpuApplyNs += mp6_host_monotonic_ns() - t1;
    }
#else
    (void)camera;
#endif
}
