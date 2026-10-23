/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
/* [MP6] This is the most-adapted ripped file: partyboard's SettingsWindow
 * skeleton, row-builder helpers, and save-on-change discipline are kept as
 * coded, but the TAB CONTENT is our settings model (VIDEO / AUDIO / GAME /
 * ABOUT over the flat mp6_config.json keys) instead of their MP4 content
 * (Prelaunch/Video/Input/Gameplay/Cheats/Interface over ConfigVar<T>).
 * Their ConfigVar-typed helpers became lambda-typed equivalents with the
 * same shapes. Rows kept semantically identical to L3's content set. */
// Credits: TwilitRealm

#include "settings.hpp"
#include "../../audio/menu_sfx.hpp"

#include "bool_button.hpp"
#include "content_setup.hpp"  /* [MP6] A4: guided content import dialog */
#include "launcher_state.hpp" /* [MP6] our config bridge */
#include "number_button.hpp"
#include "pane.hpp"
#include "ui.hpp"
#include "story_art.hpp"
#include "storybook_view.hpp"
#include <aurora/rmlui.hpp>
#include "lib/rmlui/WebGPURenderInterface.hpp"
#include "lib/gfx/common.hpp"
#include "lib/gfx/render_worker.hpp"
#include "lib/webgpu/gpu_prof.hpp"
#include "lib/window.hpp"
#include <SDL3/SDL_gamepad.h>

#include <SDL3/SDL_misc.h> /* [MP6] SDL_OpenURL for the save-folder action */
#include <SDL3/SDL_filesystem.h> /* [MP6] SDL_GetPathInfo for savestate slot stat */
#include <SDL3/SDL_time.h>       /* [MP6] SDL_TimeToDateTime for slot timestamps */
#include <SDL3/SDL_stdinc.h>     /* [MP6] SDL_strcasecmp / SDL_free for the Display row */
#include <SDL3/SDL_video.h>      /* [MP6] the Display row enumerates the live output set */

#include <algorithm>
#include <array>
#include <aurora/aurora.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

#include "mp6_display.h"   /* [MP6] Video tab: the live VSync apply + the Display row */
#include "mp6_enhancements.h" /* [MP6] Enhancements tab: preset table + derivation */
#include "mp6_freecam.h"   /* [MP6] Mods tab: freecam toggle (include) */
#include "mp6_savestate.h" /* [MP6] Save States tab: slot request/probe seam */
#include "mp6_unlock_all.h" /* [MP6] Extras tab: Unlock All mod */
#include "mp6_mic.h"        /* [MP6] Sound tab: the GameCube Mic */
#include "mp6_quick_play.h" /* [MP6] Extras tab: Quick Party mod */
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
#include "../../android/touch_pad_model.hpp"
#endif

/* [MP6] Mods tab, live Widescreen flip (aurora_bridge.c; see the Mods tab
 * builder below for the honest live-vs-scene-load semantics). */
extern "C" void mp6_widescreen_set_enabled(int enabled);
extern "C" void mp6_bridge_apply_content_aspect_policy(int aspectLockedCfg);

/* 1 = apply legacy FXAA now; 0 = queued full AA transaction or an unsupported
 * legacy transition. See launcher_core.cpp. */
