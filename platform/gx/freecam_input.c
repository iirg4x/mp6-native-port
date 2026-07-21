/* MP6 native port -- freecam input collector (windowed builds only; see
 * shim/include/mp6_freecam.h for the two-TU split and the automation
 * contract). Turns host input into per-tick camera deltas and pushes them
 * through mp6_freecam_host_input(); the decomp-side half
 * (platform/hsf/mp6_freecam.c) integrates them at Hu3DExec entry.
 *
 * Bindings (documented on the Mods page too):
 *   desktop: W/A/S/D move, Q/E down/up, hold RIGHT MOUSE + move = look,
 *            wheel = dolly, SHIFT = fast, CTRL = slow. Deliberately
 *            disjoint from the keyboard-PAD bridge's game keys
 *            (Enter/Space/Z/X/arrows -- aurora_bridge.c g_keyBinds), so
 *            flying never leaks game input.
 *   gamepad: right stick = look (the game itself reads the C-substick on
 *            port 0 only in scenes freecam users are not driving).
 *   android: one finger drag = look, two-finger drag = move (screen-space
 *            pan), pinch = dolly. Fingers that land on the touch-pad
 *            controls (or the gear button) keep driving the virtual pad,
 *            not the camera.
 *
 * Everything here is inert unless mp6_freecam_enabled() -- and movement is
 * additionally suspended while the in-game menu is open
 * (mp6_launcher_menu_visible), so menu navigation never flies the camera.
 */
#include <SDL3/SDL.h>
#include <dolphin/pad.h> /* aurora's pad.h: PADGetIndexForPort/PADGetSDLGamepadForIndex */

#include <math.h>
#include <string.h>

#include "mp6_freecam.h"

extern int mp6_launcher_menu_visible(void); /* platform/gx/ui/launcher_core.cpp */
#ifdef __ANDROID__
extern int mp6_touch_pad_control_at(float nx, float ny); /* platform/android/touch_pad.cpp */
#endif

/* Tuning (world units per tick / degrees per unit). The party board's own
 * camera sits at zoom ~2150, so ~15 units/tick (~900/s at 60 Hz) reads as
 * a comfortable base fly speed. */
#define FC_MOVE_PER_TICK   15.0f
#define FC_FAST_MULT        4.0f
#define FC_SLOW_MULT        0.25f
#define FC_MOUSE_DEG_PER_PX 0.22f
#define FC_WHEEL_DOLLY     120.0f
#define FC_STICK_DEG_PER_TICK 2.6f
#define FC_STICK_DEADZONE   0.20f
#define FC_TOUCH_LOOK_DEG  200.0f  /* full screen-width drag, degrees */
#define FC_TOUCH_MOVE_UNITS 1800.0f /* full screen-width two-finger drag */
#define FC_TOUCH_PINCH_UNITS 2600.0f

/* Event-accumulated deltas (drained once per tick). */
static float g_mouseLookYaw, g_mouseLookPitch;
static float g_wheelDolly;
static float g_touchYaw, g_touchPitch;
static float g_touchRight, g_touchUp;
static float g_touchDolly;

/* Touch tracking (normalized coords; slot per finger). */
#define FC_MAX_FINGERS 8
typedef struct {
    int active;
    int grabbed; /* landed on a touch-pad control -- never drives the camera */
    SDL_FingerID id;
    float x, y;
} FCFinger;
static FCFinger g_fingers[FC_MAX_FINGERS];

static FCFinger *fc_find(SDL_FingerID id)
{
    int i;
    for (i = 0; i < FC_MAX_FINGERS; i++) {
        if (g_fingers[i].active && g_fingers[i].id == id) return &g_fingers[i];
    }
    return NULL;
}

static int fc_camera_finger_count(void)
{
    int i, n = 0;
    for (i = 0; i < FC_MAX_FINGERS; i++) {
        if (g_fingers[i].active && !g_fingers[i].grabbed) n++;
    }
    return n;
}

static FCFinger *fc_other_camera_finger(const FCFinger *self)
{
    int i;
    for (i = 0; i < FC_MAX_FINGERS; i++) {
        FCFinger *f = &g_fingers[i];
        if (f->active && !f->grabbed && f != self) return f;
    }
    return NULL;
}

