/* On-screen touch controls for the Android build: SDL finger events ->
 * screen regions -> PAD bits, drawn as a subtle translucent overlay via
 * aurora's own per-frame ImGui pass.
 *
 * DESIGN:
 *
 *   - Input path: this TU produces a button mask + analog tilt that
 *     platform/gx/aurora_bridge.c's mp6_pump_keyboard_to_pad() ORs into
 *     the SAME PADStatus it already builds for the keyboard/input-script
 *     bridges, going out through the SAME PADSetVirtualStatus(0, ...)
 *     call -- one merge point, additive with a real Bluetooth/USB
 *     controller exactly like the keyboard bridge is (aurora pad.cpp's
 *     merge_virtual_status). Nothing here talks to the game directly.
 *
 *   - Rendering path: aurora composites an ImGui frame OVER the presented
 *     game every frame (lib/aurora.cpp: imgui::new_frame in
 *     aurora_begin_frame, imgui::freeze + imgui::render after the game
 *     blit in aurora_end_frame) -- so drawing into ImGui's foreground
 *     draw list is a zero-GX-contact overlay: it cannot disturb the
 *     game's own GX state machine, and it lands ON TOP of the letterboxed
 *     present, so the pillarbox bands a 21.5:9 phone shows around the 4:3
 *     scene are usable control space -- thumbs never cover the game.
 *
 *   - Region model: fingers are tracked by SDL_FingerID in a fixed slot
 *     array; each finger is ASSIGNED to a control at finger-down (d-pad
 *     disc / A / B / START hit circles, sized larger than the visuals for
 *     thumb slop) and keeps that assignment while it moves (d-pad
 *     direction follows the drag even outside the disc, the standard
 *     virtual-stick behavior; buttons release if the finger slides well
 *     clear). Multi-touch is inherent: one finger can hold the d-pad while
 *     another taps A.
 *
 *   - D-pad -> BOTH digital bits and analog tilt: MP6's menus are split on
 *     this -- most read HuPadBtnDown/HuPadDStkRep interchangeably, but the
 *     file-select name-entry cursor reads ONLY HuPadDStkRep, which
 *     game/pad.c's PadADConv derives from the ANALOG stick alone. Emitting
 *     both from one gesture drives every menu; a menu that honors both
 *     sources sees one coherent direction edge on the same tick, not two
 *     conflicting ones.
 *
 * SDL touch coordinates are normalized 0..1 across the window
 * (SDL_TouchFingerEvent.x/y); layout geometry is computed from the last
 * ImGui display size (points -- same space ImGui draws in), cached by
 * mp6_touch_pad_draw() each frame. Events that arrive before the first
 * drawn frame hit a zero-size layout and are ignored -- boot's first
 * frames have no touch UI to press anyway.
 *
 * Android-only by construction: compiled exclusively into the --windowed
 * aarch64-android row (tools/build.py); the Windows build never sees this
 * TU, and every aurora_bridge.c call site is #ifdef __ANDROID__ (its
 * Windows preprocessed output stays byte-identical).
 */

#include <SDL3/SDL.h>
#include <dolphin/pad.h> /* aurora's own PAD_BUTTON_* bit values (include/dolphin/pad.h) */
#include <imgui.h>

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Layout -- height-relative units anchored to the window edges, so the
 * controls land in the pillarbox bands on a wide phone (2340x1080: bands
 * are 450 px each side of the 1440 px 4:3 scene) and degrade to over-game
 * corner placement on anything narrower. Values chosen on the target
 * device for thumb reach in landscape (Samsung S22+); nothing game-side
 * depends on them. */