extern "C" int mp6_launcher_aa_apply_live(int aa);

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {

bool SettingsWindow::StorybookSource::operator==(const StorybookSource &other) const
{
    return aa == other.aa && ambientOcclusion == other.ambientOcclusion &&
        fpsCorner == other.fpsCorner && heapScale == other.heapScale &&
        masterVolume == other.masterVolume && menuMotion == other.menuMotion &&
        sfxVoices == other.sfxVoices && shadowQuality == other.shadowQuality &&
        showFps == other.showFps &&
        unlockedFps == other.unlockedFps && smoothFrames == other.smoothFrames && vsync == other.vsync &&
        widescreen == other.widescreen && windowMode == other.windowMode &&
        touchEnabled == other.touchEnabled && touchSize == other.touchSize &&
        touchOpacity == other.touchOpacity && touchFloating == other.touchFloating &&
        touchVisible == other.touchVisible && freecam == other.freecam &&
        unlockAll == other.unlockAll && quickPlay == other.quickPlay &&
        microphone == other.microphone &&
        disabledFlags == other.disabledFlags &&
        windowScale == other.windowScale && tickHz == other.tickHz &&
        std::strcmp(backend, other.backend) == 0 &&
        std::strcmp(display, other.display) == 0;
}

static void select_display(const char *name)
{
    if (!mp6_display_select(name)) {
        push_toast({ .type = "display", .title = "Display unavailable",
            .content = "The window could not move to that display. The selection was not saved.",
            .duration = std::chrono::seconds(4) });
        return;
    }
    snprintf(cfg().display, sizeof(cfg().display), "%s", name);
    cfg_save();
}
namespace {

float settings_font_pixels(Rml::Vector2i size, float dpi)
{
    // Rasterize at the physical viewport size, including platform density.
    return std::clamp(std::max(18.f*dpi, std::min(size.x/48.f, size.y/26.f)),
        16.f, std::max(16.f, size.x/42.f));
}

Rml::String story_text(const Rml::String &text)
{
    // Values are escaped RML, but the native font engine consumes plain UTF-8.
    Rml::String plain; bool tag = false;
    for (const char c : text) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false;
        else if (!tag) plain += c;
    }
    const std::pair<const char *, const char *> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "},
    };
    for (const auto &[from, to] : entities) {
        size_t pos = 0;
        while ((pos = plain.find(from, pos)) != Rml::String::npos) {
            plain.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    }
    return plain;
}

#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
    class TouchLayoutPreview : public Component {
    public:
        explicit TouchLayoutPreview(Rml::Element *parent) : Component(append(parent, "div"))
        {
            mRoot->SetClass("touch-preview", true);
            const char *labels[] = {"Stick", "C", "+", "A", "B", "X", "Y", "Z", "L", "R", "Start"};
            for (int i = 0; i < MP6_TOUCH_COUNT; ++i) {
                mControls[i] = append(mRoot, "div");
                mControls[i]->SetClass("preview-control", true);
                mControls[i]->SetInnerRML(Rml::String("<span>") + labels[i] + "</span>");
            }
        }
        void update() override
        {
            auto *ctx = mRoot->GetContext();
            const auto size = ctx->GetDimensions();
            if (mWidth == size.x && mHeight == size.y &&
                std::memcmp(&mLast, &cfg().touch, sizeof(mLast)) == 0) return;
            mLast = cfg().touch; mWidth = size.x; mHeight = size.y;
            const float dp = ctx->GetDensityIndependentPixelRatio();
            mp6::touch::Layout layout;
            layout.make(size.x / dp, size.y / dp, cfg().touch);
            for (int i = 0; i < MP6_TOUCH_COUNT; ++i) {
                const auto &r = layout.controls[i];
                auto *control = mControls[i];
                control->SetProperty("left", fmt::format("{}%", 100 * (r.x - r.rx) / layout.w));
                control->SetProperty("top", fmt::format("{}%", 100 * (r.y - r.ry) / layout.h));
                control->SetProperty("width", fmt::format("{}%", 200 * r.rx / layout.w));
                control->SetProperty("height", fmt::format("{}%", 200 * r.ry / layout.h));
                control->SetProperty("display", (cfg().touch.visible & (1u << i)) ? "flex" : "none");
                control->SetProperty("opacity", fmt::format("{}", cfg().touch.opacity / 100.f));
            }
            mRoot->SetProperty("opacity", cfg().touch.enabled ? "1" : "0.35");
        }
    private:
        Rml::Element *mControls[MP6_TOUCH_COUNT]{};
        Mp6TouchConfig mLast{};
        int mWidth = -1, mHeight = -1;
    };
    void touch_preview(Pane &pane) { pane.add_child<TouchLayoutPreview>(); }
#endif

    constexpr std::array kFpsOverlayCornerNames = {
        "Top Left",
        "Top Right",
        "Bottom Left",
        "Bottom Right",
    };

    /* [MP6] our window-size presets (video.window_scale; 0 keeps the
     * current size). 640x480 is HU_FB x1. */
    struct WindowSizePreset {
        float scale;
        const char *label;
    };
    constexpr std::array<WindowSizePreset, 5> kWindowSizePresets = { {
        { 0.0f, "Auto (keep current)" },
        { 1.0f, "640 \xC3\x97 480" },
        { 1.5f, "960 \xC3\x97 720" },
        { 2.0f, "1280 \xC3\x97 960" },
        { 3.0f, "1920 \xC3\x97 1440" },
    } };

    /* One selection chooses a mutually exclusive AA mechanism. */
    struct AaPreset {
        int mode; /* Mp6AaMode */
        const char *label;
    };
    /* SSAA 1.5x/2x are DESKTOP-ONLY (P3): the two rows are gated out of the
     * array itself on Android and touch-QA builds, so both the label lookup
     * and the button loop below exclude them with no per-site guards --
     * mirroring launcher_core.cpp's platform UI gating. */
#if !defined(__ANDROID__) && !defined(MP6_SETTINGS_TOUCH_QA)
    constexpr std::array<AaPreset, 5> kAaPresets = { {
        { MP6_AA_OFF, "Off" },
        { MP6_AA_MSAA4X, "Sharp (MSAA 4x)" },
        { MP6_AA_FXAA, "Fast (FXAA)" },
        { MP6_AA_SSAA15, "Ultra (SSAA 1.5x)" },
        { MP6_AA_SSAA2X, "Ultra (SSAA 2x)" },
    } };
#else
    constexpr std::array<AaPreset, 3> kAaPresets = { {
        { MP6_AA_OFF, "Off" },
        { MP6_AA_MSAA4X, "Sharp (MSAA 4x)" },
        { MP6_AA_FXAA, "Fast (FXAA)" },
    } };
#endif

    /* ------------------------------------------------------------------
     * ENHANCEMENTS ([MP6], ours -- not partyboard's).
     *
     * The port ships enhanced by default and every enhancement has a
     * faithful-off switch. The six switches live in ONE named group with a
     * preset selector on top; the preset table and the label derivation are
     * NOT here -- they are pure C in src/enh/mp6_enhancements.c (which
     * is what tools/enh_preset_selftest.c drives), reached through
     * launcher_state.hpp's enh_preset_current()/enh_preset_apply(). This
     * file only renders them and applies the live side effects.
     * ------------------------------------------------------------------ */

    struct EnhPresetTier {
        int preset;
        const char *label;
        const char *blurb;
    };
    constexpr std::array<EnhPresetTier, 3> kEnhPresetTiers = { {
        { MP6_ENH_PRESET_VANILLA, "Original",
          "The classic 4:3 picture, original shadows and 60 FPS." },
        { MP6_ENH_PRESET_VANILLA_PLUS, "Classic+",
          "Keep the 4:3 picture, with smoother motion and cleaner edges." },
        { MP6_ENH_PRESET_MODERN, "Modern",
          "Fill a wide screen, with smoother motion and cleaner edges. Recommended." },
    } };

    Rml::String preset_label()
    {
        for (const auto &tier : kEnhPresetTiers) {
            if (tier.preset == enh_preset_current()) return tier.label;
        }
        return "Custom";
    }

    /* Every enhancement row's "modified" dot means DIFFERS FROM WHAT THE PORT
     * SHIPS -- i.e. from the default preset, not from the retail value. A dot
     * on "Widescreen: On" in a Modern-default build would be noise. */
    Mp6EnhValues enh_shipped()
    {
        Mp6EnhValues v;
        mp6_enh_defaults(&v);
        return v;
    }

    /* Anti-Aliasing is the one switch whose selection can take effect NOW or
     * next launch depending on what this session initialized with; both the
     * AA row and a preset press go through here so the two can never drift.
     * See mp6_launcher_aa_apply_live()'s contract in launcher_core.cpp. */
    void enh_apply_aa_live(int mode)
    {
        if (mp6_launcher_aa_apply_live(mode)) {
            aurora_set_post_aa(mode == MP6_AA_FXAA ? AURORA_POST_AA_FXAA : AURORA_POST_AA_NONE);
        }
    }

    /* Widescreen's live half, shared by its own row and by a preset press.
     * Order matters: the policy call reads mp6_widescreen_enabled(). Not done
     * pre-boot (prelaunch instance) -- A5 keeps the content-fit policy off
     * until right before GameMain so the launcher never letterboxes itself;
     * main_native.c applies the config value on boot as always. */
    void enh_apply_widescreen_live(bool value, bool inGame)
    {
        apply_display();
        if (inGame) {
            mp6_widescreen_set_enabled(value ? 1 : 0);
            mp6_bridge_apply_content_aspect_policy(cfg().aspectLocked);
        }
    }

    /* Selecting a preset writes all six values and applies the two that have
     * a live half. The rows themselves need no rebuild: every row re-reads its
     * getValue each frame (ControlledSelectButton::update), so pressing a
     * preset visibly moves all six -- and rebuilding the tab from inside a
     * button's own event handler would destroy the element dispatching it. */
    void enh_preset_press(int preset, bool inGame)
    {
        enh_preset_apply(preset);
        /* cfg_save() FIRST, because it is also the publish: it hands the six
         * new values to the enhancements seam. Only after that do
         * mp6_enh_widescreen() / mp6_enh_aa_mode() describe this press -- and
         * reading the answers back through the seam rather than straight off
         * cfg() is what keeps a set MP6_ENH_* / MP6_ENH_PRESET lever winning
         * over a preset button, which is the priority the rows themselves
         * claim.  (The individual rows do not need this: each is greyed out
         * while its own lever is set, so its value IS the answer.) */
        cfg_save();
        enh_apply_widescreen_live(mp6_enh_widescreen() != 0, inGame);
        enh_apply_aa_live(mp6_enh_aa_mode());
    }

    /* Their backend name/id tables, kept verbatim. */
    bool try_parse_backend(std::string_view backend, AuroraBackend &outBackend)
    {
        if (backend == "auto") {
            outBackend = BACKEND_AUTO;
            return true;
        }
        if (backend == "d3d11") {
            outBackend = BACKEND_D3D11;
            return true;
        }
        if (backend == "d3d12") {
            outBackend = BACKEND_D3D12;
            return true;
        }
        if (backend == "metal") {
            outBackend = BACKEND_METAL;
            return true;
        }
        if (backend == "vulkan") {
            outBackend = BACKEND_VULKAN;
            return true;
        }
        if (backend == "opengl") {
            outBackend = BACKEND_OPENGL;
            return true;
        }
        if (backend == "opengles") {
            outBackend = BACKEND_OPENGLES;
            return true;
        }
        if (backend == "webgpu") {
            outBackend = BACKEND_WEBGPU;
            return true;
        }
        if (backend == "null") {
            outBackend = BACKEND_NULL;
            return true;
        }

        return false;
    }

    std::string_view backend_name(AuroraBackend backend)
    {
        switch (backend) {
            default:
                return "Auto";
            case BACKEND_D3D12:
                return "D3D12";
            case BACKEND_D3D11:
                return "D3D11";
            case BACKEND_METAL:
                return "Metal";
            case BACKEND_VULKAN:
                return "Vulkan";
            case BACKEND_OPENGL:
                return "OpenGL";
            case BACKEND_OPENGLES:
                return "OpenGL ES";
            case BACKEND_WEBGPU:
                return "WebGPU";
            case BACKEND_NULL:
                return "Null";
        }
    }

    std::string_view backend_id(AuroraBackend backend)
    {
        switch (backend) {
            default:
                return "auto";
            case BACKEND_D3D12:
                return "d3d12";
            case BACKEND_D3D11:
                return "d3d11";
            case BACKEND_METAL:
                return "metal";
            case BACKEND_VULKAN:
                return "vulkan";
            case BACKEND_OPENGL:
                return "opengl";
            case BACKEND_OPENGLES:
                return "opengles";
            case BACKEND_WEBGPU:
                return "webgpu";
            case BACKEND_NULL:
                return "null";
        }
    }

    std::vector<AuroraBackend> available_backends()
    {
        std::vector<AuroraBackend> backends;
        backends.emplace_back(BACKEND_AUTO);
        size_t backendCount = 0;
        const AuroraBackend *raw = aurora_get_available_backends(&backendCount);
        for (size_t i = 0; i < backendCount; ++i) {
            /* [MP6] do not expose NULL; unlike partyboard we KEEP D3D11 --
             * their exclusion was an MP4-specific workaround, our port
             * supports it (L3 decision, retained). */
            if (raw[i] != BACKEND_NULL) {
                backends.emplace_back(raw[i]);
            }
        }
        return backends;
    }

    AuroraBackend configured_backend()
    {
        AuroraBackend configuredBackend = BACKEND_AUTO;
        if (!try_parse_backend(cfg().backend, configuredBackend)) {
            configuredBackend = BACKEND_AUTO;
        }
        return configuredBackend;
    }

    /* Their config_bool_select helper, with the ConfigVar<bool>& swapped for
     * getter/setter lambdas over our flat config ([MP6]); shape and
     * register_control wiring unchanged. */
    struct ConfigBoolProps {
        Rml::String key;
        Rml::String icon;
        Rml::String helpText;
        std::function<bool()> getValue;
        std::function<void(bool)> setValue;
        std::function<bool()> isDisabled;
        std::function<bool()> isModified;
    };

    SelectButton &config_bool_select(Pane &leftPane, Pane &rightPane, ConfigBoolProps props)
    {
        const Rml::String title = props.key;
        auto &button = leftPane.add_child<BoolButton>(BoolButton::Props {
            .key = std::move(props.key),
            .icon = std::move(props.icon),
            .getValue = props.getValue,
            .setValue =
                [get = props.getValue, set = std::move(props.setValue)](bool value) {
                    if (value == get()) {
                        return;
                    }
                    set(value);
                    cfg_save();
                },
            .isDisabled = std::move(props.isDisabled),
            .isModified = std::move(props.isModified),
        });
        leftPane.register_control(button, rightPane, [title, helpText = std::move(props.helpText)](Pane &pane) {
            pane.clear();
            pane.add_section(title);
            pane.add_rml(helpText);
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
            if (title == "Show Touch Controls" || title == "Floating Sticks") touch_preview(pane);
#endif
        });
        return button;
    }

    /* [MP6] Save States tab: one status string per slot file. The probe is
     * header-only (savestate.c), so a stale-binary slot reads as
     * "incompatible (other build)" here instead of a raw console error. */
    Rml::String slot_status(const char *path)
    {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
            return "empty";
        }
        const int probe = mp6_savestate_probe(path);
        if (probe == MP6_SAVESTATE_ERR_BINARY_MISMATCH) {
            return "from another version";
        }
        if (probe != MP6_SAVESTATE_OK) {
            return "cannot be read";
        }
        SDL_DateTime dt;
        if (SDL_TimeToDateTime(info.modify_time, &dt, true)) {
            return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}", dt.year, dt.month, dt.day, dt.hour, dt.minute);
        }
        return "saved";
    }

} // namespace

void SettingsWindow::preload_resources()
{
    auto* context=aurora::rmlui::get_context();
    if(!context)return;
    // Display preferences may have resized the SDL window since Rml startup,
    // and the game's presentation can be aspect-fitted. The book always uses
    // the full native window, so warm its actual font sizes without changing
    // the context metrics seen by the first game/overlay frame.
    const auto originalDimensions=context->GetDimensions();
    const auto windowSize=aurora::window::get_window_size();
    if(windowSize.native_fb_width&&windowSize.native_fb_height)
        context->SetDimensions({int(windowSize.native_fb_width),int(windowSize.native_fb_height)});
    const auto textures=StorybookView::preload_resources(*context);
    // Aurora defers uploads until a texture is drawn. Prime them (and the UI
    // pipeline) in a private 1px target before any interactive frame. This is
    // a startup-only graphics frame: no surface acquisition/present, settings
    // document rendering, focus change, game tick, or entrance animation.
    if(aurora::gfx::begin_frame()) {
        aurora::gfx::finish();
        auto* renderer=static_cast<aurora::rmlui::WebGPURenderInterface*>(Rml::GetRenderInterface());
        auto target=aurora::webgpu::create_render_texture(1,1,false);
        auto& manager=context->GetRenderManager();
        const auto viewport=manager.GetViewport();
        renderer->SetWindowSize({1,1});
        renderer->BeginFrame(target,{},aurora::rmlui::BaseLayerContent::Transparent);
        manager.PrepareRender({1,1});
        Rml::Mesh mesh;
        const Rml::ColourbPremultiplied white{255,255,255,255};
        mesh.vertices={{{0,0},white,{0,0}},{{1,0},white,{1,0}},
                       {{1,1},white,{1,1}},{{0,1},white,{0,1}}};
        mesh.indices={0,1,2,0,2,3};
        auto geometry=manager.MakeGeometry(std::move(mesh));
        for(const auto& texture:textures)geometry.Render({0,0},texture);
        // Prime the retained procedural-sky pipeline here as well. The quad's
        // UV x values address entries 0 and 1 in this untextured shader.
        const aurora::rmlui::GeometryTransform identity{{1,0,0,1},{0,0,0,0},{1,1,1,1}};
        const std::array<aurora::rmlui::GeometryTransform,2> transforms{identity,identity};
        auto skyShader=manager.CompileShader("transform-palette",{});
        aurora::rmlui::set_geometry_transform_palette(transforms);
        geometry.Render({0,0},{},skyShader);
        manager.ResetState();
        renderer->EndFrame();
        manager.SetViewport(viewport);
        renderer->SetWindowSize(context->GetDimensions());
        aurora::gfx::finish();
        aurora::gfx::end_frame([](wgpu::CommandEncoder& encoder){
            aurora::webgpu::gpu_prof::frame_end(encoder);
            const auto commands=encoder.Finish();
            aurora::webgpu::g_queue.Submit(1,&commands);
            aurora::webgpu::gpu_prof::after_submit();
        });
        aurora::gfx::render_worker::synchronize();
    }
    // Prime Rml's shared stylesheet/document caches while still loading. Never
    // show this document or put it on the focus/input stack, and never run any
    // control callbacks. A fresh settings window still reads current settings.
    {
        SettingsWindow warmup(true);
        // Lay out before closing, while visibility remains hidden. Otherwise
        // only parsing is warmed and the first opening pays for font metrics.
        warmup.mDocument->UpdateDocument();
    }
    context->SetDimensions(originalDimensions);
    static_cast<aurora::rmlui::WebGPURenderInterface*>(Rml::GetRenderInterface())->SetWindowSize(originalDimensions);
    context->Update(); // Retire the hidden document before presenting the UI.
}

