/* On-screen Hu3D cameras use native coordinates at the API boundary.
 * Shadow/reflection targets use GX directly and are deliberately unaffected. */
#include "game/hu3d.h"
#include "mp6_widescreen.h"
#include <string.h>

typedef struct {
    int live;
    float aspect, x, width;
    float scissorX, scissorWidth;
    int nativeTarget;
    int centeredScissor;
} MP6WSCamera;

static MP6WSCamera cameras[HU3D_CAM_MAX];

static float camera_scale(int index)
{
    return cameras[index].nativeTarget ? 1.0f : mp6_widescreen_scale_factor();
}

static unsigned int clipped_span(unsigned int origin, unsigned int extent, unsigned int limit)
{
    /* Animated splits subtract their inset even as a losing view reaches
     * zero. A wrapped -4 must stay empty, never become a huge GPU scissor. */
    if (origin >= limit || extent > 0x7fffffffU) return 0;
    return extent > limit - origin ? limit - origin : extent;
}

static void clip_interval(float origin, float extent, float limit,
                          s16 *start, s16 *size)
{
    if (!isfinite(origin) || !isfinite(extent) || extent <= 0) {
        *start = *size = 0;
        return;
    }
    limit = fminf(limit, 32767.0f);
    float left = fminf(limit, fmaxf(0, origin));
    float right = fminf(limit, fmaxf(left, origin + extent));
    *start = (s16)(left + 0.5f);
    *size = (s16)((int)(right + 0.5f) - *start);
}

static void apply_camera(int index)
{
    MP6WSCamera *native = &cameras[index];
    HU3D_CAMERA *camera = &Hu3DCamera[index];
    float scale = camera_scale(index);
    if (!native->live || camera->fov == -1.0f) return;
    camera->aspect = native->aspect * scale;
    camera->viewportX = native->x * scale;
    camera->viewportW = native->width * scale;
    /* Map endpoints together: adjacent split-screen rectangles must agree on
     * the same pixel, including animated splits and zero-width hidden views. */
    if (native->centeredScissor) {
        /* A world-projected cutout follows the unchanged native composition,
         * not a proportional split of the widened framebuffer. */
        clip_interval(native->scissorX + 320.0f * (scale - 1.0f),
                      native->scissorWidth, 640.0f * scale,
                      &camera->scissorX, &camera->scissorW);
    } else {
        camera->scissorX = (unsigned int)(native->scissorX * (double)scale + 0.5);
        camera->scissorW = (unsigned int)(((double)native->scissorX + native->scissorWidth) * scale + 0.5)
                           - camera->scissorX;
    }
}

void mp6_widescreen_camera_created(int bits)
{
    int i;
    for (i = 0; i < HU3D_CAM_MAX; ++i) {
        if (!((unsigned int)bits & (1u << i))) continue;
        cameras[i] = (MP6WSCamera){1, Hu3DCamera[i].aspect, 0.0f, 640.0f, 0, 640, 0, 0};
        apply_camera(i);
    }
}

void mp6_widescreen_camera_forget(int bits)
{
    int i;
    for (i = 0; i < HU3D_CAM_MAX; ++i) {
        if ((unsigned int)bits & (1u << i)) memset(&cameras[i], 0, sizeof(cameras[i]));
    }
}

void mp6_widescreen_camera_native_target(int bits, int enabled)
{
    for (int i = 0; i < HU3D_CAM_MAX; ++i) {
        if (!((unsigned int)bits & (1u << i)) || !cameras[i].live) continue;
        cameras[i].nativeTarget = enabled != 0;
        apply_camera(i);
    }
}

void mp6_widescreen_camera_centered_scissor(int bits, int enabled)
{
    for (int i = 0; i < HU3D_CAM_MAX; ++i) {
        if (!((unsigned int)bits & (1u << i)) || !cameras[i].live) continue;
        cameras[i].centeredScissor = enabled != 0;
        apply_camera(i);
    }
}