#define TP_MARGIN_H      0.035f /* edge margin, fraction of window height */
#define TP_DPAD_HIT_R    0.210f /* d-pad assignment disc (hit) radius */
#define TP_DPAD_VIS_R    0.165f /* d-pad drawn disc radius */
#define TP_DPAD_DEAD     0.30f  /* neutral zone, fraction of HIT radius */
#define TP_A_R           0.085f /* A visual radius (GC: A is the big one) */
#define TP_B_R           0.062f /* B visual radius */
#define TP_BTN_HIT_SCALE 1.35f  /* button hit circle = visual * this */
#define TP_BTN_DROP_SCALE 1.9f  /* assigned finger releases past this * visual */
#define TP_START_HW      0.085f /* START pill half-width */
#define TP_START_HH      0.032f /* START pill half-height */
#define TP_MAX_FINGERS   10

typedef enum {
    TP_CTL_NONE = 0,
    TP_CTL_DPAD,
    TP_CTL_A,
    TP_CTL_B,
    TP_CTL_START,
    TP_CTL_GEAR, /* in-game menu button: contributes NO pad bits; a tap
                  * (down+up inside the circle) toggles the RmlUi settings
                  * menu via mp6_launcher_toggle_menu() */
} TPControl;

/* In-game menu seam (platform/gx/ui/launcher_core.cpp). Both inert in
 * automation mode by the launcher TU's own mode guard. */
extern "C" void mp6_launcher_toggle_menu(void);
extern "C" int mp6_launcher_menu_visible(void);

typedef struct {
    bool active;
    SDL_FingerID id;
    TPControl ctl;
    float x, y; /* window points, layout space */
} TPFinger;

typedef struct {
    float w, h;      /* cached window size (ImGui display points) */
    float dpadCx, dpadCy, dpadHitR, dpadVisR;
    float aCx, aCy, aR;
    float bCx, bCy, bR;
    float stCx, stCy, stHw, stHh;
    float gearCx, gearCy, gearR; /* persistent in-game menu (gear) button */
} TPLayout;

static TPLayout g_layout;   /* zero until the first drawn frame */
static TPFinger g_fingers[TP_MAX_FINGERS];
static unsigned short g_lastLoggedMask = 0;
static bool g_lastLogValid = false;
static bool g_layoutLogged = false;

/* One-tick down-edge latch, the touch twin of aurora_bridge.c's
 * g_padKeyLatch: a synthetic tap (`adb shell input tap`, and the fastest
 * real taps) delivers FINGER_DOWN and FINGER_UP inside the SAME
 * aurora_update() pump, so the per-finger STATE below reads "no fingers"
 * by the time the tick's collect runs -- the same missed-edge race the
 * keyboard bridge solves. The down
 * edge latches the control's bits (for the d-pad: the direction at the
 * down position) so at least one full PADSetVirtualStatus/PADRead cycle
 * observes the press, however fast the up followed. Consumed (cleared) by
 * the next collect; a genuinely-held finger keeps working via the state
 * scan alone. */
static unsigned short g_downLatchMask = 0;
static signed char g_downLatchSX = 0;
static signed char g_downLatchSY = 0;

static void tp_layout_update(float w, float h)
{
    float m;
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    if (g_layout.w == w && g_layout.h == h) {
        return;
    }
    m = TP_MARGIN_H * h;
    g_layout.w = w;
    g_layout.h = h;
    g_layout.dpadHitR = TP_DPAD_HIT_R * h;
    g_layout.dpadVisR = TP_DPAD_VIS_R * h;
    g_layout.dpadCx = m + g_layout.dpadHitR;
    g_layout.dpadCy = h - m - g_layout.dpadHitR;
    g_layout.aR = TP_A_R * h;
    g_layout.aCx = w - m - 0.115f * h;
    g_layout.aCy = 0.52f * h;
    g_layout.bR = TP_B_R * h;
    g_layout.bCx = w - m - 0.270f * h;
    g_layout.bCy = 0.70f * h;
    g_layout.stHw = TP_START_HW * h;
    g_layout.stHh = TP_START_HH * h;
    g_layout.stCx = w - m - 0.190f * h;
    g_layout.stCy = 0.16f * h;
    /* Gear: top-left corner band (START owns top-right), small and out of
     * every gameplay control's reach. */
    g_layout.gearR = 0.048f * h;
    g_layout.gearCx = m + g_layout.gearR;
    g_layout.gearCy = m + g_layout.gearR;
    if (!g_layoutLogged) {
        g_layoutLogged = true;
        printf("[TOUCH] overlay armed: window %.0fx%.0f -- dpad@(%.0f,%.0f r=%.0f) "
               "A@(%.0f,%.0f r=%.0f) B@(%.0f,%.0f r=%.0f) START@(%.0f,%.0f %.0fx%.0f)\n",
               w, h, g_layout.dpadCx, g_layout.dpadCy, g_layout.dpadVisR,
               g_layout.aCx, g_layout.aCy, g_layout.aR,
               g_layout.bCx, g_layout.bCy, g_layout.bR,
               g_layout.stCx, g_layout.stCy, g_layout.stHw * 2.0f, g_layout.stHh * 2.0f);
        fflush(stdout);
    }
}