SettingsWindow::SettingsWindow(bool prelaunch, int initialTab, bool inGame)
    : Window(true), mPrelaunch(prelaunch), mInGame(inGame)
{
    mSeenSavestateGen = mp6_savestate_ui_generation();
    mSuppressNavFallback = prelaunch;
    // The persistent in-game document is laid out while hidden. Use its real
    // font size now, not a 28px throwaway layout followed by a cold first-show
    // inherited-font invalidation. update() still handles live resize/DPI.
    auto* context=mDocument->GetContext();
    mFontPixels=settings_font_pixels(context->GetDimensions(),context->GetDensityIndependentPixelRatio());
    mRoot->SetProperty("font-size",fmt::format("{}px",mFontPixels));
    build_shell();
    add_tab("Picture", [this](Rml::Element *content) { build_picture(content); prepare_book("Picture", "Make the adventure shine."); });
    add_tab("Sound", [this](Rml::Element *content) { build_sound(content); prepare_book("Sound", "Every party has a soundtrack."); });
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
    add_tab("Controls", [this](Rml::Element *content) { build_controls(content); prepare_book("Controls", "Find your way to play."); });
#endif
    add_tab("Game", [this](Rml::Element *content) { build_game(content); prepare_book("Game", "Keep your adventure going."); });
    add_tab("Extras", [this](Rml::Element *content) { build_extras(content); prepare_book("Extras", "A little extra party magic."); });
    if (initialTab > 0) set_active_tab(initialTab);
}

void SettingsWindow::build_shell()
{
    static Rml::ElementInstancerGeneric<StoryShape> shapeInstancer;
    Rml::Factory::RegisterElementInstancer("story-shape", &shapeInstancer);
    mRoot->SetClass("storybook", true);
    mRoot->SetClass("in-game", mInGame);
    auto *backdrop = append(mRoot, "story-backdrop");
    backdrop->SetInnerRML("<story-shape kind=\"day\" class=\"day-field\"/>"
        "<story-shape kind=\"rays\" class=\"day-rays\"/>"
        "<story-shape kind=\"zigzag\" class=\"night-field\"/>");
    // Separate retained meshes and sprites: no flattened book or UI screenshot.
    auto *book = append(mRoot, "story-book");
    book->SetInnerRML("<book-cover/><book-edges class=\"left\"/><book-edges class=\"right\"/>"
        "<story-shape kind=\"page\" class=\"page left\"/>"
        "<story-shape kind=\"page\" right=\"1\" class=\"page right\"/><book-spine/>");
    auto *decoration = append(mRoot, "story-decoration");
    for (int i=0; i<9; ++i) {
        auto *star = append(decoration, "story-shape");
        star->SetAttribute("kind", "star");
        star->SetClass("sky-star", true);
        star->SetProperty("left", fmt::format("{}%", 56+(i*13)%42));
        star->SetProperty("top", fmt::format("{}%", i%2 ? 92+(i%4) : 1+(i%5)));
        star->SetClass(i%2 ? "twinkle-slow" : "twinkle-fast", true);
    }
    for (int i=0; i<7; ++i) {
        auto *paper = append(decoration, "confetti");
        paper->SetProperty("left", fmt::format("{}%", 1+(i*7)%42));
        paper->SetProperty("top", fmt::format("{}%", i%2 ? 93 : 1+(i%4)));
        paper->SetClass(i%2 ? "pink" : "blue", true);
    }
    auto *cast = append(mRoot, "story-cast");
    const auto sprite = [](const char *name) { return escape(resource_path(
        Rml::String("menu/story-")+name+".png").generic_string()); };
    cast->SetInnerRML("<img class=\"host brighton\" src=\"" + sprite("brighton") + "\"/>"
        "<img class=\"host twila\" src=\"" + sprite("twila") + "\"/>"
        "<img class=\"cloud cloud-left\" src=\"" + sprite("cloud") + "\"/>"
        "<img class=\"cloud cloud-right\" src=\"" + sprite("cloud") + "\"/>");
    auto *emblem = append(mRoot, "story-shape");
    emblem->SetAttribute("kind", "sun");
    emblem->SetClass("chapter-emblem", true);
    auto *corners = append(book, "page-ornaments");
    corners->SetInnerRML("<page-corner class=\"top-left\"/><page-corner class=\"bottom-left\"/>"
        "<page-corner class=\"top-right\"/><page-corner class=\"bottom-right\"/>");
    auto *heading = append(mRoot, "chapter-heading");
    mChapterTitle = append(heading, "chapter-title");
    mChapterSubtitle = append(heading, "chapter-subtitle");

    auto *preview = append(mRoot, "story-preview");
    preview->SetInnerRML("<preview-heading>Your adventure</preview-heading>"
        "<preview-frame><img class=\"game-frame\" src=\"mp6scene://live\"/>"
        "<preview-empty><span>Your adventure goes here</span>"
        "<small>Start playing to see your game here.</small></preview-empty>"
        "<story-shape kind=\"photo\" class=\"photo-corner tl\"/><story-shape kind=\"photo\" class=\"photo-corner tr\"/>"
        "<story-shape kind=\"photo\" class=\"photo-corner bl\"/><story-shape kind=\"photo\" class=\"photo-corner br\"/></preview-frame>"
        "<preview-caption>Live game preview</preview-caption>");
    // A diagram is useful on the Controls page; it is native UI, not a fake
    // screenshot, and deliberately labelled as a controller guide.
    mController = append(mRoot, "controller-guide");
    mController->SetInnerRML("<preview-heading>Your controller</preview-heading>"
        "<controller-body><pad-trigger class=\"trigger-l\"><span>L</span></pad-trigger>"
        "<pad-trigger class=\"trigger-r\"><span>R</span></pad-trigger><pad-trigger class=\"trigger-z\"><span>Z</span></pad-trigger>"
        "<pad-key class=\"stick\"><span>Stick</span></pad-key>"
        "<pad-key class=\"dpad\"><span>+</span></pad-key>"
        "<pad-key class=\"cstick\"><span>C</span></pad-key>"
        "<pad-key class=\"a\"><span>A</span></pad-key><pad-key class=\"b\"><span>B</span></pad-key>"
        "<pad-key class=\"x\"><span>X</span></pad-key><pad-key class=\"y\"><span>Y</span></pad-key>"
        "<pad-key class=\"start\"><span>Start</span></pad-key></controller-body>"
        "<controller-status>USB, Bluetooth or keyboard</controller-status>");

    auto *guide = append(mRoot, "settings-guide");
    guide->SetInnerRML("<guide-title>Welcome!</guide-title>"
        "<guide-copy>Choose an option to make it your party.</guide-copy>");
    mGuideCopy = guide->QuerySelector("guide-copy");
    auto *footer = append(mRoot, "settings-footer");
    mChooseButton = std::make_unique<Button>(footer, "Choose");
    mChooseButton->root()->SetId("settings-choose");
    mChooseButton->root()->SetInnerRML("<pad-prompt class=\"a\"><span>A</span></pad-prompt><span>Choose</span>");
    mChooseButton->on_pressed([this] {
        const size_t paneIndex = mRoot->IsClassSet("detail-open") ? 1 : 0;
        if (paneIndex < mContentComponents.size())
            if (auto *pane = dynamic_cast<Pane *>(mContentComponents[paneIndex].get())) pane->activate_focused();
    });
    mBackButton = std::make_unique<Button>(footer, "Back");
    mBackButton->root()->SetId("settings-back");
    mBackButton->root()->SetInnerRML("<pad-prompt class=\"b\"><span>B</span></pad-prompt><span>Back</span>");
    mBackButton->on_pressed([this] {
        if (mRoot->IsClassSet("detail-open")) {
            mRoot->SetClass("detail-open", false);
            mDetailFocusPending = -2;
        } else request_close();
    });
    mHelpButton = std::make_unique<Button>(footer, "Info");
    mHelpButton->root()->SetClass("settings-help", true);
    mHelpButton->root()->SetProperty("display", "none");
    mPreviousChapter = std::make_unique<Button>(footer, "L");
    mPreviousChapter->root()->SetId("previous-chapter");
    mPreviousChapter->root()->SetAttribute("aria-label", "Previous chapter");
    mPreviousChapter->on_pressed([this] { set_active_tab((mChapterIndex+kSettingsChapterCount-1)%kSettingsChapterCount); });
    mNextChapter = std::make_unique<Button>(footer, "R");
    mNextChapter->root()->SetId("next-chapter");
    mNextChapter->root()->SetAttribute("aria-label", "Next chapter");
    mNextChapter->on_pressed([this] { set_active_tab((mChapterIndex+1)%kSettingsChapterCount); });
    append(footer, "chapter-hint")->SetInnerRML("Chapters");
    listen(mRoot, Rml::EventId::Submit, [this](Rml::Event &event) {
        if (event.GetTargetElement()->Closest("pane.settings-list")) {
            // Help is printed on the right page; legacy Submit must not focus
            // or expose its hidden option-list pane.
            event.StopImmediatePropagation();
        }
    }, true);
    static Rml::ElementInstancerGeneric<StorybookView> bookInstancer;
    Rml::Factory::RegisterElementInstancer("storybook-view", &bookInstancer);
    mRoot->SetClass("native-storybook", true);
    mRoot->SetAttribute("data-full-window-ui", "");
    mStorybook=static_cast<StorybookView *>(append(mRoot,"storybook-view"));
    const auto observe_story_focus = [this](Rml::Event &event) {
        auto *target = event.GetTargetElement();
        // RmlUi's Closest searches ancestors, excluding the target itself.
        // Controller/keyboard focus lands on the row, while hover may target
        // its children. Both must invalidate the retained cursor/description.
        auto *row = target->IsClassSet("setting-row") ? target : target->Closest(".setting-row");
        if (row != nullptr && row != mStoryFocusRow) {
            mStoryFocusRow = row;
            mStoryStateDirty = true;
        }
    };
    listen(mContentRoot, Rml::EventId::Focus, observe_story_focus, true);
    listen(mContentRoot, Rml::EventId::Mouseover, observe_story_focus, true);
    listen(mStorybook,Rml::EventId::Mousedown,[this](Rml::Event &event){
        if(event.GetParameter<int>("button",0)!=0)return;
        // RmlUi focuses the mesh to deliver its click. Keep keyboard navigation
        // on the semantic control, including after clicking the footer/paper.
        const size_t pane=mRoot->IsClassSet("detail-open")?1:0;
        if(pane<mContentComponents.size())mContentComponents[pane]->focus();
    });
    listen(mStorybook,Rml::EventId::Mousemove,[this](Rml::Event &event){storybook_pointer(event,false);});
    listen(mStorybook,Rml::EventId::Click,[this](Rml::Event &event){storybook_pointer(event,true);});
    listen(mRoot,Rml::EventId::Keydown,[this](Rml::Event &event){
        const auto command=map_nav_event(event);
        if(mStorybook->handing_off()&&command!=NavCommand::Cancel&&command!=NavCommand::Menu){
            event.StopImmediatePropagation();return;
        }
        const auto key=event.GetParameter<int>("key_identifier",Rml::Input::KI_UNKNOWN);
        if(event.GetParameter<int>("ctrl_key",0)||event.GetParameter<int>("alt_key",0)||event.GetParameter<int>("meta_key",0))return;
        if(key==Rml::Input::KI_Q||key==Rml::Input::KI_E){
            if(!mStorybook->turning())set_active_tab((mChapterIndex+(key==Rml::Input::KI_Q?kSettingsChapterCount-1:1))%kSettingsChapterCount);
            event.StopPropagation();
        }
    },true);
}