void mp6_freecam_input_event(const SDL_Event *ev)
{
    if (!mp6_freecam_enabled() || mp6_launcher_menu_visible()) {
        return;
    }
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (ev->motion.state & SDL_BUTTON_RMASK) {
            g_mouseLookYaw += -ev->motion.xrel * FC_MOUSE_DEG_PER_PX;
            g_mouseLookPitch += -ev->motion.yrel * FC_MOUSE_DEG_PER_PX;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        g_wheelDolly += ev->wheel.y * FC_WHEEL_DOLLY;
        break;
    case SDL_EVENT_FINGER_DOWN: {
        int i;
        if (fc_find(ev->tfinger.fingerID)) break;
        for (i = 0; i < FC_MAX_FINGERS; i++) {
            if (!g_fingers[i].active) {
                g_fingers[i].active = 1;
                g_fingers[i].id = ev->tfinger.fingerID;
                g_fingers[i].x = ev->tfinger.x;
                g_fingers[i].y = ev->tfinger.y;
#ifdef __ANDROID__
                g_fingers[i].grabbed = mp6_touch_pad_control_at(ev->tfinger.x, ev->tfinger.y);
#else
                g_fingers[i].grabbed = 0;
#endif
                break;
            }
        }
        break;
    }
    case SDL_EVENT_FINGER_MOTION: {
        FCFinger *f = fc_find(ev->tfinger.fingerID);
        float dx, dy;
        if (f == NULL) break;
        dx = ev->tfinger.x - f->x;
        dy = ev->tfinger.y - f->y;
        f->x = ev->tfinger.x;
        f->y = ev->tfinger.y;
        if (f->grabbed) break;
        if (fc_camera_finger_count() == 1) {
            /* one finger: look (drag right = look right) */
            g_touchYaw += -dx * FC_TOUCH_LOOK_DEG;
            g_touchPitch += -dy * FC_TOUCH_LOOK_DEG;
        } else {
            FCFinger *other = fc_other_camera_finger(f);
            /* two+ fingers: this finger's HALF of the pan (both fingers
             * report motion, each contributing half keeps the pan speed
             * finger-count independent), plus pinch dolly from the
             * distance change against the other finger. */
            g_touchRight += -dx * FC_TOUCH_MOVE_UNITS * 0.5f;
            g_touchUp += dy * FC_TOUCH_MOVE_UNITS * 0.5f;
            if (other != NULL) {
                float preDx = (f->x - dx) - other->x;
                float preDy = (f->y - dy) - other->y;
                float postDx = f->x - other->x;
                float postDy = f->y - other->y;
                float pre = sqrtf(preDx * preDx + preDy * preDy);
                float post = sqrtf(postDx * postDx + postDy * postDy);
                g_touchDolly += (post - pre) * FC_TOUCH_PINCH_UNITS;
            }
        }
        break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        FCFinger *f = fc_find(ev->tfinger.fingerID);
        if (f) f->active = 0;
        break;
    }
    default:
        break;
    }
}

void mp6_freecam_input_tick(void)
{
    float lookYaw, lookPitch, right, up, fwd, dolly;
    float mult = 1.0f;

    if (!mp6_freecam_enabled()) {
        /* Drop anything stale so a later re-enable starts clean. */
        g_mouseLookYaw = g_mouseLookPitch = g_wheelDolly = 0.0f;
        g_touchYaw = g_touchPitch = g_touchRight = g_touchUp = g_touchDolly = 0.0f;
        return;
    }
    if (mp6_launcher_menu_visible()) {
        g_mouseLookYaw = g_mouseLookPitch = g_wheelDolly = 0.0f;
        g_touchYaw = g_touchPitch = g_touchRight = g_touchUp = g_touchDolly = 0.0f;
        return;
    }

    lookYaw = g_mouseLookYaw + g_touchYaw;
    lookPitch = g_mouseLookPitch + g_touchPitch;
    right = g_touchRight;
    up = g_touchUp;
    fwd = 0.0f;
    dolly = g_wheelDolly + g_touchDolly;
    g_mouseLookYaw = g_mouseLookPitch = g_wheelDolly = 0.0f;
    g_touchYaw = g_touchPitch = g_touchRight = g_touchUp = g_touchDolly = 0.0f;

    {
        const bool *kb = SDL_GetKeyboardState(NULL);
        if (kb != NULL) {
            if (kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT]) mult = FC_FAST_MULT;
            else if (kb[SDL_SCANCODE_LCTRL] || kb[SDL_SCANCODE_RCTRL]) mult = FC_SLOW_MULT;
            if (kb[SDL_SCANCODE_W]) fwd += FC_MOVE_PER_TICK;
            if (kb[SDL_SCANCODE_S]) fwd -= FC_MOVE_PER_TICK;
            if (kb[SDL_SCANCODE_D]) right += FC_MOVE_PER_TICK;
            if (kb[SDL_SCANCODE_A]) right -= FC_MOVE_PER_TICK;
            if (kb[SDL_SCANCODE_E]) up += FC_MOVE_PER_TICK;
            if (kb[SDL_SCANCODE_Q]) up -= FC_MOVE_PER_TICK;
        }
    }

    /* Pad right stick = look (port 0's mapped gamepad, if any). */
    {
        s32 idx = PADGetIndexForPort(0);
        if (idx >= 0) {
            SDL_Gamepad *gp = PADGetSDLGamepadForIndex((u32)idx);
            if (gp != NULL) {
                float rx = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
                float ry = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
                if (fabsf(rx) > FC_STICK_DEADZONE) lookYaw += -rx * FC_STICK_DEG_PER_TICK;
                if (fabsf(ry) > FC_STICK_DEADZONE) lookPitch += -ry * FC_STICK_DEG_PER_TICK;
            }
        }
    }

    if (lookYaw != 0.0f || lookPitch != 0.0f || right != 0.0f || up != 0.0f ||
        fwd != 0.0f || dolly != 0.0f) {
        mp6_freecam_host_input(lookYaw, lookPitch, right * mult, up * mult, fwd * mult, dolly);
    }
}