static float tp_dist(float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    return sqrtf(dx * dx + dy * dy);
}

static TPControl tp_hit_test(float x, float y)
{
    if (g_layout.w <= 0.0f) return TP_CTL_NONE;
    if (tp_dist(x, y, g_layout.gearCx, g_layout.gearCy) <= g_layout.gearR * 1.5f) return TP_CTL_GEAR;
    /* While the in-game menu is open the PAD controls are hidden (draw) and
     * inert (PADBlockInput neutralizes the virtual status anyway) -- only
     * the gear keeps accepting taps, as the close button. */
    if (mp6_launcher_menu_visible()) return TP_CTL_NONE;
    if (tp_dist(x, y, g_layout.dpadCx, g_layout.dpadCy) <= g_layout.dpadHitR * 1.15f) return TP_CTL_DPAD;
    if (tp_dist(x, y, g_layout.aCx, g_layout.aCy) <= g_layout.aR * TP_BTN_HIT_SCALE) return TP_CTL_A;
    if (tp_dist(x, y, g_layout.bCx, g_layout.bCy) <= g_layout.bR * TP_BTN_HIT_SCALE) return TP_CTL_B;
    if (fabsf(x - g_layout.stCx) <= g_layout.stHw * TP_BTN_HIT_SCALE &&
        fabsf(y - g_layout.stCy) <= g_layout.stHh * 2.2f) return TP_CTL_START;
    return TP_CTL_NONE;
}

/* Freecam collector query (platform/gx/freecam_input.c): does a finger at
 * NORMALIZED (nx,ny) land on any touch control (incl. the gear)? Such
 * fingers keep driving the virtual pad, never the camera. */
extern "C" int mp6_touch_pad_control_at(float nx, float ny)
{
    if (g_layout.w <= 0.0f) return 0;
    return tp_hit_test(nx * g_layout.w, ny * g_layout.h) != TP_CTL_NONE;
}

static TPFinger *tp_find(SDL_FingerID id)
{
    for (int i = 0; i < TP_MAX_FINGERS; i++) {
        if (g_fingers[i].active && g_fingers[i].id == id) return &g_fingers[i];
    }
    return nullptr;
}

static TPFinger *tp_free_slot(void)
{
    for (int i = 0; i < TP_MAX_FINGERS; i++) {
        if (!g_fingers[i].active) return &g_fingers[i];
    }
    return nullptr;
}

/* Slide-off release for the buttons: an assigned finger that drags far
 * outside its control's visual stops pressing it (but keeps its slot, so
 * dragging back re-presses -- matches every stock virtual gamepad). */
