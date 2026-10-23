/*
 * Portable implementations for the handful of Dolphin SDK entry points
 * whose retail names explicitly select the Gekko paired-single variants.
 * The recovered sources implement those PS* names in PPC assembly; the
 * native port needs the same math under the original ABI-visible names.
 */
#include <math.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <game/gamework.h>

#ifdef MP6_HEADLESS_BUILD
#include "mp6_shim_log.h"
#endif

float __fabsf(float value)
{
    return fabsf(value);
}

/* Console preference storage used by the recovered Options overlay. Output
 * mixing remains owned by MSM; this setting must round-trip across overlays. */
static u32 nativeSoundMode = OS_SOUND_MODE_STEREO;

u32 OSGetSoundMode(void)
{
    return nativeSoundMode;
}

void OSSetSoundMode(u32 mode)
{
    if (mode == OS_SOUND_MODE_MONO || mode == OS_SOUND_MODE_STEREO)
        nativeSoundMode = mode;
}

/* MBBoardNoGet used to be defined here, out of line: on the e32fa5e pin the
 * accessor was an inline in include/REL/w01Dll_world01.h only, and src/board/
 * capsule.c -- which calls it -- does not include that REL header, so the
 * native monolith had to supply the equivalent itself.  The 9da5780 pin
 * (decomp 8d947f9, "Centralize the board number accessor") moved the inline
 * into include/game/board/main.h, which capsule.c, world01.c and s01.c all
 * include, so every caller now resolves it in its own translation unit and
 * this definition became unreachable.  Deleted rather than kept: an
 * out-of-line copy that nothing can call is one more thing to keep in sync
 * with GwSystem for no benefit. */

void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA)
{
    switch (axis) {
    case 'x':
    case 'X':
        m[0][0] = 1.0f; m[0][1] = 0.0f;  m[0][2] = 0.0f;   m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = cosA;  m[1][2] = -sinA;  m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = sinA;  m[2][2] = cosA;   m[2][3] = 0.0f;
        break;
    case 'y':
    case 'Y':
        m[0][0] = cosA;  m[0][1] = 0.0f; m[0][2] = sinA; m[0][3] = 0.0f;
        m[1][0] = 0.0f;  m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = -sinA; m[2][1] = 0.0f; m[2][2] = cosA; m[2][3] = 0.0f;
        break;
    case 'z':
    case 'Z':
        m[0][0] = cosA; m[0][1] = -sinA; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = sinA; m[1][1] = cosA;  m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f;  m[2][2] = 1.0f; m[2][3] = 0.0f;
        break;
    default:
        /* Match the SDK's release-build behavior: invalid axes do not
         * manufacture a matrix. Callers in MP6 pass only x/y/z. */
        break;
    }
}

void PSQUATMultiply(const Quaternion *p, const Quaternion *q, Quaternion *pq)
{
    Quaternion out;

    /* Use a temporary exactly like C_QUATMultiply so aliasing either input
     * with the destination remains valid. */
    out.w = (p->w * q->w) - (p->x * q->x) - (p->y * q->y) - (p->z * q->z);
    out.x = (p->w * q->x) + (p->x * q->w) + (p->y * q->z) - (p->z * q->y);
    out.y = (p->w * q->y) + (p->y * q->w) + (p->z * q->x) - (p->x * q->z);
    out.z = (p->w * q->z) + (p->z * q->w) + (p->x * q->y) - (p->y * q->x);
    *pq = out;
}

void PSQUATNormalize(const Quaternion *src, Quaternion *unit)
{
    /* Same threshold and zero result as C_QUATNormalize; src may alias unit. */
    f32 mag = (src->x * src->x) + (src->y * src->y) + (src->z * src->z) + (src->w * src->w);

    if (mag >= 0.00001f) {
        mag = 1.0f / sqrtf(mag);
        unit->x = src->x * mag;
        unit->y = src->y * mag;
        unit->z = src->z * mag;
        unit->w = src->w * mag;
    } else {
        unit->x = unit->y = unit->z = unit->w = 0.0f;
    }
}