void mp6_widescreen_camera_perspective(int index, float aspect)
{
    cameras[index].aspect = aspect;
    Hu3DCamera[index].aspect = aspect * camera_scale(index);
}

void mp6_widescreen_camera_cutout(int bits, float x, float y, float width, float height)
{
    for (int i = 0; i < HU3D_CAM_MAX; ++i) {
        if (!((unsigned int)bits & (1u << i)) || !cameras[i].live) continue;
        cameras[i].centeredScissor = 1;
        cameras[i].scissorX = x;
        cameras[i].scissorWidth = width;
        clip_interval(y, height, 480, &Hu3DCamera[i].scissorY, &Hu3DCamera[i].scissorH);
        apply_camera(i);
    }
}

void mp6_widescreen_camera_viewport(int index, float x, float width)
{
    cameras[index].x = x;
    cameras[index].width = width;
    Hu3DCamera[index].viewportX = x * camera_scale(index);
    Hu3DCamera[index].viewportW = width * camera_scale(index);
}

void mp6_widescreen_camera_scissor(int index, unsigned int x, unsigned int width)
{
    HU3D_CAMERA *camera = &Hu3DCamera[index];
    cameras[index].scissorX = x < 640 ? x : 640;
    cameras[index].scissorWidth = clipped_span(x, width, 640);
    /* The native setter has just assigned Y/H. Clip them without storing a
     * vertical baseline or interfering with viewport/FOV animation. */
    camera->scissorH = clipped_span(camera->scissorY, camera->scissorH, 480);
    if (camera->scissorY > 480) camera->scissorY = 480;
    apply_camera(index);
}

void mp6_widescreen_camera_reapply(void)
{
    int i;
    /* No cached FOV/near/far/height/position: a cinematic or a minigame can
     * animate those every tick. Only the horizontal mapping belongs here. */
    for (i = 0; i < HU3D_CAM_MAX; ++i) apply_camera(i);
}

int mp6_widescreen_offstage_offset(int bits, const float point[3], float offset[3])
{
    offset[0] = offset[1] = offset[2] = 0.0f;
    for (int i = 0; i < HU3D_CAM_MAX; ++i) {
        if (!((unsigned int)bits & (1u << i))) continue;
        const HU3D_CAMERA *camera = &Hu3DCamera[i];
        float scale = camera_scale(i);
        if (!cameras[i].live || camera->fov == -1.0f) continue;
        if (scale <= 1.0f) return 0;
        float f[3] = {camera->target.x - camera->pos.x, camera->target.y - camera->pos.y,
                      camera->target.z - camera->pos.z};
        float r[3] = {f[1] * camera->up.z - f[2] * camera->up.y,
                      f[2] * camera->up.x - f[0] * camera->up.z,
                      f[0] * camera->up.y - f[1] * camera->up.x};
        float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (fl <= 0.0f || rl <= 0.0f) return 0;
        float d[3] = {point[0] - camera->pos.x, point[1] - camera->pos.y, point[2] - camera->pos.z};
        float depth = (d[0] * f[0] + d[1] * f[1] + d[2] * f[2]) / fl;
        float side = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / rl;
        float nativeHalf = depth * tanf(camera->fov * 0.008726646f) * camera->aspect / scale;
        if (depth <= 0.0f || fabsf(side) <= nativeHalf) return 0;
        float push = nativeHalf * (scale - 1.0f) * (side < 0.0f ? -1.0f : 1.0f) / rl;
        for (int k = 0; k < 3; ++k) offset[k] = r[k] * push;
        return 1;
    }
    return 0;
}

void mp6_widescreen_camera_widen(int bits, float fov, float nearZ, float farZ, float aspect, float height)
{
    Hu3DCameraPerspectiveSet(bits, fov, nearZ, farZ, aspect);
    Hu3DCameraViewportSet(bits, 0.0f, 0.0f, 640.0f, height, 0.0f, 1.0f);
    Hu3DCameraScissorSet(bits, 0, 0, 640, (unsigned int)height);
}