static bool tp_button_engaged(const TPFinger *f)
{
    switch (f->ctl) {
    case TP_CTL_A:
        return tp_dist(f->x, f->y, g_layout.aCx, g_layout.aCy) <= g_layout.aR * TP_BTN_DROP_SCALE;
    case TP_CTL_B:
        return tp_dist(f->x, f->y, g_layout.bCx, g_layout.bCy) <= g_layout.bR * TP_BTN_DROP_SCALE;
    case TP_CTL_START:
        return fabsf(f->x - g_layout.stCx) <= g_layout.stHw * TP_BTN_DROP_SCALE &&
               fabsf(f->y - g_layout.stCy) <= g_layout.stHh * 3.5f;
    default:
        return false;
    }
}

/* D-pad state for the CURRENT finger position: digital 8-way bits plus the
 * proportional analog tilt (see the file header for why both). Screen y
 * grows DOWN; GC stick y grows UP -- inverted here. */
static unsigned short tp_dpad_bits(const TPFinger *f, signed char *sx, signed char *sy)
{
    float dx = f->x - g_layout.dpadCx;
    float dy = f->y - g_layout.dpadCy;
    float len = sqrtf(dx * dx + dy * dy);
    unsigned short bits = 0;
    if (len < g_layout.dpadHitR * TP_DPAD_DEAD) {
        return 0; /* neutral center -- no direction, no tilt */
    }
    {
        /* Proportional tilt, saturating at the hit-disc edge; +-72 is the
         * effective hardware range game/pad.c's PadADConv expects (the
         * input-script's stick:X step uses the same +-72). */
        float nx = dx / (len > g_layout.dpadHitR ? len : g_layout.dpadHitR);
        float ny = -dy / (len > g_layout.dpadHitR ? len : g_layout.dpadHitR);
        float mag = 72.0f;
        int ix = (int)lrintf(nx * mag);
        int iy = (int)lrintf(ny * mag);
        if (ix > 72) ix = 72; else if (ix < -72) ix = -72;
        if (iy > 72) iy = 72; else if (iy < -72) iy = -72;
        *sx = (signed char)ix;
        *sy = (signed char)iy;
    }
    {
        /* 8-way digital: sector per 45 degrees, diagonals set two bits.
         * atan2 y-axis flipped to screen-up like the tilt above. */
        float ang = atan2f(-dy, dx); /* -pi..pi, 0 = right, +pi/2 = up */
        float deg = ang * 57.29578f;
        if (deg >= -67.5f && deg <= 67.5f) bits |= PAD_BUTTON_RIGHT;
        if (deg >= 22.5f && deg <= 157.5f) bits |= PAD_BUTTON_UP;
        if (deg >= 112.5f || deg <= -112.5f) bits |= PAD_BUTTON_LEFT;
        if (deg >= -157.5f && deg <= -22.5f) bits |= PAD_BUTTON_DOWN;
    }
    return bits;
}

