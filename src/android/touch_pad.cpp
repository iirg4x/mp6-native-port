/* Android-only full GameCube touch controller. Host input stays outside save
 * states; geometry and sampling are exercised by native model tests. */
#include <SDL3/SDL.h>
#include "touch_pad_draw.hpp"
#include "../gx/ui/launcher_state.hpp"
#include "mp6_console.h"
#include "mp6_host_section.h"

extern "C" void mp6_launcher_toggle_menu(void);
extern "C" int mp6_launcher_menu_visible(void);

using namespace mp6::touch;
static Model g_pad;
static bool g_edit = false, g_beginEdit = false, g_blocked = false;
static Mp6TouchConfig g_beforeEdit;
static float g_offsetX = 0, g_offsetY = 0, g_windowW = 0, g_windowH = 0;
static int g_selected = -1;
struct HostFinger
{
    bool active = false;
    int64_t device = 0, id = 0;
    int action = -1; // 0 gear, 1 console, 2 save, 3 cancel, 4 reset, 5 drag
    float dx = 0, dy = 0;
};
static HostFinger g_hostFingers[16];

extern "C" void mp6_touch_pad_savestate_reset(void)
{
    g_pad.reset();
    for (auto &f : g_hostFingers)
        f = {};
    g_selected = -1;
}
extern "C" int mp6_touch_pad_editing(void) { return g_edit || g_beginEdit; }
extern "C" void mp6_touch_pad_begin_edit(void) { g_beginEdit = true; }
static bool blocked() { return mp6_launcher_menu_visible() || mp6_console_is_open(); }
static void sync()
{
    bool b = blocked();
    if (b != g_blocked)
    {
        mp6_touch_pad_savestate_reset();
        g_blocked = b;
    }
    if (g_pad.layout.w > 0)
        g_pad.configure(g_pad.layout.w, g_pad.layout.h, mp6::ui::cfg().touch);
}
static Rect toolbar(int action)
{
    float u = g_pad.layout.unit, w = g_pad.layout.w;
    float rx = std::max(28.f, u * .075f), ry = std::max(22.f, u * .035f);
    return {w * .5f + (action - 3) * (rx * 2 + 12), ry + 12, rx, ry};
}
static void finish_edit(bool save)
{
    if (!save)
        mp6::ui::cfg().touch = g_beforeEdit;
    else
        mp6::ui::cfg_save();
    g_edit = false;
    mp6_touch_pad_savestate_reset();
    if (!mp6_launcher_menu_visible())
        mp6_launcher_toggle_menu();
}
static int host_hit(float x, float y)
{
    if (g_edit)
    {
        for (int a = 2; a <= 4; ++a)
            if (inside(toolbar(a), x, y))
                return a;
        return -1;
    }
    if (inside(g_pad.layout.menu, x, y))
        return 0;
    if ((!mp6_launcher_menu_visible() || mp6_console_is_open()) && inside(g_pad.layout.console, x, y))
        return 1;
    return -1;
}
extern "C" int mp6_touch_pad_control_at(float nx, float ny)
{
    if (g_edit || g_beginEdit)
        return 1;
    if (g_pad.layout.w <= 0)
        return 0;
    const float x = nx * g_windowW - g_offsetX, y = ny * g_windowH - g_offsetY;
    return host_hit(x, y) >= 0 || (!blocked() && mp6::ui::cfg().touch.enabled &&
                                   g_pad.layout.hit(x, y, mp6::ui::cfg().touch.visible) >= 0);
}
extern "C" void mp6_touch_pad_event(const SDL_Event *event)
{
    if (!event)
        return;
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST || event->type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
        event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    {
        // A canceled drag should not become a saved edit after resuming.
        if (g_edit &&
            (event->type == SDL_EVENT_WINDOW_FOCUS_LOST || event->type == SDL_EVENT_WILL_ENTER_BACKGROUND))
            finish_edit(false);
        mp6_touch_pad_savestate_reset();
        if (event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        {
            g_windowW = g_windowH = 0;
            g_pad.layout.w = g_pad.layout.h = 0;
        }
        return;
    }
    if (event->type != SDL_EVENT_FINGER_DOWN && event->type != SDL_EVENT_FINGER_UP &&
        event->type != SDL_EVENT_FINGER_MOTION && event->type != SDL_EVENT_FINGER_CANCELED)
        return;
    sync();
    if (g_pad.layout.w <= 0)
        return;
    const auto &t = event->tfinger;
    const float x = t.x * g_windowW - g_offsetX, y = t.y * g_windowH - g_offsetY;
    if (!std::isfinite(x) || !std::isfinite(y))
    {
        mp6_touch_pad_savestate_reset();
        return;
    }
    HostFinger *host = nullptr;
    for (auto &f : g_hostFingers)
        if (f.active && f.device == t.touchID && f.id == t.fingerID)
        {
            host = &f;
            break;
        }
    if (event->type == SDL_EVENT_FINGER_DOWN)
    {
        if (host)
            return;
        int action = host_hit(x, y), selected = -1;
        if (g_edit && action < 0 && g_selected < 0)
        {
            selected = g_pad.layout.hit(x, y, mp6::ui::cfg().touch.visible);
            if (selected >= 0)
                action = 5;
        }
        if (action >= 0)
        {
            for (auto &f : g_hostFingers)
                if (!f.active)
                {
                    f = {};
                    f.active = true;
                    f.device = t.touchID;
                    f.id = t.fingerID;
                    f.action = action;
                    if (action == 5)
                    {
                        g_selected = selected;
                        auto &r = g_pad.layout.controls[selected];
                        f.dx = x - r.x;
                        f.dy = y - r.y;
                    }
                    break;
                }
        }
        else if (!g_edit && !g_beginEdit && !g_blocked)
            g_pad.down(t.touchID, t.fingerID, x, y);
    }
    else if (event->type == SDL_EVENT_FINGER_MOTION)
    {
        if (host && host->action == 5 && g_selected >= 0)
        {
            auto &c = mp6::ui::cfg().touch;
            auto &r = g_pad.layout.controls[g_selected];
            float cx = clamp(x - host->dx, r.rx + 8, g_pad.layout.w - r.rx - 8);
            float cy = clamp(y - host->dy, r.ry + 8, g_pad.layout.h - r.ry - 8);
            c.x[g_selected] = (int)std::lround(cx / g_pad.layout.w * 10000);
            c.y[g_selected] = (int)std::lround(cy / g_pad.layout.h * 10000);
        }
        else if (!g_edit && !g_blocked)
            g_pad.move(t.touchID, t.fingerID, x, y);
    }
    else
    {
        bool canceled = event->type == SDL_EVENT_FINGER_CANCELED;
        if (host)
        {
            int action = host->action;
            *host = {};
            if (action == 5)
                g_selected = -1;
            if (!canceled && host_hit(x, y) == action)
            {
                if (action == 0) {
                    mp6_console_close();
                    mp6_launcher_toggle_menu();
                } else if (action == 1) {
                    if (mp6_console_is_open()) mp6_console_close();
                    else mp6_console_open();
                }
                else if (action == 2 || action == 3)
                    finish_edit(action == 2);
                else if (action == 4)
                {
                    auto &c = mp6::ui::cfg().touch;
                    for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
                        c.x[i] = c.y[i] = -1;
                    mp6_touch_pad_savestate_reset();
                }
            }
        }
        else
            g_pad.up(t.touchID, t.fingerID, x, y, canceled);
    }
}
extern "C" void mp6_touch_pad_collect(Mp6TouchState *out)
{
    if (!out)
        return;
    sync();
    if (g_edit || g_beginEdit || g_blocked)
    {
        g_pad.reset();
        *out = {};
        return;
    }
    *out = g_pad.collect();
}
extern "C" void mp6_touch_pad_draw(void)
{
    auto size = ImGui::GetIO().DisplaySize;
    if (size.x <= 0 || size.y <= 0)
        return;
    g_windowW = size.x;
    g_windowH = size.y;
    float w = size.x, h = size.y, ox = 0, oy = 0;
    // Cutouts/system bars: SDL window coordinates can differ from ImGui points.
    if (auto *window = SDL_GetKeyboardFocus())
    {
        SDL_Rect safe;
        int ww = 0, wh = 0;
        if (SDL_GetWindowSize(window, &ww, &wh) && ww > 0 && wh > 0 && SDL_GetWindowSafeArea(window, &safe) &&
            safe.w > 0 && safe.h > 0)
        {
            ox = safe.x * size.x / ww;
            oy = safe.y * size.y / wh;
            w = safe.w * size.x / ww;
            h = safe.h * size.y / wh;
        }
    }
    if (ox != g_offsetX || oy != g_offsetY)
        mp6_touch_pad_savestate_reset();
    g_offsetX = ox;
    g_offsetY = oy;
    sync();
    if (g_beginEdit)
    {
        g_beginEdit = false;
        g_edit = true;
        g_beforeEdit = mp6::ui::cfg().touch;
        if (mp6_launcher_menu_visible())
            mp6_launcher_toggle_menu();
        mp6_touch_pad_savestate_reset();
    }
    if (g_edit && blocked())
        finish_edit(false);
    g_pad.configure(w, h, mp6::ui::cfg().touch);
    auto *dl = ImGui::GetForegroundDrawList();
    const int firstVertex = dl->VtxBuffer.Size;
    const auto state = g_pad.held();
    if (g_edit)
    {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), color(10, 15, 24, .30f));
        for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
            if (g_pad.config.visible & (1u << i))
                draw_control(dl, g_pad, i, {}, true, i == g_selected);
        const char *names[] = {"Save", "Cancel", "Reset"};
        for (int a = 2; a <= 4; ++a)
        {
            auto r = toolbar(a);
            pill(dl, r, color(31, 44, 67, .97f), color(231, 239, 255, .9f));
            label(dl, r.x, r.y, std::max(16.f, g_pad.layout.unit * .029f), names[a - 2]);
        }
        label(dl, w * .5f, toolbar(2).y + toolbar(2).ry + 24, std::max(15.f, g_pad.layout.unit * .025f),
              "Drag controls to move them");
    }
    else
    {
        draw_system(dl, g_pad.layout, false);
        if (!mp6_launcher_menu_visible() || mp6_console_is_open())
            draw_system(dl, g_pad.layout, true, mp6_console_is_open() != 0);
        if (!mp6_launcher_menu_visible())
        {
            if (!blocked() && g_pad.config.enabled)
                for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
                    if (g_pad.config.visible & (1u << i))
                        draw_control(dl, g_pad, i, state);
        }
    }
    for (int i = firstVertex; i < dl->VtxBuffer.Size; ++i)
    {
        dl->VtxBuffer[i].pos.x += ox;
        dl->VtxBuffer[i].pos.y += oy;
    }
}