void SettingsWindow::prepare_book(const char *title, const char *subtitle)
{
    mRoot->SetClass("detail-open", false);
    const char *names[] = {"Picture", "Sound",
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
        "Controls",
#endif
        "Game", "Extras"};
    for(int i=0; i<kSettingsChapterCount; ++i) if (Rml::String(title)==names[i]) mChapterIndex=i;
    mRoot->SetClass("controls-chapter", Rml::String(title)=="Controls");
    mChapterTitle->SetInnerRML(title);
    mChapterSubtitle->SetInnerRML(subtitle);
    mStoryChapter = title;
    mStoryRowsDirty = true;
    mStoryStateDirty = true;
    auto *list = mContentRoot->QuerySelector("pane.settings-list");
    if (!list) return;
    if(Rml::String(title)=="Picture"&&!mContentComponents.empty())
        if(auto *pane=dynamic_cast<Pane *>(mContentComponents[0].get()))pane->promote_first("Preset");
    mStoryRows.clear();
    mStoryValues.clear();
    mStoryLabels.clear();
    mStoryValueLabels.clear();
    mStoryDisabled.clear();
    mStoryCanLeft.clear();
    mStoryCanRight.clear();
    mStoryFocusRow = nullptr;
    // Rows stay in place on hover/focus. Only the native star cursor animates.
    for (int i=0; i<list->GetNumChildren(); ++i) {
        auto *row = list->GetChild(i);
        if (!row->HasAttribute("data-title")) continue;
        mStoryRows.push_back(row);
        row->SetClass("setting-row", true);
        const auto titleText = row->GetAttribute<Rml::String>("data-title", "");
        mStoryLabels.push_back(story_text(titleText));
        mStoryValues.push_back(row->QuerySelector("value"));
        if (row->GetTagName() == "button") {
            row->SetInnerRML("<focus-star><story-shape kind=\"star\"/></focus-star><key>" +
                escape(titleText) + "</key><span class=\"action-arrow\">&#x203a;</span>");
        }
        auto ribbon = mDocument->CreateElement("story-shape");
        ribbon->SetAttribute("kind", "ribbon");
        ribbon->SetClass("row-ribbon", true);
        row->InsertBefore(std::move(ribbon), row->GetFirstChild());
    }
    mRoot->SetClass("chapter-even", (mChapterIndex % 2) == 0);
    mDetailFocusPending = -2;
}

void SettingsWindow::turn_from_launcher(const StorybookView& source)
{
    if(mPrelaunch&&!mInGame&&mStorybook)mStorybook->turn_from(source);
}

void SettingsWindow::show()
{
    const bool wasVisible = visible();
    if(!wasVisible&&mInGame)audio::play_menu_sound(audio::MenuSound::Accept);
    mClosing = false;
    mPendingClose = false;
    mRoot->SetClass("book-closing", false);
    Window::show();
    // Launcher -> Settings changes the spread inside an already-open book.
    // Only a standalone/in-game menu needs to bring the scenery and book in.
    if (!wasVisible && mStorybook) mStorybook->enter(cfg().menuMotion != 0 && (!mPrelaunch || mInGame));
    mRoot->SetClass("quiet-motion", !cfg().menuMotion);
    mDetailFocusPending = -2;
    mStoryStateDirty = true;
}

bool SettingsWindow::visible() const
{
    // Release the game's input immediately while the last visual frames finish.
    return !mClosing && Window::visible();
}

void SettingsWindow::hide(bool close)
{
    if (mClosing) {mPendingClose = mPendingClose || close; return;}
    if(Window::visible())audio::play_menu_sound(audio::MenuSound::Cancel);
    if ((mPrelaunch && !mInGame) || !mStorybook || !cfg().menuMotion || !Window::visible()) {Window::hide(close); return;}
    mClosing = true;
    mPendingClose = close;
    mDetailFocusPending = 0;
    mRoot->SetClass("detail-open", false);
    mRoot->SetClass("book-closing", true);
    mStorybook->leave(true);
}

bool SettingsWindow::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    if (cmd == NavCommand::Cancel) {
        if (mRoot->IsClassSet("detail-open")) {
            mRoot->SetClass("detail-open", false);
            mDetailFocusPending = -2;
        } else request_close();
        return true;
    }
    if (event.GetTargetElement()->Closest("settings-footer")) {
        if (cmd == NavCommand::Up) { focus_content(); return true; }
        if (cmd == NavCommand::Left) { mBackButton->focus(); return true; }
        if (cmd == NavCommand::Right) { mHelpButton->focus(); return true; }
    }
    // A list row with no adjustable value must not navigate into an invisible
    // choices pane. The row handles value steps before this fallback runs.
    if ((cmd == NavCommand::Left || cmd == NavCommand::Right) &&
        event.GetTargetElement()->Closest("pane.settings-list")) return true;
    return Window::handle_nav_command(event, cmd);
}