void PSMTXQuat(Mtx m, const Quaternion *q)
{
    f32 s = 2.0f / ((q->w * q->w) + (q->x * q->x) +
                    (q->y * q->y) + (q->z * q->z));
    f32 xs = q->x * s;
    f32 ys = q->y * s;
    f32 zs = q->z * s;
    f32 wx = q->w * xs;
    f32 wy = q->w * ys;
    f32 wz = q->w * zs;
    f32 xx = q->x * xs;
    f32 xy = q->x * ys;
    f32 xz = q->x * zs;
    f32 yy = q->y * ys;
    f32 yz = q->y * zs;
    f32 zz = q->z * zs;

    m[0][0] = 1.0f - (yy + zz); m[0][1] = xy - wz;          m[0][2] = xz + wy;          m[0][3] = 0.0f;
    m[1][0] = xy + wz;          m[1][1] = 1.0f - (xx + zz); m[1][2] = yz - wx;          m[1][3] = 0.0f;
    m[2][0] = xz - wy;          m[2][1] = yz + wx;          m[2][2] = 1.0f - (xx + yy); m[2][3] = 0.0f;
}

#ifdef MP6_HEADLESS_BUILD
/* These SDK vector operations are supplied by Aurora in graphical builds.
 * Keep the CPU-only backend's results real, including aliased destinations.
 * C_VECReflect follows src/dolphin/mtx/vec.c's normalization sequence. */
f32 PSVECSquareDistance(const Vec *a, const Vec *b)
{
    f32 x = a->x - b->x;
    f32 y = a->y - b->y;
    f32 z = a->z - b->z;
    return x * x + y * y + z * z;
}

static void sdk_normalize(const Vec *src, Vec *dst)
{
    f32 scale = 1.0f / sqrtf(src->x * src->x + src->y * src->y + src->z * src->z);
    Vec result = {src->x * scale, src->y * scale, src->z * scale};
    *dst = result;
}

void C_VECReflect(const Vec *src, const Vec *normal, Vec *dst)
{
    Vec a = {-src->x, -src->y, -src->z};
    Vec b;
    sdk_normalize(&a, &a);
    sdk_normalize(normal, &b);
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z;
    Vec result = {b.x * 2.0f * dot - a.x,
                  b.y * 2.0f * dot - a.y,
                  b.z * 2.0f * dot - a.z};
    sdk_normalize(&result, dst);
}

/* Aurora supplies these three real SDK/FIFO implementations in graphical
 * builds. The null backend has no FIFO, but still needs to consume and
 * report immediate attributes so headless execution can traverse draw code
 * without pretending the calls never happened. */
void C_QUATRotAxisRad(Quaternion *q, const Vec *axis, f32 rad)
{
    f32 len = sqrtf((axis->x * axis->x) + (axis->y * axis->y) +
                    (axis->z * axis->z));
    f32 half = rad * 0.5f;
    f32 scale = sinf(half) / len;

    q->x = axis->x * scale;
    q->y = axis->y * scale;
    q->z = axis->z * scale;
    q->w = cosf(half);
}

void GXNormal3f32(f32 x, f32 y, f32 z)
{
    (void)x; (void)y; (void)z;
    mp6_shim_log("GX", "GXNormal3f32 (headless attribute sink)");
}

void GXColor1u32(u32 color)
{
    (void)color;
    mp6_shim_log("GX", "GXColor1u32 (headless attribute sink)");
}
#endif

#ifndef MP6_HEADLESS_BUILD
/* aurora/aurora.h; game files do not see Aurora's headers. */
void aurora_texture_sources_written(const void *begin, const void *end);

/* There is no CPU data cache to write back, but a flush is how the game says
 * it wrote memory the GPU will read: a texture drawn into in place (Mowtown's
 * mowed lawn) is converted again from the new bytes at its next draw. */
void DCFlushRange(void *addr, u32 nBytes)
{
    aurora_texture_sources_written(addr, (const char *)addr + nBytes);
}

void DCFlushRangeNoSync(void *addr, u32 nBytes)
{
    aurora_texture_sources_written(addr, (const char *)addr + nBytes);
}

void DCStoreRange(void *addr, u32 nBytes)
{
    aurora_texture_sources_written(addr, (const char *)addr + nBytes);
}

void DCStoreRangeNoSync(void *addr, u32 nBytes)
{
    aurora_texture_sources_written(addr, (const char *)addr + nBytes);
}
#endif
