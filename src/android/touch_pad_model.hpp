/* Pure controller geometry/input, also exercised on the host in native tests.
 * No SDL, ImGui, game globals, allocation, or frame-rate dependence. */
#pragma once
#include "mp6_touch_config.hpp"
#include <dolphin/pad.h>
#include <cmath>
#include <algorithm>

namespace mp6::touch
{
struct Rect
{
    float x = 0, y = 0, rx = 0, ry = 0;
};
inline float clamp(float v, float a, float b) { return std::max(a, std::min(v, b)); }
inline bool inside(const Rect &r, float x, float y, float scale = 1)
{
    return std::abs(x - r.x) <= r.rx * scale && std::abs(y - r.y) <= r.ry * scale;
}
inline unsigned button(int i)
{
    const unsigned bits[] = {0,
                             0,
                             0,
                             PAD_BUTTON_A,
                             PAD_BUTTON_B,
                             PAD_BUTTON_X,
                             PAD_BUTTON_Y,
                             PAD_TRIGGER_Z,
                             PAD_TRIGGER_L,
                             PAD_TRIGGER_R,
                             PAD_BUTTON_START};
    return i >= 0 && i < MP6_TOUCH_COUNT ? bits[i] : 0;
}
struct Layout
{
    float w = 0, h = 0, unit = 0;
    Rect controls[MP6_TOUCH_COUNT], menu, console;
    void make(float width, float height, const Mp6TouchConfig &c)
    {
        w = width;
        h = height;
        unit = std::min(h, w / 1.5f);
        const float u = unit, scale = clamp((float)c.size, 75, 150) / 100;
        auto put = [&](int i, float x, float y, float rx, float ry)
        {
            // Visible button targets are at least 44 display points across.
            rx = std::max(22.0f, rx * u * scale);
            ry = std::max(22.0f, ry * u * scale);
            if (i == MP6_TOUCH_L || i == MP6_TOUCH_R)
                ry = std::max(36.0f, ry);
            if (c.x[i] >= 0)
            {
                x = w * c.x[i] / 10000;
                y = h * c.y[i] / 10000;
            }
            controls[i] = {clamp(x, rx + 8, w - rx - 8), clamp(y, ry + 8, h - ry - 8), rx, ry};
        };
        put(MP6_TOUCH_STICK, .18f * u, h - .37f * u, .115f, .115f);
        put(MP6_TOUCH_CSTICK, w - .42f * u, h - .135f * u, .085f, .085f);
        put(MP6_TOUCH_DPAD, .42f * u, h - .135f * u, .085f, .085f);
        put(MP6_TOUCH_A, w - .14f * u, h - .37f * u, .067f, .067f);
        put(MP6_TOUCH_B, w - .29f * u, h - .28f * u, .045f, .045f);
        put(MP6_TOUCH_X, w - .085f * u, h - .555f * u, .040f, .065f);
        put(MP6_TOUCH_Y, w - .255f * u, h - .56f * u, .065f, .040f);
        put(MP6_TOUCH_Z, w - .405f * u, h - .72f * u, .065f, .034f);
        put(MP6_TOUCH_L, .18f * u, h - .76f * u, .100f, .065f);
        put(MP6_TOUCH_R, w - .17f * u, h - .79f * u, .100f, .065f);
        put(MP6_TOUCH_START, w * .5f, h - .075f * u, .070f, .034f);
        if (h > w)
        {
            // Portrait needs two rows across the bottom, not a compressed
            // landscape cluster with overlapping X/Y and tiny directional pads.
            unit = std::min(w, h * .6f);
            const float p = unit;
            auto portrait = [&](int i, float x, float y, float rx, float ry)
            { put(i, x * p, h - y * p, rx * p / u, ry * p / u); };
            portrait(MP6_TOUCH_STICK, .20f, .50f, .15f, .15f);
            portrait(MP6_TOUCH_DPAD, .20f, .17f, .14f, .14f);
            portrait(MP6_TOUCH_CSTICK, .72f, .17f, .11f, .11f);
            portrait(MP6_TOUCH_A, .88f, .48f, .075f, .075f);
            portrait(MP6_TOUCH_B, .71f, .32f, .06f, .06f);
            portrait(MP6_TOUCH_X, .90f, .72f, .05f, .07f);
            portrait(MP6_TOUCH_Y, .69f, .64f, .07f, .05f);
            portrait(MP6_TOUCH_Z, .60f, .84f, .07f, .05f);
            portrait(MP6_TOUCH_L, .20f, .99f, .13f, .075f);
            portrait(MP6_TOUCH_R, .80f, .99f, .13f, .075f);
            portrait(MP6_TOUCH_START, .50f, .075f, .07f, .035f);
        }
        const float r = std::max(22.0f, unit * .041f);
        menu = {r + 12, r + 12, r, r};
        console = {3 * r + 24, r + 12, r, r};
    }
    int hit(float x, float y, unsigned visible) const
    {
        if (w <= 0 || h <= 0)
            return -1;
        // Closest normalized center wins if enlarged/custom targets overlap.
        int best = -1;
        float score = 100;
        for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
        {
            const auto &r = controls[i];
            if (!(visible & (1u << i)) || !inside(r, x, y, 1.08f))
                continue;
            float dx = (x - r.x) / r.rx, dy = (y - r.y) / r.ry, d = dx * dx + dy * dy;
            if (d < score)
            {
                best = i;
                score = d;
            }
        }
        return best;
    }
};
struct Finger
{
    bool active = false;
    int64_t device = 0, id = 0;
    int control = -1;
    float x = 0, y = 0, originX = 0, originY = 0;
    Mp6TouchState pending = {};
};
inline void merge(Mp6TouchState &to, const Mp6TouchState &from)
{
    to.buttons |= from.buttons;
    if (!to.stickX && !to.stickY)
    {
        to.stickX = from.stickX;
        to.stickY = from.stickY;
    }
    if (!to.cstickX && !to.cstickY)
    {
        to.cstickX = from.cstickX;
        to.cstickY = from.cstickY;
    }
    to.triggerL = std::max(to.triggerL, from.triggerL);
    to.triggerR = std::max(to.triggerR, from.triggerR);
}
struct Model
{
    Layout layout;
    Mp6TouchConfig config = mp6_touch_defaults();
    Finger fingers[16] = {};
    Mp6TouchState released = {};
    void reset()
    {
        for (auto &f : fingers)
            f = {};
        released = {};
    }
    void configure(float w, float h, const Mp6TouchConfig &c)
    {
        if (w != layout.w || h != layout.h || std::memcmp(&config, &c, sizeof(c)))
            reset();
        config = c;
        layout.make(w, h, c);
    }
    Finger *find(int64_t device, int64_t id)
    {
        for (auto &f : fingers)
            if (f.active && f.device == device && f.id == id)
                return &f;
        return nullptr;
    }
    Mp6TouchState sample(const Finger &f) const
    {
        Mp6TouchState out = {};
        const auto &r = layout.controls[f.control];
        if (f.control == MP6_TOUCH_STICK || f.control == MP6_TOUCH_CSTICK)
        {
            float dx = (f.x - f.originX) / r.rx, dy = (f.originY - f.y) / r.ry;
            float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > .15f)
            {
                float gain = clamp((distance - .15f) / .85f, 0, 1) / distance;
                const int range = f.control == MP6_TOUCH_STICK ? 72 : 59;
                int8_t x = (int8_t)std::lround(dx * gain * range), y = (int8_t)std::lround(dy * gain * range);
                if (f.control == MP6_TOUCH_STICK)
                {
                    out.stickX = x;
                    out.stickY = y;
                }
                else
                {
                    out.cstickX = x;
                    out.cstickY = y;
                }
            }
        }
        else if (f.control == MP6_TOUCH_DPAD)
        {
            float dx = (f.x - r.x) / r.rx, dy = (r.y - f.y) / r.ry;
            if (std::max(std::abs(dx), std::abs(dy)) > .25f)
            {
                if (std::abs(dx) >= .4142f * std::abs(dy))
                    out.buttons |= dx > 0 ? PAD_BUTTON_RIGHT : PAD_BUTTON_LEFT;
                if (std::abs(dy) >= .4142f * std::abs(dx))
                    out.buttons |= dy > 0 ? PAD_BUTTON_UP : PAD_BUTTON_DOWN;
            }
        }
        else if (f.control == MP6_TOUCH_L || f.control == MP6_TOUCH_R)
        {
            unsigned value = inside(r, f.x, f.y, 1.4f) ? 255 : 0;
            if (value >= 245)
            {
                value = 255;
                out.buttons |= button(f.control);
            }
            if (f.control == MP6_TOUCH_L)
                out.triggerL = (uint8_t)value;
            else
                out.triggerR = (uint8_t)value;
        }
        else if (inside(r, f.x, f.y, 1.4f))
            out.buttons |= button(f.control);
        return out;
    }
    bool down(int64_t device, int64_t id, float x, float y)
    {
        if (!config.enabled || find(device, id))
            return false;
        int i = layout.hit(x, y, config.visible);
        if (i < 0)
            return false;
        // Only one thumb owns a stick/D-pad/trigger; buttons may have two.
        for (auto &f : fingers)
            if (f.active && f.control == i && (i <= MP6_TOUCH_DPAD || i == MP6_TOUCH_L || i == MP6_TOUCH_R))
                return false;
        for (auto &f : fingers)
            if (!f.active)
            {
                f = {};
                f.active = true;
                f.device = device;
                f.id = id;
                f.control = i;
                f.x = x;
                f.y = y;
                const auto &r = layout.controls[i];
                f.originX = config.floating ? x : r.x;
                f.originY = config.floating ? y : r.y;
                f.pending = sample(f);
                return true;
            }
        return false;
    }
    void move(int64_t device, int64_t id, float x, float y)
    {
        if (auto *f = find(device, id))
        {
            f->x = x;
            f->y = y;
        }
    }
    void up(int64_t device, int64_t id, float x, float y, bool canceled = false)
    {
        if (auto *f = find(device, id))
        {
            f->x = x;
            f->y = y;
            if (!canceled)
            {
                merge(released, f->pending);
                merge(released, sample(*f));
            }
            *f = {};
        }
    }
    Mp6TouchState held() const
    {
        Mp6TouchState out = {};
        if (config.enabled)
            for (const auto &f : fingers)
                if (f.active)
                    merge(out, sample(f));
        return out;
    }
    Mp6TouchState collect()
    {
        Mp6TouchState out = held();
        merge(out, released);
        released = {};
        for (auto &f : fingers)
            f.pending = {};
        return out;
    }
};
} // namespace mp6::touch