extern "C" void mp6_touch_pad_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_FINGER_DOWN: {
        float x = ev->tfinger.x * g_layout.w;
        float y = ev->tfinger.y * g_layout.h;
        TPFinger *f;
        if (tp_find(ev->tfinger.fingerID)) return; /* duplicate down -- keep first */
        f = tp_free_slot();
        if (!f) return;
        f->active = true;
        f->id = ev->tfinger.fingerID;
        f->x = x;
        f->y = y;
        f->ctl = tp_hit_test(x, y);
        /* Down-edge latch (see g_downLatchMask's comment) + the per-press
         * evidence line -- user-rate, not per-tick, so unthrottled. */
        switch (f->ctl) {
        case TP_CTL_GEAR:  /* no pad bits; the toggle fires on FINGER_UP */ break;
        case TP_CTL_A:     g_downLatchMask |= PAD_BUTTON_A;     break;
        case TP_CTL_B:     g_downLatchMask |= PAD_BUTTON_B;     break;
        case TP_CTL_START: g_downLatchMask |= PAD_BUTTON_START; break;
        case TP_CTL_DPAD: {
            signed char lsx = 0, lsy = 0;
            g_downLatchMask |= tp_dpad_bits(f, &lsx, &lsy);
            if (lsx != 0 || lsy != 0) {
                g_downLatchSX = lsx;
                g_downLatchSY = lsy;
            }
            break;
        }
        default: break;
        }
        printf("[TOUCH] finger down @(%.0f,%.0f) ctl=%d latch=0x%04x\n",
               x, y, (int)f->ctl, g_downLatchMask);
        fflush(stdout);
        break;
    }
    case SDL_EVENT_FINGER_MOTION: {
        TPFinger *f = tp_find(ev->tfinger.fingerID);
        if (!f) return;
        f->x = ev->tfinger.x * g_layout.w;
        f->y = ev->tfinger.y * g_layout.h;
        break;
    }
    case SDL_EVENT_FINGER_UP: {
        TPFinger *f = tp_find(ev->tfinger.fingerID);
        if (f) {
            f->active = false;
            /* Gear tap: toggle the in-game menu if the finger lifted while
             * still on the button (slide-off cancels, like any button). */
            if (f->ctl == TP_CTL_GEAR &&
                tp_dist(f->x, f->y, g_layout.gearCx, g_layout.gearCy) <= g_layout.gearR * 1.9f) {
                printf("[TOUCH] gear tap -- toggling in-game menu\n");
                fflush(stdout);
                mp6_launcher_toggle_menu();
            }
        }
        break;
    }
    case SDL_EVENT_FINGER_CANCELED: {
        /* The system stole the gesture (edge swipe, palm rejection) --
         * release everything owned by that finger; a stuck virtual button
         * would be a soft-lock. Partyboard's overlay resets its whole
         * tracker on CANCELED for the same reason. */
        TPFinger *f = tp_find(ev->tfinger.fingerID);
        if (f) f->active = false;
        break;
    }
    default:
        break;
    }
}