void SettingsWindow::build_picture(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);


        leftPane.add_section("Picture");
        config_bool_select(leftPane, rightPane,
            {
                .key = "Widescreen",
                .helpText = "Fill wide screens without stretching the picture. Turn off for the original 4:3 view.<br/><br/>Some scenes finish adjusting when you enter the next area.",
                .getValue = [] { return cfg().widescreen != 0; },
                .setValue =
                    [inGame = mInGame](bool value) {
                        cfg().widescreen = value ? 1 : 0;
                        enh_apply_widescreen_live(value, inGame);
                    },
                .isDisabled = [] { return getenv("MP6_FREE_ASPECT") != NULL || getenv("MP6_ENH_WIDESCREEN") != NULL; },
                .isModified = [] { return cfg().widescreen != enh_shipped().widescreen; },
            });

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Anti-Aliasing",
                                      .getValue =
                                          [] {
                                              for (const auto &p : kAaPresets) {
                                                  if (cfg().aa == p.mode) {
                                                      return Rml::String { p.mode == MP6_AA_FXAA ? "Fast" :
                                                          p.mode == MP6_AA_MSAA4X ? "Sharp" :
                                                          p.mode == MP6_AA_SSAA15 ? "Ultra 1.5x" :
                                                          p.mode == MP6_AA_SSAA2X ? "Ultra 2x" : "Off" };
                                                  }
                                              }
                                              return Rml::String { "Off" };
                                          },
                                      .isDisabled = [] { return getenv("MP6_MSAA") != NULL || getenv("MP6_FXAA") != NULL || getenv("MP6_SSAA") != NULL || getenv("MP6_ENH_AA") != NULL; },
                                      .isModified = [] { return cfg().aa != enh_shipped().aa; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                for (const auto &preset : kAaPresets) {
                    pane
                        .add_button({
                            .text = preset.label,
                            .isSelected = [mode = preset.mode] { return cfg().aa == mode; },
                        })
                        .on_pressed([mode = preset.mode] {
                            cfg().aa = mode;
                            enh_apply_aa_live(mode);
                            cfg_save();
                        });
                }
                pane.add_rml("<br/>Smooth jagged edges. <b>Fast</b> is best for performance but "
                             "can soften the picture. <b>Sharp</b> keeps edges clearer but is "
                             "more demanding."
#if !defined(__ANDROID__) && !defined(MP6_SETTINGS_TOUCH_QA)
                             " <b>Ultra</b> gives the cleanest picture and uses the most power."
#else
                             "<br/><br/>If Sharp causes slowdowns, choose Fast or Off."
#endif
#ifdef MP6_AURORA_LIVE_AA
                             "<br/><br/>Applies during play. Switching modes may briefly pause the picture."
#else
                             "<br/><br/>Some changes need a restart. The menu will tell you when one is needed."
#endif
                );
            });

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Ambient Occlusion",
                                      .getValue = [] { return Rml::String { cfg().ambientOcclusion == 2 ? "Strong" : cfg().ambientOcclusion == 1 ? "Subtle" : "Off" }; },
                                      .isDisabled = [] { return getenv("MP6_ENH_AMBIENT_OCCLUSION") != nullptr; },
                                      .isModified = [] { return cfg().ambientOcclusion != 0; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                for (const int level : { 0, 1, 2 }) {
                    pane.add_button({
                            .text = level == 2 ? "Strong" : level == 1 ? "Subtle" : "Off",
                            .isSelected = [level] { return cfg().ambientOcclusion == level; },
                        })
                        .on_pressed([level] {
                            cfg().ambientOcclusion = level;
                            cfg_save();
                        });
                }
                pane.add_rml("<br/>Add soft shading where characters and scenery meet, giving "
                             "the board more depth. <b>Subtle</b> keeps a lighter look; "
                             "<b>Strong</b> makes the shading more noticeable."
                             "<br/><br/>Applies during play, with no restart. May lower FPS "
                             "and use more battery. Off keeps the original lighting.");
            });

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Shadow Quality",
                                      .getValue = [] { return Rml::String { fmt::format("{}x", cfg().shadowQuality) }; },
                                      .isDisabled = [] { return getenv("MP6_SHADOW_QUALITY") != NULL || getenv("MP6_ENH_SHADOW_QUALITY") != NULL; },
                                      .isModified = [] { return cfg().shadowQuality != enh_shipped().shadowQuality; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                for (const int scale : { 1, 2, 4, 8, 16 }) {
                    pane
                        .add_button({
                            .text = fmt::format("{}x", scale),
                            .isSelected = [scale] { return cfg().shadowQuality == scale; },
                        })
                        .on_pressed([scale] {
                            cfg().shadowQuality = scale;
                            cfg_save();
                        });
                }
                pane.add_rml("<br/>Make supported character shadows sharper. 1x is the original "
                             "quality; 4x is recommended. Higher settings use more memory and "
                             "may lower FPS.<br/><br/>Changes apply during play. If your device "
                             "cannot fit the selected quality, the game uses a lower setting.");
            });

        leftPane.add_section("Smoothness");
        // One selector over two saved keys: video.unlocked_fps (on/off) and
        // video.frame_blending (which in-between frame style; Smooth by default).
        //   0 Off        -- present == tick
        //   1 Responsive -- predict ahead from the last two game frames
        //   2 Smooth     -- blend the last two game frames, one frame later
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Unlocked FPS",
                                      .getValue = [] { return Rml::String { !cfg().unlockedFps ? "Off" : cfg().smoothFrames ? "Smooth" : "Responsive" }; },
                                      .isDisabled = [] { return getenv("MP6_UNLOCKED_FPS") != NULL || getenv("MP6_ENH_UNLOCKED_FPS") != NULL; },
                                      .isModified = [] { return cfg().unlockedFps != enh_shipped().unlockedFps || cfg().smoothFrames != 1; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                for (const int mode : { 0, 1, 2 }) {
                    pane.add_button({
                            .text = mode == 2 ? "Smooth" : mode == 1 ? "Responsive" : "Off",
                            .isSelected = [mode] {
                                const int cur = !cfg().unlockedFps ? 0 : cfg().smoothFrames ? 2 : 1;
                                return cur == mode;
                            },
                        })
                        .on_pressed([mode] {
                            cfg().unlockedFps = mode != 0 ? 1 : 0;
                            if (mode != 0) cfg().smoothFrames = mode == 2 ? 1 : 0;
                            cfg_save();
                        });
                }
                pane.add_rml("<br/>Show extra frames between the game's 60 updates a second, so "
                             "characters and the camera move smoother without speeding up the "
                             "game. Menus and some effects still update at 60 FPS."
                             "<br/><br/><b>Responsive</b> predicts each extra frame from the last "
                             "two game frames, for the lowest input delay. <b>Smooth</b> blends "
                             "between the last two game frames instead, so movement never "
                             "overshoots, but input shows up one game frame (about 17 ms) later."
                             "<br/><br/>Uses more power; Off "
                             "saves battery. Fast Forward takes priority when enabled in Extras.");
            });

        leftPane.add_section("Quick Setup");
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Preset",
                                      .getValue = [] { return preset_label(); },
                                      .isModified = [] { return enh_preset_current() != MP6_ENH_PRESET_DEFAULT; },
                                  }),
            rightPane, [inGame = mInGame](Pane &pane) {
                pane.clear();
                for (const auto &tier : kEnhPresetTiers) {
                    pane
                        .add_button({
                            .text = tier.label,
                            .isSelected = [preset = tier.preset] { return enh_preset_current() == preset; },
                        })
                        .on_pressed([preset = tier.preset, inGame] { enh_preset_press(preset, inGame); });
                }
                Rml::String help = "<br/>Choose a starting point, then adjust settings to taste.<br/>";
                for (const auto &tier : kEnhPresetTiers) {
                    help += fmt::format("<br/><b>{}:</b> {}<br/>", escape(tier.label),
                        escape(tier.blurb));
                }
                help += "<br/>Also sets More Sound Effects (Sound) and Extra Memory (Extras). "
                        "Memory changes need a restart during play."
#ifndef MP6_AURORA_LIVE_AA
                        " Some anti-aliasing changes do too."
#endif
                        ;
                pane.add_rml(help);
            });

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) /* web and Android: fixed present mode (mp6_display_set_vsync) */
        config_bool_select(leftPane, rightPane,
            {
                .key = "VSync",
                .helpText = "Match the frame rate to your screen to prevent horizontal tearing. Turn off for higher FPS, but tearing may appear. Your device may still limit the frame rate.",
                .getValue = [] { return cfg().vsync != 0; },
                .setValue = [](bool value) {
                    cfg().vsync = value ? 1 : 0;
                    mp6_display_set_vsync(value ? 1 : 0);
                },
                .isModified = [] { return cfg().vsync != 1; },
            });
#endif

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Show FPS Counter",
                                      .getValue =
                                          [] {
                                              if (!cfg().showFps) {
                                                  return Rml::String { "Off" };
                                              }
                                              return Rml::String { kFpsOverlayCornerNames[cfg().fpsCorner] };
                                          },
                                      .isModified =
                                          [] {
                                              return cfg().showFps != 0 || (cfg().showFps && cfg().fpsCorner != 0);
                                          },
                                  }),
            rightPane, [](Pane &pane) {
                pane.add_button({
                                    .text = "Off",
                                    .isSelected = [] { return !cfg().showFps; },
                                })
                    .on_pressed([] {
                        cfg().showFps = 0;
                        cfg_save();
                    });
                for (int i = 0; i < static_cast<int>(kFpsOverlayCornerNames.size()); ++i) {
                    pane
                        .add_button({
                            .text = kFpsOverlayCornerNames[i],
                            .isSelected = [i] { return cfg().showFps && cfg().fpsCorner == i; },
                        })
                        .on_pressed([i] {
                            cfg().showFps = 1;
                            cfg().fpsCorner = i;
                            cfg_save();
                        });
                }
                pane.add_rml("<br/>Display the current framerate in a corner of the screen while playing.");
            });

#if !defined(__ANDROID__) && !defined(MP6_SETTINGS_TOUCH_QA)
        leftPane.add_section("Window");
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Window Mode",
                                      .getValue =
                                          [] {
                                              return Rml::String { cfg().windowMode == MP6_WINMODE_FULLSCREEN ? "Fullscreen" : "Windowed" };
                                          },
                                      .isModified = [] { return cfg().windowMode != MP6_WINMODE_WINDOWED; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                pane.add_button({
                                    .text = "Windowed",
                                    .isSelected = [] { return cfg().windowMode == MP6_WINMODE_WINDOWED; },
                                })
                    .on_pressed([] {
                        cfg().windowMode = MP6_WINMODE_WINDOWED;
                        cfg_save();
                        apply_display();
                    });
                pane.add_button({
                                    .text = "Fullscreen",
                                    .isSelected = [] { return cfg().windowMode == MP6_WINMODE_FULLSCREEN; },
                                })
                    .on_pressed([] {
                        cfg().windowMode = MP6_WINMODE_FULLSCREEN;
                        cfg_save();
                        apply_display();
                    });
                pane.add_rml("<br/>Fill your screen, or play in a resizable window. Changes apply immediately.");
            });

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Window Size",
                                      .getValue =
                                          [] {
                                              for (const auto &p : kWindowSizePresets) {
                                                  if (cfg().windowScale == p.scale) {
                                                      return Rml::String { p.label };
                                                  }
                                              }
                                              return Rml::String { fmt::format("{:g}\xC3\x97", cfg().windowScale) };
                                          },
                                      .isDisabled = [] { return cfg().windowMode == MP6_WINMODE_FULLSCREEN; },
                                      .isModified = [] { return cfg().windowScale != 0.0f; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                for (const auto &preset : kWindowSizePresets) {
                    pane.add_button({
                                        .text = preset.label,
                                        .isSelected = [scale = preset.scale] { return cfg().windowScale == scale; },
                                    })
                        .on_pressed([scale = preset.scale] {
                            cfg().windowScale = scale;
                            cfg_save();
                            apply_display();
                        });
                }
                pane.add_rml("<br/>Resize the game window. Smaller windows can improve performance. Available in Windowed mode.");
            });

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Display",
                                      .getValue =
                                          [] {
                                              if (SDL_strcasecmp(cfg().display, "auto") == 0) {
                                                  return Rml::String { "Auto (highest refresh)" };
                                              }
                                              if (SDL_strcasecmp(cfg().display, "primary") == 0) {
                                                  return Rml::String { "Primary" };
                                              }
                                              return Rml::String { cfg().display };
                                          },
                                      .isModified = [] { return SDL_strcasecmp(cfg().display, "auto") != 0; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                pane.add_button({
                                    .text = "Auto (highest refresh)",
                                    .isSelected = [] { return SDL_strcasecmp(cfg().display, "auto") == 0; },
                                })
                    .on_pressed([] {
                        select_display("auto");
                    });
                pane.add_button({
                                    .text = "Primary",
                                    .isSelected = [] { return SDL_strcasecmp(cfg().display, "primary") == 0; },
                                })
                    .on_pressed([] {
                        select_display("primary");
                    });
                int count = 0;
                SDL_DisplayID *ids = SDL_GetDisplays(&count);
                if (ids != nullptr) {
                    for (int i = 0; i < count; ++i) {
                        const char *rawName = SDL_GetDisplayName(ids[i]);
                        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(ids[i]);
                        if (rawName == nullptr || rawName[0] == '\0') continue;
                        const Rml::String name { rawName };
                        Rml::String label = name;
                        if (mode != nullptr) {
                            double hz = (mode->refresh_rate_denominator > 0 && mode->refresh_rate_numerator > 0)
                                            ? (double)mode->refresh_rate_numerator / (double)mode->refresh_rate_denominator
                                            : (double)mode->refresh_rate;
                            label = fmt::format("{} ({:.0f} Hz)", name, hz);
                        }
                        pane.add_button({
                                            .text = label,
                                            .isSelected = [name] { return SDL_strcasecmp(cfg().display, name.c_str()) == 0; },
                                        })
                            .on_pressed([name] {
                                select_display(name.c_str());
                            });
                    }
                    SDL_free(ids);
                }
                pane.add_rml("<br/>Choose which monitor shows the game. Auto picks your fastest screen. The window moves immediately.");
            });
#endif
}

