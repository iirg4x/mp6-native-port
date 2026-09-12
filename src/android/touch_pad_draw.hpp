/* Code-native GameCube skin, shared with the off-device visual test. */
#pragma once
#include "touch_pad_model.hpp"
#include <imgui.h>
namespace mp6::touch
{
inline ImU32 color(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(clamp(a, 0, 1) * 255)); }
inline void label(ImDrawList *dl, float x, float y, float size, const char *text, float alpha = 1)
{
    auto *font = ImGui::GetFont();
    auto bounds = font->CalcTextSizeA(size, 10000, 0, text);
    ImVec2 p(x - bounds.x * .5f, y - bounds.y * .5f);
    dl->AddText(font, size, ImVec2(p.x + 1, p.y + 1), color(0, 0, 0, alpha * .85f), text);
    dl->AddText(font, size, p, color(250, 251, 255, alpha), text);
}
inline void pill(ImDrawList *dl, const Rect &r, ImU32 fill, ImU32 line)
{
    dl->AddRectFilled(ImVec2(r.x - r.rx, r.y - r.ry), ImVec2(r.x + r.rx, r.y + r.ry), fill, r.ry * .75f);
    dl->AddRect(ImVec2(r.x - r.rx, r.y - r.ry), ImVec2(r.x + r.rx, r.y + r.ry), line, r.ry * .75f, 0, 2);
}
inline void draw_control(ImDrawList *dl, const Model &model, int i, const Mp6TouchState &state,
                         bool editing = false, bool selected = false)
{
    const auto &r = model.layout.controls[i];
    bool active = (state.buttons & button(i)) != 0;
    float opacity = editing ? .85f : model.config.opacity / 100.0f;
    int red = 172, green = 184, blue = 205;
    if (i == MP6_TOUCH_A)
    {
        red = 48;
        green = 196;
        blue = 127;
    }
    if (i == MP6_TOUCH_B)
    {
        red = 232;
        green = 79;
        blue = 84;
    }
    if (i == MP6_TOUCH_CSTICK)
    {
        red = 236;
        green = 189;
        blue = 63;
    }
    if (i == MP6_TOUCH_Z)
    {
        red = 153;
        green = 129;
        blue = 234;
    }
    ImU32 rim = color(230, 237, 250, opacity),
          fill = color(red, green, blue, opacity * (active ? .88f : .30f));
    const float textSize = std::max(16.0f, model.layout.unit * .034f);
    if (i == MP6_TOUCH_STICK || i == MP6_TOUCH_CSTICK)
    {
        float cx = r.x, cy = r.y;
        for (const auto &f : model.fingers)
            if (f.active && f.control == i)
            {
                cx = f.originX;
                cy = f.originY;
                break;
            }
        const int range = i == MP6_TOUCH_STICK ? 72 : 59;
        const float sx = (i == MP6_TOUCH_STICK ? state.stickX : state.cstickX) / (float)range;
        const float sy = (i == MP6_TOUCH_STICK ? state.stickY : state.cstickY) / (float)range;
        dl->AddCircleFilled(ImVec2(cx, cy), r.rx, color(25, 31, 44, opacity * .24f), 48);
        dl->AddCircle(ImVec2(cx, cy), r.rx, rim, 8, 2);
        dl->AddCircle(ImVec2(cx, cy), r.rx * .72f, color(red, green, blue, opacity * .35f), 48, 1);
        if (i == MP6_TOUCH_STICK)
        {
            dl->AddLine(ImVec2(cx - r.rx * .25f, cy), ImVec2(cx + r.rx * .25f, cy), rim, 1);
            dl->AddLine(ImVec2(cx, cy - r.ry * .25f), ImVec2(cx, cy + r.ry * .25f), rim, 1);
        }
        float x = cx + sx * r.rx * .58f, y = cy - sy * r.ry * .58f;
        dl->AddCircleFilled(ImVec2(x, y), r.rx * .38f,
                            color(red, green, blue, opacity * ((sx || sy) ? .9f : .5f)), 32);
        dl->AddCircle(ImVec2(x, y), r.rx * .38f, rim, 32, 2);
        label(dl, x, y, textSize, i == MP6_TOUCH_STICK ? "" : "C", opacity);
    }
    else if (i == MP6_TOUCH_DPAD)
    {
        const unsigned directions[] = {PAD_BUTTON_UP, PAD_BUTTON_RIGHT, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT};
        const float dx[] = {0, .63f, 0, -.63f}, dy[] = {-.63f, 0, .63f, 0};
        for (int d = 0; d < 4; ++d)
        {
            Rect arm = {r.x + dx[d] * r.rx, r.y + dy[d] * r.ry, r.rx * (d % 2 ? .37f : .30f),
                        r.ry * (d % 2 ? .30f : .37f)};
            bool pressed = (state.buttons & directions[d]) != 0;
            pill(dl, arm, color(red, green, blue, opacity * (pressed ? .90f : .30f)), rim);
            float ax = arm.x, ay = arm.y, s = r.rx * .18f;
            ImVec2 a(ax + dx[d] * s, ay + dy[d] * s),
                b(ax - dx[d] * s - dy[d] * s, ay - dy[d] * s + dx[d] * s),
                c(ax - dx[d] * s + dy[d] * s, ay - dy[d] * s - dx[d] * s);
            dl->AddTriangleFilled(a, b, c, rim);
        }
        dl->AddRectFilled(ImVec2(r.x - r.rx * .29f, r.y - r.ry * .29f),
                          ImVec2(r.x + r.rx * .29f, r.y + r.ry * .29f),
                          color(red, green, blue, opacity * .3f), 2);
    }
    else if (i == MP6_TOUCH_L || i == MP6_TOUCH_R)
    {
        pill(dl, r, fill, rim);
        label(dl, r.x, r.y, textSize, mp6_touch_name(i), opacity);
    }
    else
    {
        if (i == MP6_TOUCH_A || i == MP6_TOUCH_B)
        {
            dl->AddCircleFilled(ImVec2(r.x, r.y), r.rx, fill, 48);
            dl->AddCircle(ImVec2(r.x, r.y), r.rx, rim, 48, 2);
        }
        else
            pill(dl, r, fill, rim);
        label(dl, r.x, r.y, textSize * (i == MP6_TOUCH_START ? .74f : 1), mp6_touch_name(i), opacity);
    }
    if (editing)
        dl->AddRect(ImVec2(r.x - r.rx - 4, r.y - r.ry - 4), ImVec2(r.x + r.rx + 4, r.y + r.ry + 4),
                    color(selected ? 255 : 160, selected ? 208 : 177, selected ? 86 : 202, .85f), 4, 0,
                    selected ? 3 : 1);
}
inline void draw_system(ImDrawList *dl, const Layout &layout, bool console, bool open = false)
{
    auto r = console ? layout.console : layout.menu;
    dl->AddCircleFilled(ImVec2(r.x, r.y), r.rx, color(25, 31, 44, .35f), 32);
    dl->AddCircle(ImVec2(r.x, r.y), r.rx, color(235, 240, 255, .65f), 32, 1.5f);
    if (console)
        label(dl, r.x, r.y, r.rx * .7f, open ? "X" : ">_", .9f);
    else
    {
        dl->AddCircle(ImVec2(r.x, r.y), r.rx * .46f, color(235, 240, 255, .85f), 24, 3);
        for (int i = 0; i < 8; ++i)
        {
            float a = i * 3.14159265f / 4, x = std::cos(a), y = std::sin(a);
            dl->AddLine(ImVec2(r.x + x * r.rx * .46f, r.y + y * r.ry * .46f),
                        ImVec2(r.x + x * r.rx * .7f, r.y + y * r.ry * .7f), color(235, 240, 255, .85f), 4);
        }
    }
}
} // namespace mp6::touch