extern "C" void mp6_touch_pad_collect(unsigned short *btnOut, signed char *stickXOut, signed char *stickYOut)
{
    unsigned short btn = 0;
    signed char sx = 0, sy = 0;
    for (int i = 0; i < TP_MAX_FINGERS; i++) {
        TPFinger *f = &g_fingers[i];
        if (!f->active) continue;
        switch (f->ctl) {
        case TP_CTL_DPAD:
            btn |= tp_dpad_bits(f, &sx, &sy);
            break;
        case TP_CTL_A:
            if (tp_button_engaged(f)) btn |= PAD_BUTTON_A;
            break;
        case TP_CTL_B:
            if (tp_button_engaged(f)) btn |= PAD_BUTTON_B;
            break;
        case TP_CTL_START:
            if (tp_button_engaged(f)) btn |= PAD_BUTTON_START;
            break;
        default:
            break;
        }
    }
    /* Merge + consume the one-tick down-edge latch (comment at its
     * definition). Latched stick tilt only fills in when no live finger
     * is supplying one this tick. */
    btn |= g_downLatchMask;
    if (sx == 0 && sy == 0 && (g_downLatchSX != 0 || g_downLatchSY != 0)) {
        sx = g_downLatchSX;
        sy = g_downLatchSY;
    }
    g_downLatchMask = 0;
    g_downLatchSX = 0;
    g_downLatchSY = 0;

    *btnOut = btn;
    *stickXOut = sx;
    *stickYOut = sy;
    /* Logcat evidence trail (throttled to CHANGES only -- a held button
     * prints once, not per tick) so PAD bits are observably moving in
     * response to touch. */
    if (!g_lastLogValid || btn != g_lastLoggedMask) {
        g_lastLogValid = true;
        g_lastLoggedMask = btn;
        printf("[TOUCH] pad mask=0x%04x stick=(%d,%d)\n", btn, (int)sx, (int)sy);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing -- foreground draw list only; runs between aurora_begin_frame()
 * and the frame's eventual aurora_end_frame() (the bridge calls this right
 * after begin_frame succeeds, so ImGui::NewFrame has already run). */

static ImU32 tp_col(unsigned r, unsigned g, unsigned b, float a)
{
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return IM_COL32(r, g, b, (unsigned)(a * 255.0f + 0.5f));
}

static void tp_text_centered(ImDrawList *dl, float cx, float cy, float size, ImU32 col, const char *txt)
{
    ImFont *font = ImGui::GetFont();
    ImVec2 ext = font->CalcTextSizeA(size, 3.4e38f, 0.0f, txt);
    dl->AddText(font, size, ImVec2(cx - ext.x * 0.5f, cy - ext.y * 0.5f), col, txt);
}

extern "C" void mp6_touch_pad_draw(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl;
    unsigned short mask = 0;
    signed char sx = 0, sy = 0;
    bool anyTouch = false;
    float dim;

    tp_layout_update(io.DisplaySize.x, io.DisplaySize.y);
    if (g_layout.w <= 0.0f) {
        return;
    }
    for (int i = 0; i < TP_MAX_FINGERS; i++) {
        if (g_fingers[i].active) { anyTouch = true; break; }
    }
    /* Recompute the live mask purely for HIGHLIGHT state (cheap, and keeps
     * draw decoupled from the collect call order). */
    for (int i = 0; i < TP_MAX_FINGERS; i++) {
        TPFinger *f = &g_fingers[i];
        if (!f->active) continue;
        if (f->ctl == TP_CTL_DPAD) mask |= tp_dpad_bits(f, &sx, &sy);
        else if (f->ctl == TP_CTL_A && tp_button_engaged(f)) mask |= PAD_BUTTON_A;
        else if (f->ctl == TP_CTL_B && tp_button_engaged(f)) mask |= PAD_BUTTON_B;
        else if (f->ctl == TP_CTL_START && tp_button_engaged(f)) mask |= PAD_BUTTON_START;
    }

    dim = anyTouch ? 1.0f : 0.62f; /* idle overlay stays subtle; touching brightens it */
    dl = ImGui::GetForegroundDrawList();

    /* --- gear (in-game menu toggle) -- always drawn, doubles as the close
     * button while the menu is open --------------------------------------- */
    {
        const bool menuOpen = mp6_launcher_menu_visible() != 0;
        float cx = g_layout.gearCx, cy = g_layout.gearCy, r = g_layout.gearR;
        float a = menuOpen ? 0.85f : 0.45f * dim;
        dl->AddCircleFilled(ImVec2(cx, cy), r, tp_col(30, 30, 30, 0.35f * (menuOpen ? 1.0f : dim)), 32);
        dl->AddCircle(ImVec2(cx, cy), r * 0.55f, tp_col(255, 255, 255, a), 24, r * 0.16f);
        for (int i = 0; i < 6; i++) {
            float ang = (float)i * 1.047198f; /* 60 deg */
            float c = cosf(ang), s = sinf(ang);
            dl->AddLine(ImVec2(cx + c * r * 0.62f, cy + s * r * 0.62f),
                        ImVec2(cx + c * r * 0.92f, cy + s * r * 0.92f),
                        tp_col(255, 255, 255, a), r * 0.20f);
        }
        dl->AddCircle(ImVec2(cx, cy), r, tp_col(255, 255, 255, 0.30f * (menuOpen ? 1.0f : dim)), 32, 2.0f);
        /* Menu open: the pad controls below are hidden (their input is
         * PADBlockInput()ed; drawing them over the RmlUi menu would just
         * be clutter) -- the gear alone stays, as the close button. */
        if (menuOpen) {
            return;
        }
    }

    /* --- d-pad: base disc + 4 direction wedges (triangles) ----------- */
    {
        float cx = g_layout.dpadCx, cy = g_layout.dpadCy, R = g_layout.dpadVisR;
        float in = R * 0.28f;  /* wedge inner edge */
        float out = R * 0.88f; /* wedge tip */
        float hw = R * 0.30f;  /* wedge half-width at inner edge */
        struct { unsigned short bit; float dx, dy; } dirs[4] = {
            { PAD_BUTTON_UP,    0.0f, -1.0f },
            { PAD_BUTTON_DOWN,  0.0f,  1.0f },
            { PAD_BUTTON_LEFT, -1.0f,  0.0f },
            { PAD_BUTTON_RIGHT, 1.0f,  0.0f },
        };
        dl->AddCircleFilled(ImVec2(cx, cy), R, tp_col(255, 255, 255, 0.07f * dim), 48);
        dl->AddCircle(ImVec2(cx, cy), R, tp_col(255, 255, 255, 0.32f * dim), 48, 2.0f);
        for (int i = 0; i < 4; i++) {
            float px = -dirs[i].dy, py = dirs[i].dx; /* perpendicular */
            bool on = (mask & dirs[i].bit) != 0;
            ImVec2 tip(cx + dirs[i].dx * out, cy + dirs[i].dy * out);
            ImVec2 baseA(cx + dirs[i].dx * in + px * hw, cy + dirs[i].dy * in + py * hw);
            ImVec2 baseB(cx + dirs[i].dx * in - px * hw, cy + dirs[i].dy * in - py * hw);
            dl->AddTriangleFilled(tip, baseA, baseB,
                                  on ? tp_col(255, 255, 255, 0.75f)
                                     : tp_col(255, 255, 255, 0.26f * dim));
        }
    }

    /* --- A (GC green, the big one) ------------------------------------ */
    {
        bool on = (mask & PAD_BUTTON_A) != 0;
        float r = g_layout.aR * (on ? 1.06f : 1.0f);
        dl->AddCircleFilled(ImVec2(g_layout.aCx, g_layout.aCy), r,
                            tp_col(60, 180, 100, (on ? 0.62f : 0.26f) * (on ? 1.0f : dim)), 48);
        dl->AddCircle(ImVec2(g_layout.aCx, g_layout.aCy), r,
                      tp_col(120, 230, 160, 0.45f * dim), 48, 2.0f);
        tp_text_centered(dl, g_layout.aCx, g_layout.aCy, g_layout.aR * 1.1f,
                         tp_col(255, 255, 255, 0.80f * dim), "A");
    }

    /* --- B (GC red, smaller, lower-left of A) -------------------------- */
    {
        bool on = (mask & PAD_BUTTON_B) != 0;
        float r = g_layout.bR * (on ? 1.06f : 1.0f);
        dl->AddCircleFilled(ImVec2(g_layout.bCx, g_layout.bCy), r,
                            tp_col(205, 70, 70, (on ? 0.62f : 0.26f) * (on ? 1.0f : dim)), 48);
        dl->AddCircle(ImVec2(g_layout.bCx, g_layout.bCy), r,
                      tp_col(255, 130, 130, 0.45f * dim), 48, 2.0f);
        tp_text_centered(dl, g_layout.bCx, g_layout.bCy, g_layout.bR * 1.1f,
                         tp_col(255, 255, 255, 0.80f * dim), "B");
    }

    /* --- START (gray pill, top-right band) ----------------------------- */
    {
        bool on = (mask & PAD_BUTTON_START) != 0;
        ImVec2 a(g_layout.stCx - g_layout.stHw, g_layout.stCy - g_layout.stHh);
        ImVec2 b(g_layout.stCx + g_layout.stHw, g_layout.stCy + g_layout.stHh);
        dl->AddRectFilled(a, b, tp_col(200, 200, 200, (on ? 0.55f : 0.20f) * (on ? 1.0f : dim)),
                          g_layout.stHh);
        dl->AddRect(a, b, tp_col(255, 255, 255, 0.40f * dim), g_layout.stHh, 0, 2.0f);
        tp_text_centered(dl, g_layout.stCx, g_layout.stCy, g_layout.stHh * 1.35f,
                         tp_col(255, 255, 255, 0.80f * dim), "START");
    }
}