void SettingsWindow::build_sound(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);


        leftPane.add_section("Volume");
        leftPane.register_control(
            leftPane.add_child<SliderButton>(SliderButton::Props {
                .key = "Master Volume",
                .getValue = [] { return cfg().masterVolume; },
                .setValue =
                    [](int value) {
                        cfg().masterVolume = std::clamp(value, 0, 100);
                        cfg_save();
                        apply_volume();
                    },
                .isModified = [] { return cfg().masterVolume != 100; },
                .min = 0,
                .max = 100,
                .step = 5,
                .suffix = "%",
            }),
            rightPane, [](Pane &pane) {
                pane.clear();
                pane.add_text("Adjusts the volume of all sounds in the game. Applies immediately.");
            });

        leftPane.add_section("Sound Effects");
        config_bool_select(leftPane, rightPane,
            {
                .key = "More Sound Effects",
                .helpText = "Allow more sounds to play at once, so fewer effects cut out during busy moments. Turn off to use the original sound limit.",
                .getValue = [] { return cfg().sfxVoices >= 32; },
                .setValue = [](bool value) { cfg().sfxVoices = value ? 32 : 16; },
                .isDisabled = [] { return getenv("MP6_ENH_SFX_VOICES") != NULL; },
                .isModified = [] { return cfg().sfxVoices != enh_shipped().sfxVoices; },
            });

        leftPane.add_section("Microphone");
        config_bool_select(leftPane, rightPane,
            {
                .key = "Use Microphone",
                .helpText = "Play the Mic games with your own voice, like the GameCube Microphone. Hold the Mic button while you speak: "
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(MP6_SETTINGS_TOUCH_QA)
                            "the on-screen Mic button, or either stick click on a controller."
#else
                            "M on the keyboard, or either stick click on a controller."
#endif
                            " Your device's own speech recognition listens for the game's words."
                            "<br/><br/>Turn off to answer with the controller instead (the game's own no-Mic choices). Takes effect the next time the game checks for a Mic, for example on the file select or mode select screen.",
                .getValue = [] { return cfg().microphone != 0; },
                .setValue = [](bool value) { cfg().microphone = value ? 1 : 0; },
                .isDisabled = [] { return mp6_mic_env_override() != 0; },
                .isModified = [] { return cfg().microphone == 0; },
            });
}

void SettingsWindow::build_controls(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)

        leftPane.add_section("On-screen Controller");
        config_bool_select(leftPane, rightPane, {
            .key = "Show Touch Controls",
            .helpText = "Play with a full GameCube controller on your screen. Turn off when using a Bluetooth or USB controller. The settings gear stays visible.",
            .getValue = [] { return cfg().touch.enabled != 0; },
            .setValue = [](bool value) { cfg().touch.enabled = value; },
            .isModified = [] { return !cfg().touch.enabled; },
        });
        leftPane.register_control(leftPane.add_child<SliderButton>(SliderButton::Props {
            .key = "Control Size",
            .getValue = [] { return cfg().touch.size; },
            .setValue = [](int value) { cfg().touch.size = std::clamp(value, 75, 150); cfg_save(); },
            .isModified = [] { return cfg().touch.size != 100; },
            .min = 75, .max = 150, .step = 5, .suffix = "%",
        }), rightPane, [](Pane &pane) {
            pane.clear(); pane.add_text("Make buttons and sticks easier to reach. Applies immediately. Use Edit Layout to spread out larger controls.");
            touch_preview(pane);
        });
        leftPane.register_control(leftPane.add_child<SliderButton>(SliderButton::Props {
            .key = "Control Opacity",
            .getValue = [] { return cfg().touch.opacity; },
            .setValue = [](int value) { cfg().touch.opacity = std::clamp(value, 20, 100); cfg_save(); },
            .isModified = [] { return cfg().touch.opacity != 55; },
            .min = 20, .max = 100, .step = 5, .suffix = "%",
        }), rightPane, [](Pane &pane) {
            pane.clear(); pane.add_text("Lower values let you see more of the game through the controller. Pressed buttons light up immediately.");
            touch_preview(pane);
        });
        config_bool_select(leftPane, rightPane, {
            .key = "Floating Sticks",
            .helpText = "Start each stick movement from where your thumb touches its area. Turn off to keep each stick centered in its ring.",
            .getValue = [] { return cfg().touch.floating != 0; },
            .setValue = [](bool value) { cfg().touch.floating = value; },
            .isModified = [] { return cfg().touch.floating != 0; },
        });
        leftPane.add_section("Layout");
        if (mInGame) {
            leftPane.register_control(leftPane.add_button("Edit Layout").on_pressed([] {
                mp6_touch_pad_begin_edit();
            }), rightPane, [](Pane &pane) {
                pane.clear(); pane.add_text("Drag each visible control to a comfortable position, then tap Save. Cancel keeps your previous layout. Controls will not press game buttons while you arrange them.");
            });
        } else {
            leftPane.register_control(leftPane.add_button("Edit Layout During Play"), rightPane, [](Pane &pane) {
                pane.clear(); pane.add_text("Start the game, then open the Settings > Controls > Edit Layout to move controls over the game view.");
            });
        }
        leftPane.register_control(leftPane.add_button("Reset Touch Controls"), rightPane, [](Pane &pane) {
            pane.clear();
            pane.add_text("Restore all buttons, their original positions, size and opacity. Other settings are unchanged.");
            pane.add_button("Restore Defaults").on_pressed([] { cfg().touch = mp6_touch_defaults(); cfg_save(); });
        });
        leftPane.register_control(leftPane.add_button("Using L and R"), rightPane, [](Pane &pane) {
            pane.clear(); pane.add_text("Tap and hold L or R for a full trigger press. Release to let go.");
        });
        leftPane.add_section("Visible Controls");
        for (int i = 0; i < MP6_TOUCH_COUNT; ++i) {
            config_bool_select(leftPane, rightPane, {
                .key = mp6_touch_name(i),
                .helpText = fmt::format("Show or hide {} on the screen. Hidden controls do not respond to touches.", escape(mp6_touch_name(i))),
                .getValue = [i] { return (cfg().touch.visible & (1u << i)) != 0; },
                .setValue = [i](bool value) { if(value) cfg().touch.visible |= 1u << i; else cfg().touch.visible &= ~(1u << i); },
                .isModified = [i] { return !(cfg().touch.visible & (1u << i)); },
            });
        }
#else
    leftPane.add_section("Controller & keyboard");
    leftPane.register_control(leftPane.add_button("Controller guide"), rightPane, [](Pane &pane) {
        pane.add_section("Ready to play");
        pane.add_text("Play with a USB or Bluetooth controller, or with the keyboard and mouse (see Keyboard controls). Each minigame shows the buttons it needs.");
    });
    leftPane.register_control(leftPane.add_button("Menu controls"), rightPane, [](Pane &pane) {
        pane.add_section("Make yourself at home");
        pane.add_text("Use the arrow keys to move and adjust, Enter to choose, and Esc to go back. You can also click any option with your mouse. Number keys 1-5 switch categories.");
    });
    leftPane.register_control(leftPane.add_button("Keyboard controls"), rightPane, [](Pane &pane) {
        pane.add_section("Player one");
        pane.add_rml("<b>W A S D</b> Control stick (hold <b>Shift</b> to tilt it halfway)<br/>"
                     "<b>Space / J / left click</b> A button<br/><b>K / right click</b> B button<br/>"
                     "<b>L</b> X button<br/><b>I</b> Y button<br/><b>O</b> Z button<br/>"
                     "<b>Q / E</b> L and R triggers<br/><b>Enter</b> Start<br/>"
                     "<b>Arrow keys</b> Directional pad<br/><b>Numpad 8 4 6 2</b> C stick<br/>"
                     "<b>M</b> Mic button (hold while you speak)<br/><br/>"
                     "<b>Z</b> and <b>X</b> also work as A and B. A second player uses a controller.");
    });
#endif
    rightPane.add_section("Your controls");
    rightPane.add_text("Choose an option to see more.");
}

void SettingsWindow::build_game(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);


        leftPane.add_section("Game Files");
        auto &selectGame = leftPane.add_button("Select Game...");
        selectGame.set_disabled(mInGame);
        leftPane.register_control(selectGame.on_pressed([this] {
            if (!mInGame) push(std::make_unique<ContentSetup>());
        }),
            rightPane, [this](Pane &pane) {
                pane.clear();
                pane.add_text("Choose your Mario Party 6 (USA) disc image or extracted game folder. "
                              "Your memory-card saves are kept separately.");
                if (mInGame) {
                    pane.add_rml("<br/>Close the game and use Select Game on the launch screen "
                                 "to change the game files.");
                }
            });

#if !defined(__ANDROID__) && !defined(MP6_SETTINGS_TOUCH_QA)
        leftPane.add_section("Save Data");
        leftPane.register_control(leftPane.add_button("Open Save Folder").on_pressed([] {
            char url[1200];
            const char *dir = save_dir_abs();
            int written = snprintf(url, sizeof(url), "file:///%s", dir != nullptr ? dir : "");
            if (written < 0 || static_cast<size_t>(written) >= sizeof(url)) return;
            for (char *p = url; *p; p++) {
                if (*p == '\\') *p = '/';
            }
            if (!SDL_OpenURL(url)) {
                push_toast({ .type = "save-folder", .title = "Could Not Open Folder",
                    .content = "Your saves are safe. Open the saves folder beside the game manually.",
                    .duration = std::chrono::seconds(4) });
            }
        }), rightPane, [](Pane &pane) {
            pane.clear();
            pane.add_text("Open your memory-card saves to back them up. These saves keep "
                          "your progress between game updates; save states may not.");
        });
