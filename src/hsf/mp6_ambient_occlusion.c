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
void mp6_ao_depth_cleared(int camera) { (void)camera; }
int mp6_ao_camera_has_depth(int camera) { (void)camera; return 0; }
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

void mp6_ao_decals(int camera, int after)
{
#ifndef MP6_HEADLESS_BUILD
    if (mp6_enh_ambient_occlusion() && camera >= 0 && camera < HU3D_CAM_MAX)
        mp6_ao_capture_decals(camera, after);
#else
    (void)camera; (void)after;
#endif
}

void mp6_ao_camera_end(int camera)
{
#ifndef MP6_HEADLESS_BUILD
    const HU3D_CAMERA *c;
    Mp6AoView view;
    float width, height;
    /* Admit only cameras with scene depth after their last clear. Retained FI
     * frames replay the admitted markers directly, without re-testing epochs. */
    const int hasDepth=mp6_ao_camera_has_depth(camera);
    mp6_ao_foliage_render(camera,hasDepth);
    if (!hasDepth) return;
    if (!mp6_enh_ambient_occlusion() || camera < 0 || camera >= HU3D_CAM_MAX || !RenderMode) return;
    c = &Hu3DCamera[camera];
    width = RenderMode->fbWidth;
    height = RenderMode->efbHeight;
    if (width <= 0 || height <= 0 || c->fov <= 0 || c->fov >= 179 ||
        c->near <= 0 || c->far <= c->near || c->aspect <= 0 ||
        c->viewportW <= 0 || c->viewportH <= 0 || c->viewportFar <= c->viewportNear) return;
    view.projection[0] = tanf(c->fov * 0.00872664626f);
    view.projection[1] = c->aspect;
    view.projection[2] = c->near;
    view.projection[3] = c->far;
    view.viewport[0] = c->viewportX / width;
    view.viewport[1] = c->viewportY / height;
    view.viewport[2] = c->viewportW / width;
    view.viewport[3] = c->viewportH / height;
    view.scissor[0] = c->scissorX / width;
    view.scissor[1] = c->scissorY / height;
    view.scissor[2] = c->scissorW / width;
    view.scissor[3] = c->scissorH / height;
    view.depthRange[0] = c->viewportNear;
    view.depthRange[1] = c->viewportFar;
    view.camera = camera;
    mp6_ao_apply(&view);
#else
    (void)camera;
#endif
}
