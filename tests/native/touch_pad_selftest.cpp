/* Drives the real SDL adapter, layout editor and renderer without a device. */
#include "../../src/android/touch_pad.cpp"
#include <cassert>
#include <vector>
#include <string>
#include <fstream>

static Mp6LauncherConfig testConfig = {};
static bool testMenu = false, testConsole = false;
static int saves = 0;
static bool useSafeArea = false;
namespace mp6::ui
{
Mp6LauncherConfig &cfg() { return testConfig; }
void cfg_save() { ++saves; }
} // namespace mp6::ui
extern "C" void mp6_launcher_toggle_menu() { testMenu = !testMenu; }
extern "C" int mp6_launcher_menu_visible() { return testMenu; }
extern "C" int mp6_console_is_open() { return testConsole; }
extern "C" void mp6_console_toggle() { testConsole = !testConsole; }
extern "C" void mp6_console_open() { testConsole = true; }
extern "C" void mp6_console_close() { testConsole = false; }
extern "C" SDL_Window *SDL_GetKeyboardFocus() { return useSafeArea ? (SDL_Window *)1 : nullptr; }
extern "C" bool SDL_GetWindowSize(SDL_Window *, int *w, int *h) { *w=640; *h=360; return true; }
extern "C" bool SDL_GetWindowSafeArea(SDL_Window *, SDL_Rect *r) { *r={20,10,600,340}; return true; }

static void frame(float w = 1280, float h = 720)
{
    ImGui::GetIO().DisplaySize = {w, h};
    ImGui::NewFrame();
    mp6_touch_pad_draw();
    ImGui::Render();
}
static Mp6TouchState poll()
{
    Mp6TouchState s;
    mp6_touch_pad_collect(&s);
    return s;
}
static bool neutral(const Mp6TouchState &s)
{
    return !s.buttons && !s.stickX && !s.stickY && !s.cstickX && !s.cstickY && !s.triggerL && !s.triggerR;
}
static void touch(Uint32 type, int id, float x, float y, int device = 1)
{
    SDL_Event ev = {};
    ev.type = type;
    ev.tfinger.touchID = device;
    ev.tfinger.fingerID = id;
    ev.tfinger.x = x / g_windowW;
    ev.tfinger.y = y / g_windowH;
    mp6_touch_pad_event(&ev);
}
static void tap(float x, float y)
{
    touch(SDL_EVENT_FINGER_DOWN, 14, x, y);
    touch(SDL_EVENT_FINGER_UP, 14, x, y);
}
static void clean()
{
    mp6_touch_pad_savestate_reset();
    testMenu = testConsole = false;
    g_edit = g_beginEdit = false;
    testConfig.touch = mp6_touch_defaults();
    frame();
    poll();
}