#endif
    if (mInGame) {


            leftPane.add_section("Before You Load");
            leftPane.register_control(leftPane.add_button("Trusted files only"),
                rightPane, [](Pane &pane) {
                    pane.clear();
                    pane.add_rml("Save states let you resume an exact moment. Load only states you "
                                 "created or received from someone you fully trust: unsafe files can "
                                 "harm your device. States may stop working after a game update. "
                                 "Keep using the game's normal memory-card saves too.");
                });

            for (int slot = 1; slot <= 5; ++slot) {
                char path[520];
                const bool pathValid = mp6_savestate_slot_file(slot, path, sizeof(path)) == 0;
                const Rml::String status = pathValid ? slot_status(path) : "invalid path";
                leftPane.add_section(fmt::format("Slot {} \xE2\x80\x94 {}", slot, status));

                leftPane.register_control(leftPane.add_button(fmt::format("Save to Slot {}", slot)).on_pressed([slot] {
                    char p[520];
                    if (mp6_savestate_slot_file(slot, p, sizeof(p)) != 0) {
                        push_toast({ .type = "savestate", .title = "Save State",
                            .content = "Savestate path is too long.",
                            .duration = std::chrono::seconds(4) });
                        return;
                    }
                    mp6_savestate_request_save_path(p);
                }),
                    rightPane, [slot](Pane &pane) {
                        pane.clear();
                        pane.add_rml(fmt::format("Save this moment to slot {}. Replaces the previous state "
                                                 "in this slot, but does not change your memory-card saves. "
                                                 "Saving may briefly pause the game.", slot));
                    });

                leftPane.register_control(leftPane.add_button(fmt::format("Load Slot {}", slot)).on_pressed([slot] {
                    char p[520];
                    if (mp6_savestate_slot_file(slot, p, sizeof(p)) != 0) {
                        push_toast({ .type = "savestate", .title = "Load State",
                            .content = "Savestate path is too long.",
                            .duration = std::chrono::seconds(4) });
                        return;
                    }
                    const int probe = mp6_savestate_probe(p);
                    if (probe == MP6_SAVESTATE_ERR_IO) {
                        push_toast({ .type = "savestate", .title = "Load State",
                            .content = fmt::format("Slot {} is empty.", slot),
                            .duration = std::chrono::seconds(4) });
                        return;
                    }
                    if (probe == MP6_SAVESTATE_ERR_BINARY_MISMATCH) {
                        push_toast({ .type = "savestate", .title = "Load State",
                            .content = fmt::format("Slot {} is from another version and cannot be loaded.", slot),
                            .duration = std::chrono::seconds(4) });
                        return;
                    }
                    mp6_savestate_request_load_path(p);
                }),
                    rightPane, [slot, status](Pane &pane) {
                        pane.clear();
                        Rml::String text = fmt::format("Resume the moment saved in slot {}. Progress made "
                                                       "since that state will be lost.", slot);
                        if (status == "empty") {
                            text += "<br/><br/>This slot is empty.";
                        }
                        else if (status == "from another version") {
                            text += "<br/><br/>This state is from another version and cannot be loaded. "
                                    "Use your normal memory-card save instead.";
                        }
                        pane.add_rml(text);
                    });
            }

            leftPane.add_section("Quick Save");
            leftPane.register_control(leftPane.add_button(
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
                "Quick Save"
#else
                "Quick Save (F5)"
#endif
            ).on_pressed([] {
                mp6_savestate_request_save();
            }),
                rightPane, [](Pane &pane) {
                    pane.clear();
                    pane.add_text("Save this moment to the quick-save slot, replacing its previous state.");
                });
            leftPane.register_control(leftPane.add_button(
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
                "Quick Load"
#else
                "Quick Load (F8)"
#endif
            ).on_pressed([] {
                mp6_savestate_request_load();
            }),
                rightPane, [](Pane &pane) {
                    pane.clear();
                    pane.add_text("Resume your quick save. Progress made since that state will be lost.");
                });
    }
}

void SettingsWindow::build_extras(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
        leftPane.add_section("Game Speed");

        config_bool_select(leftPane, rightPane,
            {
                .key = "Fast Forward",
                .helpText = "Speed up the entire game as fast as your device allows. This changes gameplay speed, not just FPS, and can make play difficult.<br/><br/>For smoother motion at normal speed, use Unlocked FPS in Picture instead. Turn off to return to normal speed.",
                .getValue = [] { return cfg().tickHz == 0.0; },
                .setValue = [](bool value) {
                    cfg().tickHz = value ? 0.0 : 60.0;
                    mp6_tick_rate_refresh();
                },
                .isDisabled = [] { return getenv("MP6_TICK_HZ") != NULL; },
                .isModified = [] { return cfg().tickHz != 60.0; },
            });

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Quick Party",
                                      .getValue = [] {
                                          const int speed = cfg().quickPlay;
                                          return Rml::String { speed > MP6_QUICK_PLAY_OFF ? fmt::format("{}x", speed) : "Off" };
                                      },
                                      .isDisabled = [] { return mp6_quick_play_env_override() != 0; },
                                      .isModified = [] { return cfg().quickPlay != MP6_QUICK_PLAY_OFF; },
                                  }),
            rightPane, [](Pane &pane) {
                pane.clear();
                for (int speed = MP6_QUICK_PLAY_OFF; speed <= MP6_QUICK_PLAY_MAX; speed++) {
                    pane.add_button({
                            .text = speed > MP6_QUICK_PLAY_OFF ? fmt::format("{}x", speed) : "Off",
                            .isSelected = [speed] { return cfg().quickPlay == speed; },
                        })
                        .on_pressed([speed] {
                            cfg().quickPlay = speed;
                            cfg_save();
                        });
                }
                pane.add_rml("<br/>Speed up board play: walking, camera moves, animations, event pauses "
                             "and CPU turns run at the chosen speed. Minigames, their instructions "
                             "and menus stay at normal speed, and the game slows back down whenever "
                             "it waits for you to choose, hit a block or stop a roulette."
                             "<br/><br/>Text appears instantly and lines that need no answer move on "
                             "by themselves. Questions still wait for you. The game also starts "
                             "straight at save-file selection, skipping the intro."
                             "<br/><br/>Works the same on any screen or device. Applies right away.");
            });

        leftPane.add_section("Camera");

        config_bool_select(leftPane, rightPane,
            {
                .key = "Free Camera",
                .helpText = "Explore the scene with a freely moving camera while the game keeps running.<br/><br/>"
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
                            "Drag with one finger to look, drag with two to move, and pinch to move closer or farther away."
#else
                            "W/A/S/D to move, Q/E for down/up. Hold the right mouse button to look. Scroll to move closer or farther away; Shift moves faster and Ctrl slower. A controller's right stick also looks around."
#endif
                            "<br/><br/>Turn off to return to the game's camera. Always starts off when you launch the game.",
                .getValue = [] { return mp6_freecam_enabled() != 0; },
                .setValue = [](bool value) { mp6_freecam_set_enabled(value ? 1 : 0); },
                .isDisabled = [this] { return !mInGame; },
                .isModified = [] { return mp6_freecam_enabled() != 0; },
            });

        leftPane.add_section("Unlocks");
        config_bool_select(leftPane, rightPane,
            {
                .key = "Unlock All",
                .helpText = "Play every board, character, minigame and Star Bank extra without unlocking them first. Star Bank stars are not changed.<br/><br/>Your save keeps its own progress: turn this off to return to what you have unlocked yourself. Unlocks earned while this is on are not recorded.<br/><br/>Menus that are already open update when you next enter them.",
                .getValue = [] { return cfg().unlockAll != 0; },
                .setValue = [](bool value) { cfg().unlockAll = value ? 1 : 0; },
                .isDisabled = [] { return mp6_unlock_all_env_override() != 0; },
                .isModified = [] { return cfg().unlockAll != 0; },
            });

#ifndef __EMSCRIPTEN__ /* the browser build always runs the retail heaps (mp6_enh_heap_scale) */
        leftPane.add_section("Compatibility");
        config_bool_select(leftPane, rightPane,
            {
                .key = "Extra Memory",
                .helpText = "Give demanding scenes and higher shadow settings more memory. Leave on unless you need to reduce memory use.<br/><br/>Restart required if the game is already running.",
                .getValue = [] { return cfg().heapScale >= 4; },
                .setValue = [](bool value) { cfg().heapScale = value ? 4 : 1; },
                .isDisabled = [] { return getenv("MP6_ENH_HEAP_SCALE") != NULL; },
                .isModified = [] { return cfg().heapScale != enh_shipped().heapScale; },
            });
#endif
        // Auto plus at least two real choices: a single fixed renderer is not a setting.
        if (available_backends().size() > 2) {
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Graphics Driver",
                .getValue = [] { return Rml::String { backend_name(configured_backend()) }; },
                .isModified = [] { return strcmp(cfg().backend, "auto") != 0; },
            }),
            rightPane, [](Pane &pane) {
                const auto availableBackends = available_backends();
                for (const auto backend : availableBackends) {
                    pane.add_button({
                                        .text = Rml::String { backend_name(backend) },
                                        .isSelected = [backend] { return configured_backend() == backend; },
                                    })
                        .on_pressed([backend] {
                            snprintf(cfg().backend, sizeof(cfg().backend), "%s", std::string { backend_id(backend) }.c_str());
                            cfg_save();
                        });
                }
                pane.add_rml("<br/>Leave on Auto unless you see graphics problems or crashes. Try another driver if needed. Restart required.");
            });
        }
    leftPane.add_section("About");
    leftPane.register_control(leftPane.add_button("About this port"), rightPane, [](Pane &pane) {

        pane.add_section("Starlit Party");
        pane.add_text(fmt::format("Version {}", port_version()));
        pane.add_rml("Starlit Party is an in-progress native port of Mario Party 6 for computers, Android and the web. "
                     "<br/><br/>"
                     "Menu design and framework: TwilitRealm / Mario Party R&amp;D (Partyboard). "
                     "Built with the Mario Party 6 decompilation and Aurora.<br/><br/>"
                     "Requires game files from your own copy of Mario Party 6 (USA). "
                     "No Nintendo game assets are included.");
    });
}

bool SettingsWindow::focus_content()
{
    /* [MP6] focus the first row of the active tab's first pane (used by the
     * prelaunch first-button jump to land on CONTENT ROOT). */
    if (!mContentComponents.empty()) {
        const bool focused = mContentComponents.front()->focus();
        // A rebuilt chapter can restore an already-focused row without a new
        // Focus event. Refresh its guide and presentation after restoration.
        if (focused) mStoryStateDirty = true;
        return focused;
    }
    return false;
}

