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

/* Board modules that include game/board/main.h get this as an inline on
 * Gekko.  The recovered capsule owner does not include that header, so the
 * native monolith also provides the exact out-of-line equivalent. */
s32 MBBoardNoGet(void)
{
    return GwSystem.boardNo;
}

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