static void test_model()
{
    Model m;
    auto c = mp6_touch_defaults();
    m.configure(1280, 720, c);
    for (int i = MP6_TOUCH_A; i < MP6_TOUCH_COUNT; ++i)
    {
        auto r = m.layout.controls[i];
        float y = r.y - (i == MP6_TOUCH_L || i == MP6_TOUCH_R ? r.ry * .4f : 0);
        assert(m.down(1, i, r.x, y));
        auto s = m.collect();
        assert(s.buttons == button(i));
        if (i == MP6_TOUCH_L)
            assert(s.triggerL == 255);
        if (i == MP6_TOUCH_R)
            assert(s.triggerR == 255);
        m.up(1, i, r.x, y);
        m.collect();
        assert(neutral(m.collect()));
    }
    // Independent main and C sticks, digital D-pad, and five buttons at once.
    auto l = m.layout.controls[MP6_TOUCH_STICK], r = m.layout.controls[MP6_TOUCH_CSTICK],
         d = m.layout.controls[MP6_TOUCH_DPAD];
    m.down(1, 1, l.x, l.y);
    m.move(1, 1, l.x + l.rx * 4, l.y);
    m.down(1, 2, r.x, r.y);
    m.move(1, 2, r.x, r.y - r.ry * 4);
    m.down(1, 3, d.x + d.rx * .8f, d.y - d.ry * .8f);
    for (int i = MP6_TOUCH_A; i <= MP6_TOUCH_Z; ++i)
    {
        auto b = m.layout.controls[i];
        m.down(1, i + 10, b.x, b.y);
    }
    auto s = m.collect();
    assert(s.stickX == 72 && s.stickY == 0 && s.cstickX == 0 && s.cstickY == 59);
    assert((s.buttons & (PAD_BUTTON_UP | PAD_BUTTON_RIGHT)) == (PAD_BUTTON_UP | PAD_BUTTON_RIGHT));
    for (int i = MP6_TOUCH_A; i <= MP6_TOUCH_Z; ++i)
        assert(s.buttons & button(i));
    m.reset();
    // Circular gate, neutral dead zone, and no digital directions from sticks.
    m.down(1, 1, l.x, l.y);
    m.move(1, 1, l.x + l.rx * .1f, l.y);
    assert(neutral(m.collect()));
    m.move(1, 1, l.x + l.rx * 3, l.y - l.ry * 3);
    s = m.collect();
    assert(!s.buttons && s.stickX == 51 && s.stickY == 51);
    m.reset();
    m.down(1, 1, d.x, d.y - d.ry * .8f);
    s = m.collect();
    assert(s.buttons == PAD_BUTTON_UP && !s.stickY);
    m.reset();
    // Every point on L/R is a full press, with no invisible slider zone.
    for (int control : {MP6_TOUCH_L, MP6_TOUCH_R})
    {
        auto t = m.layout.controls[control];
        m.down(1, 1, t.x, t.y + t.ry * .45f);
        s = m.collect();
        assert(s.buttons == button(control) && (control == MP6_TOUCH_L ? s.triggerL : s.triggerR) == 255);
        m.move(1, 1, t.x - t.rx, t.y);
        assert(m.collect().buttons == button(control));
        m.move(1, 1, t.x + t.rx, t.y);
        s = m.collect();
        assert(s.buttons == button(control));
        m.move(1, 1, t.x + t.rx * 2, t.y);
        assert(neutral(m.collect()));
        m.reset();
    }
    // Same numeric finger IDs on different touch devices must not collide.
    auto a = m.layout.controls[MP6_TOUCH_A], b = m.layout.controls[MP6_TOUCH_B];
    assert(m.down(1, 1, a.x, a.y));
    assert(m.down(2, 1, b.x, b.y));
    s = m.collect();
    assert(s.buttons == (PAD_BUTTON_A | PAD_BUTTON_B));
    m.reset();
    // Fast tap is delivered once; cancellation is never a tap.
    m.down(1, 1, a.x, a.y);
    m.up(1, 1, a.x, a.y);
    assert(m.collect().buttons == PAD_BUTTON_A);
    assert(neutral(m.collect()));
    m.down(1, 1, a.x, a.y);
    m.up(1, 1, a.x, a.y, true);
    assert(neutral(m.collect()));
    m.down(1, 1, a.x, a.y);
    m.collect();
    m.move(1, 1, 0, 0);
    assert(neutral(m.collect()));
    m.reset();
    // Floating origins are per stick, not one shared center.
    c.floating = 1;
    m.configure(1280, 720, c);
    m.down(1, 1, l.x + l.rx * .3f, l.y);
    assert(neutral(m.collect()));
    m.move(1, 1, l.x + l.rx * .8f, l.y);
    s = m.collect();
    assert(s.stickX > 25 && s.stickX < 35);
    m.reset();
    c.visible &= ~(1u << MP6_TOUCH_A);
    m.configure(1280, 720, c);
    assert(!m.down(1, 1, a.x, a.y));
    c.enabled = 0;
    m.configure(1280, 720, c);
    assert(!m.down(1, 1, b.x, b.y));
    assert(neutral(m.collect()));
    c = mp6_touch_defaults();
    m.configure(1280, 720, c);
    m.down(1, 1, a.x, a.y);
    m.configure(720, 1280, c);
    assert(neutral(m.collect()));
}
static void test_codec_and_layout()
{
    auto c = mp6_touch_defaults();
    char text[256];
    mp6_touch_layout_write(c, text);
    auto parsed = c;
    assert(mp6_touch_layout_read(text, parsed));
    assert(!memcmp(&c, &parsed, sizeof(c)));
    c.x[3] = 0;
    c.y[3] = 10000;
    c.visible = 0;
    mp6_touch_layout_write(c, text);
    assert(mp6_touch_layout_read(text, parsed));
    assert(!memcmp(&c, &parsed, sizeof(c)));
    for (auto bad : {"", "1;9999999999999999999;", "2;0;", "1;-1;", "1;0;nan,2;"})
        assert(!mp6_touch_layout_read(bad, parsed));
    auto before = parsed;
    std::string tail = std::string(text) + "junk";
    assert(!mp6_touch_layout_read(tail.c_str(), parsed));
    assert(!memcmp(&before, &parsed, sizeof(parsed)));
    for (auto size : {ImVec2(640, 480), ImVec2(800, 360), ImVec2(1280, 720), ImVec2(2340, 1080),
                      ImVec2(1024, 768), ImVec2(360, 800)})
    {
        Layout layout;
        c = mp6_touch_defaults();
        layout.make(size.x, size.y, c);
        for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
        {
            auto r = layout.controls[i];
            assert(r.x - r.rx >= 0 && r.y - r.ry >= 0 && r.x + r.rx <= size.x && r.y + r.ry <= size.y);
            assert(r.rx >= 22 && r.ry >= 22);
            assert(layout.hit(r.x, r.y, c.visible) == i);
        }
    }
}
static void test_adapter()
{
    clean();
    useSafeArea=true;frame();
    assert(g_offsetX==40 && g_offsetY==20 && g_pad.layout.w==1200 && g_pad.layout.h==680);
    auto safeA=g_pad.layout.controls[MP6_TOUCH_A];
    tap(safeA.x+g_offsetX,safeA.y+g_offsetY);assert(poll().buttons==PAD_BUTTON_A);
    useSafeArea=false;frame();assert(neutral(poll()));
    auto a = g_pad.layout.controls[MP6_TOUCH_A];
    tap(a.x, a.y);
    assert(poll().buttons == PAD_BUTTON_A);
    assert(neutral(poll()));
    touch(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
    SDL_Event ev = {};
    ev.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    mp6_touch_pad_event(&ev);
    assert(neutral(poll()));
    touch(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
    mp6_touch_pad_savestate_reset();
    assert(neutral(poll()));
    touch(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
    testMenu = true;
    assert(neutral(poll()));
    testMenu = false;
    assert(neutral(poll()));
    testConsole = true;
    tap(a.x, a.y);
    assert(neutral(poll()));
    testConsole = false;
    poll();
    auto console = g_pad.layout.console;
    tap(console.x, console.y);
    assert(testConsole);
    frame(); poll();
    tap(console.x, console.y);
    assert(!testConsole && neutral(poll()));
    testConsole = testMenu = true;
    frame(); poll();
    assert(mp6_touch_pad_control_at(console.x/g_windowW, console.y/g_windowH));
    tap(console.x, console.y);
    assert(!testConsole && testMenu);
    testConsole = true;
    tap(g_pad.layout.menu.x, g_pad.layout.menu.y);
    assert(!testConsole && !testMenu);
    poll();
    // UP uses its actual coordinates, not the last motion sample.
    auto gear = g_pad.layout.menu;
    touch(SDL_EVENT_FINGER_DOWN, 2, gear.x, gear.y);
    touch(SDL_EVENT_FINGER_UP, 2, 640, 360);
    assert(!testMenu);
    testConfig.touch.enabled = 0;
    frame();
    tap(gear.x, gear.y);
    assert(testMenu);
    poll();
    // Editor input never reaches the game, Cancel is transactional, Save persists.
    mp6_touch_pad_begin_edit();
    frame();
    assert(g_edit && !testMenu);
    assert(neutral(poll()));
    a = g_pad.layout.controls[MP6_TOUCH_A];
    touch(SDL_EVENT_FINGER_DOWN, 4, a.x, a.y);
    touch(SDL_EVENT_FINGER_MOTION, 4, 640, 360);
    frame();
    touch(SDL_EVENT_FINGER_UP, 4, 640, 360);
    assert(testConfig.touch.x[MP6_TOUCH_A] == 5000);
    auto cancel = toolbar(3);
    tap(cancel.x, cancel.y);
    assert(!g_edit && testMenu && testConfig.touch.x[MP6_TOUCH_A] == -1);
    mp6_touch_pad_begin_edit();
    frame();
    poll();
    a = g_pad.layout.controls[MP6_TOUCH_A];
    touch(SDL_EVENT_FINGER_DOWN, 4, a.x, a.y);
    touch(SDL_EVENT_FINGER_MOTION, 4, 640, 360);
    touch(SDL_EVENT_FINGER_UP, 4, 640, 360);
    auto save = toolbar(2);
    tap(save.x, save.y);
    assert(saves == 1 && testConfig.touch.x[MP6_TOUCH_A] == 5000);
    clean();
}

/* Small software rasterizer for the REAL ImGui triangles/font atlas. It is a
 * visual fixture, not an alternative renderer shipped with the game. */
static void screenshot(const char *path, int w, int h)
{
    unsigned char *atlas;
    int aw, ah;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    std::vector<unsigned char> pixels(w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            auto *p = &pixels[(y * w + x) * 3];
            bool band = x < w * .17f || x > w * .83f;
            p[0] = band ? 22 : 45 + y * 25 / h;
            p[1] = band ? 28 : 88 + y * 25 / h;
            p[2] = band ? 40 : 80 + y * 20 / h;
        }
    auto *data = ImGui::GetDrawData();
    for (int list = 0; list < data->CmdListsCount; ++list)
    {
        const auto *dl = data->CmdLists[list];
        for (const auto &cmd : dl->CmdBuffer)
        {
            if (cmd.UserCallback)
                continue;
            for (unsigned t = 0; t + 2 < cmd.ElemCount; t += 3)
            {
                const auto &a = dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset + t] + cmd.VtxOffset];
                const auto &b = dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset + t + 1] + cmd.VtxOffset];
                const auto &c = dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset + t + 2] + cmd.VtxOffset];
                float den =
                    (b.pos.y - c.pos.y) * (a.pos.x - c.pos.x) + (c.pos.x - b.pos.x) * (a.pos.y - c.pos.y);
                if (std::abs(den) < .0001f)
                    continue;
                int x0 = std::max(0, (int)std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))),
                    x1 = std::min(w - 1, (int)std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x})));
                int y0 = std::max(0, (int)std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))),
                    y1 = std::min(h - 1, (int)std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y})));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                    {
                        if (x < cmd.ClipRect.x || y < cmd.ClipRect.y || x >= cmd.ClipRect.z ||
                            y >= cmd.ClipRect.w)
                            continue;
                        float p = ((b.pos.y - c.pos.y) * (x + .5f - c.pos.x) +
                                   (c.pos.x - b.pos.x) * (y + .5f - c.pos.y)) /
                                  den;
                        float q = ((c.pos.y - a.pos.y) * (x + .5f - c.pos.x) +
                                   (a.pos.x - c.pos.x) * (y + .5f - c.pos.y)) /
                                  den,
                              r = 1 - p - q;
                        if (p < 0 || q < 0 || r < 0)
                            continue;
                        int u = (int)clamp((a.uv.x * p + b.uv.x * q + c.uv.x * r) * aw, 0, (float)aw - 1),
                            v = (int)clamp((a.uv.y * p + b.uv.y * q + c.uv.y * r) * ah, 0, (float)ah - 1);
                        auto channel = [&](int shift) {
                            return ((a.col >> shift) & 255) * p + ((b.col >> shift) & 255) * q +
                                   ((c.col >> shift) & 255) * r;
                        };
                        float alpha = channel(24) / 255 * atlas[(v * aw + u) * 4 + 3] / 255;
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            auto &dst = pixels[(y * w + x) * 3 + ch];
                            dst = (unsigned char)(dst * (1 - alpha) + channel(ch * 8) * alpha);
                        }
                    }
            }
        }
    }
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write((char *)pixels.data(), pixels.size());
}
int main(int argc, char **argv)
{
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.f / 60;
    unsigned char *tex;
    int tw, th;
    io.Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);
    test_model();
    test_codec_and_layout();
    test_adapter();
    if (argc == 2)
    {
        for (auto size : {ImVec2(1280, 720), ImVec2(2340, 1080), ImVec2(640, 480), ImVec2(360, 800)})
        {
            clean();
            frame(size.x, size.y);
            std::string name = std::string(argv[1]) + "/touch-" + std::to_string((int)size.x) + "x" +
                               std::to_string((int)size.y) + ".ppm";
            screenshot(name.c_str(), size.x, size.y);
        }
        clean();
        auto mainStick=g_pad.layout.controls[MP6_TOUCH_STICK];
        touch(SDL_EVENT_FINGER_DOWN,1,mainStick.x,mainStick.y);
        touch(SDL_EVENT_FINGER_MOTION,1,mainStick.x+mainStick.rx,mainStick.y-mainStick.ry);
        auto cStick=g_pad.layout.controls[MP6_TOUCH_CSTICK];
        touch(SDL_EVENT_FINGER_DOWN,2,cStick.x,cStick.y);
        touch(SDL_EVENT_FINGER_MOTION,2,cStick.x-cStick.rx,cStick.y);
        for (int i=MP6_TOUCH_A;i<=MP6_TOUCH_R;++i) {
            auto b=g_pad.layout.controls[i];
            touch(SDL_EVENT_FINGER_DOWN,i+1,b.x,b.y-b.ry*.4f);
        }
        frame();screenshot((std::string(argv[1])+"/touch-held.ppm").c_str(),1280,720);
        clean();
        mp6_touch_pad_begin_edit();
        frame();
        screenshot((std::string(argv[1]) + "/touch-editor.ppm").c_str(), 1280, 720);
    }
    ImGui::DestroyContext();
    puts("PASS: full controller, multi-touch, triggers, codec, layout and editor");
}
