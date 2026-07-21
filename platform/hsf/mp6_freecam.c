/* MP6 native port -- freecam camera override (shim/include/mp6_freecam.h
 * has the design contract; this is the decomp-header half).
 *
 * Lives in platform/hsf/ (alongside mp6_widescreen_extrude.c) because it
 * needs game/hu3d.h for Hu3DCamera[]/Hu3DCameraPosSet -- compiled with
 * COMMON_FLAGS in BOTH build modes (its one caller, the Hu3DExec patch
 * hook, is shared). The input collector half is windowed-only
 * (platform/gx/freecam_input.c); in headless builds nothing ever calls
 * mp6_freecam_set_enabled/host_input, so apply() is a permanently-false
 * branch and the log stays byte-identical.
 */
#include "mp6_freecam.h"

#include <math.h>
#include <string.h>

#include "game/hu3d.h"

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md): freecam state belongs to the
 * RUNNING process (a UI/debug tool), not to captured game state -- a
 * restored .state must neither re-enable freecam nor teleport its pose.
 * Must sit AFTER this TU's own includes and at preprocessor TOP LEVEL. */
#include "mp6_host_section.h"


#define MP6_FC_DEG2RAD 0.017453292519943295f

static int g_enabled;
static int g_poseSeeded;

/* Fly pose. */
static float g_posX, g_posY, g_posZ;
static float g_yawDeg;   /* 0 = looking down -Z̶... derived from seed, see below */
static float g_pitchDeg; /* +up, clamped to +-89 */

/* Accumulated host input (consumed per apply). */
static float g_inYaw, g_inPitch, g_inRight, g_inUp, g_inFwd, g_inDolly;

/* The game camera's own last written pose, stashed every tick before the
 * override so disable can put it back (matters only for scenes that set
 * their camera once at load; per-tick writers rewrite it anyway). */
static Vec g_gamePos, g_gameUp, g_gameTarget;
static int g_gameStashValid;

void mp6_freecam_set_enabled(int enabled)
{
    if (g_enabled && !enabled && g_gameStashValid && Hu3DCamera[0].fov != -1.0f) {
        /* Hand the camera back exactly as the game last wrote it. */
        Hu3DCameraPosSet(HU3D_CAM0,
                         g_gamePos.x, g_gamePos.y, g_gamePos.z,
                         g_gameUp.x, g_gameUp.y, g_gameUp.z,
                         g_gameTarget.x, g_gameTarget.y, g_gameTarget.z);
    }
    g_enabled = enabled != 0;
    if (!g_enabled) {
        g_poseSeeded = 0;
        g_gameStashValid = 0;
        g_inYaw = g_inPitch = g_inRight = g_inUp = g_inFwd = g_inDolly = 0.0f;
    }
}

int mp6_freecam_enabled(void) { return g_enabled; }

void mp6_freecam_host_input(float lookYawDeg, float lookPitchDeg,
                            float moveRight, float moveUp, float moveFwd,
                            float dolly)
{
    g_inYaw += lookYawDeg;
    g_inPitch += lookPitchDeg;
    g_inRight += moveRight;
    g_inUp += moveUp;
    g_inFwd += moveFwd;
    g_inDolly += dolly;
}

void mp6_freecam_apply(void)
{
    HU3D_CAMERA *cam;
    float yawR, pitchR;
    float fwdX, fwdY, fwdZ;
    float rightX, rightZ;
    float upX, upY, upZ;

    if (!g_enabled) {
        return;
    }
    cam = &Hu3DCamera[0];
    if (cam->fov == -1.0f) {
        return; /* no live main camera this frame (scene transition) */
    }

    /* Stash what the game just wrote (restore-on-disable source). */
    g_gamePos = cam->pos;
    g_gameUp = cam->up;
    g_gameTarget = cam->target;
    g_gameStashValid = 1;

    if (!g_poseSeeded) {
        /* Seed the fly pose from the game camera: position as-is, yaw and
         * pitch from the pos->target direction. */
        float dx = cam->target.x - cam->pos.x;
        float dy = cam->target.y - cam->pos.y;
        float dz = cam->target.z - cam->pos.z;
        float horiz = sqrtf(dx * dx + dz * dz);
        g_posX = cam->pos.x;
        g_posY = cam->pos.y;
        g_posZ = cam->pos.z;
        g_yawDeg = atan2f(dx, dz) / MP6_FC_DEG2RAD; /* yaw about +Y; fwd = (sin,0,cos) */
        g_pitchDeg = atan2f(dy, horiz) / MP6_FC_DEG2RAD;
        g_poseSeeded = 1;
    }

    /* Integrate this tick's input. */
    g_yawDeg += g_inYaw;
    g_pitchDeg += g_inPitch;
    if (g_pitchDeg > 89.0f) g_pitchDeg = 89.0f;
    if (g_pitchDeg < -89.0f) g_pitchDeg = -89.0f;

    yawR = g_yawDeg * MP6_FC_DEG2RAD;
    pitchR = g_pitchDeg * MP6_FC_DEG2RAD;
    fwdX = sinf(yawR) * cosf(pitchR);
    fwdY = sinf(pitchR);
    fwdZ = cosf(yawR) * cosf(pitchR);
    /* Horizontal SCREEN-right vector (yaw only) -- strafing stays level.
     * For view dir (sin yaw, 0, cos yaw) with world up +Y, screen right is
     * cross(dir, up) = (-cos yaw, 0, sin yaw) -- verified against the
     * party camera's own frame (looking down -Z, +X is screen right). */
    rightX = -cosf(yawR);
    rightZ = sinf(yawR);

    g_posX += rightX * g_inRight + fwdX * (g_inFwd + g_inDolly);
    g_posY += g_inUp + fwdY * (g_inFwd + g_inDolly);
    g_posZ += rightZ * g_inRight + fwdZ * (g_inFwd + g_inDolly);

    g_inYaw = g_inPitch = g_inRight = g_inUp = g_inFwd = g_inDolly = 0.0f;

    /* Roll-free camera up = normalize(cross(right, fwd)) with
     * right = (rightX, 0, rightZ); flipped if needed so the camera is
     * never upside down (the pitch clamp keeps this well-conditioned). */
    {
        float cx = 0.0f * fwdZ - rightZ * fwdY;
        float cy = rightZ * fwdX - rightX * fwdZ;
        float cz = rightX * fwdY - 0.0f * fwdX;
        float len = sqrtf(cx * cx + cy * cy + cz * cz);
        if (len < 1e-6f) {
            cx = 0.0f; cy = 1.0f; cz = 0.0f; len = 1.0f;
        }
        upX = cx / len; upY = cy / len; upZ = cz / len;
        if (upY < 0.0f) {
            upX = -upX; upY = -upY; upZ = -upZ;
        }
    }

    Hu3DCameraPosSet(HU3D_CAM0,
                     g_posX, g_posY, g_posZ,
                     upX, upY, upZ,
                     g_posX + fwdX * 1000.0f,
                     g_posY + fwdY * 1000.0f,
                     g_posZ + fwdZ * 1000.0f);
}