void SettingsWindow::update()
{
    // The in-game Settings instance stays on the document stack while hidden.
    // Its retained controls and live preview do not need a frame pump until it
    // is shown again. Keep pumping a closing book so its exit animation can
    // finish after input has already been released by visible().
    if (!Window::visible() && !mClosing) return;

    const auto &config = cfg();
    StorybookSource source;
    source.aa = config.aa;
    source.ambientOcclusion = config.ambientOcclusion;
    source.fpsCorner = config.fpsCorner;
    source.heapScale = config.heapScale;
    source.masterVolume = config.masterVolume;
    source.menuMotion = config.menuMotion;
    source.sfxVoices = config.sfxVoices;
    source.shadowQuality = config.shadowQuality;
    source.showFps = config.showFps;
    source.unlockedFps = config.unlockedFps;
    source.smoothFrames = config.smoothFrames;
    source.vsync = config.vsync;
    source.widescreen = config.widescreen;
    source.windowMode = config.windowMode;
    source.touchEnabled = config.touch.enabled;
    source.touchSize = config.touch.size;
    source.touchOpacity = config.touch.opacity;
    source.touchFloating = config.touch.floating;
    source.touchVisible = config.touch.visible;
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
    const bool touchLayoutChanged = !mTouchLayoutSourceValid ||
        std::memcmp(config.touch.x, mTouchLayoutSource.x, sizeof(config.touch.x)) != 0 ||
        std::memcmp(config.touch.y, mTouchLayoutSource.y, sizeof(config.touch.y)) != 0;
    mTouchLayoutSource = config.touch;
    mTouchLayoutSourceValid = true;
#else
    constexpr bool touchLayoutChanged = false;
#endif
    source.freecam = mInGame ? mp6_freecam_enabled() : 0;
    source.unlockAll = config.unlockAll;
    source.quickPlay = config.quickPlay;
    source.microphone = config.microphone;
    // Launch-only overrides never change during this process. In particular,
    // do not repeatedly lock/search the CRT environment on every interpolated
    // UI present. Live preference values above still participate in dirtiness.
    static const unsigned launchDisabledFlags =
        (std::getenv("MP6_FREE_ASPECT") || std::getenv("MP6_ENH_WIDESCREEN") ? 1u << 0 : 0) |
        (std::getenv("MP6_MSAA") || std::getenv("MP6_FXAA") || std::getenv("MP6_SSAA") ||
             std::getenv("MP6_ENH_AA") ? 1u << 1 : 0) |
        (std::getenv("MP6_ENH_AMBIENT_OCCLUSION") ? 1u << 2 : 0) |
        (std::getenv("MP6_SHADOW_QUALITY") || std::getenv("MP6_ENH_SHADOW_QUALITY") ? 1u << 3 : 0) |
        (std::getenv("MP6_UNLOCKED_FPS") || std::getenv("MP6_ENH_UNLOCKED_FPS") ? 1u << 4 : 0) |
        (std::getenv("MP6_ENH_SFX_VOICES") ? 1u << 5 : 0) |
        (std::getenv("MP6_TICK_HZ") ? 1u << 6 : 0) |
        (std::getenv("MP6_ENH_HEAP_SCALE") ? 1u << 7 : 0);
    source.disabledFlags = launchDisabledFlags;
    source.windowScale = config.windowScale;
    source.tickHz = config.tickHz;
    std::memcpy(source.backend, config.backend, sizeof(source.backend));
    source.backend[sizeof(source.backend) - 1] = '\0';
    std::memcpy(source.display, config.display, sizeof(source.display));
    source.display[sizeof(source.display) - 1] = '\0';
    const bool sourceChanged = !mStorySourceValid || !(source == mStorySource);
    if (sourceChanged) {
        const bool motionChanged = !mStorySourceValid || source.menuMotion != mStorySource.menuMotion;
        mStorySource = source;
        mStorySourceValid = true;
        mStoryRowsDirty = true;
        mStoryStateDirty = true;
        if (motionChanged) mRoot->SetClass("quiet-motion", !source.menuMotion);
    }

    auto *context = mDocument->GetContext();
    const auto size = context->GetDimensions();
    const float dpi = context->GetDensityIndependentPixelRatio();
    // Use physical viewport AND the platform's density. Text is rasterized at
    // its final size, not magnified with a transform or a low-resolution bitmap.
    const float fontPx = settings_font_pixels(size, dpi);
    const bool fontChanged = fontPx != mFontPixels;
    if (fontChanged) {
        mFontPixels = fontPx;
        mRoot->SetProperty("font-size", fmt::format("{}px", fontPx));
    }
    const bool controlViewportChanged = size.x != mControlViewportWidth ||
        size.y != mControlViewportHeight || dpi != mControlViewportDensity;
    mControlViewportWidth = size.x;
    mControlViewportHeight = size.y;
    mControlViewportDensity = dpi;
    // StorybookView owns the live photo's scene-aspect and viewport layout.
    // Do not resize the hidden legacy preview: display:none measures zero,
    // so a retry-until-sized loop forces document layout on every present.
    if (mDetailFocusPending > 0 && --mDetailFocusPending == 0 && mContentComponents.size() > 1) {
        mDocument->UpdateDocument();
        if (mContentComponents[1]->focus()) mStoryStateDirty = true;
    } else if (mDetailFocusPending < 0 && ++mDetailFocusPending == 0) {
        mDocument->UpdateDocument();
        focus_content();
    }
    if (mInGame) {
        const unsigned int gen = mp6_savestate_ui_generation();
        // Keep a generation change pending while the storybook is hidden or
        // closing.  Consuming it before visible() becomes true would skip
        // rebuilding the Save-State tab when the book is shown again.
        if (visible() && gen != mSeenSavestateGen) {
            mSeenSavestateGen = gen;
            refresh_active_tab();
        }
    }
    // Window::update() also walks every settings control and re-runs each
    // getter/predicate.  Keep its safe-area and document portions on every
    // frame, but only walk controls when a source, focus/tab transition,
    // layout, or deferred focus can have changed their rendered state.
    const Insets oldBodyPadding = mBodyPadding;
    update_safe_area();
    const bool safeAreaChanged = !(mBodyPadding == oldBodyPadding);
    const bool controlsDirty = mStoryRowsDirty || mStoryStateDirty || sourceChanged || touchLayoutChanged ||
        fontChanged || controlViewportChanged || safeAreaChanged || mDetailFocusPending != 0 ||
        mRoot->IsClassSet("detail-open");
    if (controlsDirty) {
        for (const auto &component : mContentComponents) {
            component->update();
        }
        mBackButton->update();
        mHelpButton->update();
        mChooseButton->update();
        mPreviousChapter->update();
        mNextChapter->update();
    }
    // Preserve the Window update contract even while the retained control tree
    // is idle.  Document::update is currently empty, but it is the extension
    // point used by the base window and must remain unconditional.
    Document::update();
    update_storybook();
    if (mClosing && mStorybook->exit_finished()) {
        Window::hide(mPendingClose);
        mClosing = false;
        mRoot->SetClass("book-closing", false);
    }
}

void SettingsWindow::update_storybook()
{
    if (!mStorybook || !mStoryStateDirty) return;

    if (mStoryRowsDirty) {
        mStoryValueLabels.clear();
        mStoryDisabled.clear();
        mStoryCanLeft.clear();
        mStoryCanRight.clear();
        mStoryValueLabels.reserve(mStoryRows.size());
        mStoryDisabled.reserve(mStoryRows.size());
        mStoryCanLeft.reserve(mStoryRows.size());
        mStoryCanRight.reserve(mStoryRows.size());
        for (size_t i = 0; i < mStoryRows.size(); ++i) {
            auto *row = mStoryRows[i];
            auto *value = i < mStoryValues.size() ? mStoryValues[i] : nullptr;
            mStoryValueLabels.push_back(value ? story_text(value->GetInnerRML()) : Rml::String {});
            mStoryDisabled.push_back(row->IsPseudoClassSet("disabled") ? 1 : 0);
            mStoryCanLeft.push_back(row->GetAttribute<bool>("data-can-left", true) ? 1 : 0);
            mStoryCanRight.push_back(row->GetAttribute<bool>("data-can-right", true) ? 1 : 0);
        }
        mStoryRowsDirty = false;
    }

    StorybookState state;
    state.chapter = mStoryChapter;
    state.chapterIndex = mChapterIndex;
    state.motion = mStorySource.menuMotion != 0;
    state.inGame = mInGame;
    state.detail = mRoot->IsClassSet("detail-open");
    state.chapters = {"Picture", "Sound",
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
        "Controls",
#endif
        "Game", "Extras"};
    state.description = mGuideCopy ? story_text(mGuideCopy->GetInnerRML()) : Rml::String {};
    state.rows.reserve(mStoryRows.size());
    for (size_t i = 0; i < mStoryRows.size(); ++i) {
        const bool adjustable = i < mStoryValues.size() && mStoryValues[i] != nullptr;
        state.rows.push_back({
            i < mStoryLabels.size() ? mStoryLabels[i] : Rml::String {},
            i < mStoryValueLabels.size() ? mStoryValueLabels[i] : Rml::String {},
            adjustable,
            i < mStoryDisabled.size() && mStoryDisabled[i] != 0,
            i >= mStoryCanLeft.size() || mStoryCanLeft[i] != 0,
            i >= mStoryCanRight.size() || mStoryCanRight[i] != 0,
        });
        if (state.rows.back().label == "Preset") state.rows.back().label = "Picture Preset";
        if (mStoryRows[i]->IsClassSet("current-row")) {
            state.selected = int(i);
            mStoryFocusRow = mStoryRows[i];
        }
    }
    mStoryStateDirty = false;
    mStorybook->set_state(std::move(state));
}

void SettingsWindow::storybook_pointer(Rml::Event &event,bool click)
{
    if(!visible()||!mStorybook||mStorybook->turning()||mStorybook->entering())return;
    const auto p=mStorybook->design_point(event.GetParameter<float>("mouse_x",-100),event.GetParameter<float>("mouse_y",-100));
    const float x=p.x,y=p.y;
    if(click&&y<105&&y>=0){const float tw=kSettingsChapterCount>4?218.f:245.f,start=(1672-kSettingsChapterCount*tw-(kSettingsChapterCount-1)*16)/2;
        for(int i=0;i<kSettingsChapterCount;++i)if(x>=start+i*(tw+16)&&x<=start+i*(tw+16)+tw){set_active_tab(i);return;}}
    const float footerLift=mStorybook->footer_lift(); // prompt strip lifted off the gesture zone
    if(click&&y>=877-footerLift&&y<=941-footerLift){
        if(x>=520&&x<710)mChooseButton->root()->DispatchEvent(Rml::EventId::Click,{});
        else if(x>=724&&x<866)mBackButton->root()->DispatchEvent(Rml::EventId::Click,{});
        else if(x>=895&&x<976)mPreviousChapter->root()->DispatchEvent(Rml::EventId::Click,{});
        else if(x>=985&&x<1170)mNextChapter->root()->DispatchEvent(Rml::EventId::Click,{});
        return;
    }
    if(mRoot->IsClassSet("detail-open"))return;
    int selected=0;for(size_t i=0;i<mStoryRows.size();++i)if(mStoryRows[i]->IsClassSet("current-row"))selected=int(i);
    const float localY=StorybookView::unwarp_y(x,y);
    if(click&&mStoryRows.size()>6&&x>365&&x<575&&localY>=625&&localY<680){
        const int pageCount=(int(mStoryRows.size())+5)/6;
        int index=((selected/6+(x<470?pageCount-1:1))%pageCount)*6;
        while(index<int(mStoryRows.size())&&mStoryRows[index]->IsPseudoClassSet("disabled"))++index;
        if(index<int(mStoryRows.size())) {
            mStoryRows[index]->Focus();
            // Also refresh when the pointer restores an already-focused row.
            mStoryStateDirty = true;
        }
        return;
    }
    const int index=selected/6*6+int(std::floor((localY-147)/73));
    if(x<=140||x>=812||localY<147||localY>=147+6*73||index<0||index>=int(mStoryRows.size()))return;
    auto* row=mStoryRows[index];if(row->IsPseudoClassSet("disabled"))return;row->Focus();
    mStoryStateDirty = true;
    if(click){
        if(x>540){Rml::ElementList steps;row->QuerySelectorAll(steps,"choice-step");if(steps.size()==2){steps[x<595?0:1]->DispatchEvent(Rml::EventId::Click,{});return;}}
        row->DispatchEvent(Rml::EventId::Click,{});
    }
}

bool SettingsWindow::consume_close_request()
{
    /* [MP6] the persistent in-game instance HIDES on Cancel/close and stays
     * on the document stack, so the next F10/F1/gear press re-summons the
     * same window (state, tab, scroll intact). Pre-boot instances keep the
     * ripped pop-on-close behavior. */
    if (mInGame) {
        hide(false);
        return true;
    }
    return false;
}

} // namespace mp6::ui
