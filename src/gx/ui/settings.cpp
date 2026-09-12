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

#include "bool_button.hpp"
#include "content_setup.hpp"  /* [MP6] A4: guided content import dialog */
#include "launcher_state.hpp" /* [MP6] our config bridge */
#include "number_button.hpp"
#include "pane.hpp"
#include "ui.hpp"

#include <SDL3/SDL_misc.h> /* [MP6] SDL_OpenURL for the save-folder action */
#include <SDL3/SDL_filesystem.h> /* [MP6] SDL_GetPathInfo for savestate slot stat */
#include <SDL3/SDL_time.h>       /* [MP6] SDL_TimeToDateTime for slot timestamps */
#include <SDL3/SDL_stdinc.h>     /* [MP6] SDL_strcasecmp / SDL_free for the Display row */
#include <SDL3/SDL_video.h>      /* [MP6] the Display row enumerates the live output set */

#include <algorithm>
#include <array>
#include <aurora/aurora.h>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>

#include "mp6_display.h"   /* [MP6] Video tab: the live VSync apply + the Display row */
#include "mp6_enhancements.h" /* [MP6] Enhancements tab: preset table + derivation */
#include "mp6_freecam.h"   /* [MP6] Mods tab: freecam toggle (include) */
#include "mp6_savestate.h" /* [MP6] Save States tab: slot request/probe seam */

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
    /* SSAA 1.5x/2x are DESKTOP-ONLY (P3): the two rows are #ifndef __ANDROID__-
     * gated out of the array itself, so both the label lookup and the button
     * loop below exclude them on Android with no per-site guards -- mirroring
     * the launcher_core.cpp __ANDROID__ UI gating. */
#ifndef __ANDROID__
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
        leftPane.register_control(button, rightPane, [helpText = std::move(props.helpText)](Pane &pane) {
            pane.clear();
            pane.add_rml(helpText);
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

SettingsWindow::SettingsWindow(bool prelaunch, int initialTab, bool inGame)
    : mPrelaunch(prelaunch)
    , mInGame(inGame)
{
    mSeenSavestateGen = mp6_savestate_ui_generation();
    if (prelaunch) {
        mSuppressNavFallback = true;
    }

    add_tab("Video", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

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
                help += "<br/>Also sets More Sound Effects (Audio) and Extra Memory (Advanced). "
                        "Memory changes need a restart during play."
#ifndef MP6_AURORA_LIVE_AA
                        " Some anti-aliasing changes do too."
#endif
                        ;
                pane.add_rml(help);
            });

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

        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Anti-Aliasing",
                                      .getValue =
                                          [] {
                                              for (const auto &p : kAaPresets) {
                                                  if (cfg().aa == p.mode) {
                                                      return Rml::String { p.label };
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
#ifndef __ANDROID__
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

        leftPane.add_section("Smoothness");
        config_bool_select(leftPane, rightPane,
            {
                .key = "Unlocked FPS",
                .helpText = "Make characters and camera movement smoother above 60 FPS without speeding up the game. Menus and some effects still update at 60 FPS.<br/><br/>Turn VSync off for the highest possible FPS. Uses more power; turn this off to save battery. Fast Forward takes priority when enabled in Mods.",
                .getValue = [] { return cfg().unlockedFps != 0; },
                .setValue = [](bool value) { cfg().unlockedFps = value ? 1 : 0; },
                .isDisabled = [] { return getenv("MP6_UNLOCKED_FPS") != NULL || getenv("MP6_ENH_UNLOCKED_FPS") != NULL; },
                .isModified = [] { return cfg().unlockedFps != enh_shipped().unlockedFps; },
            });

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

#ifndef __ANDROID__
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
    });

    add_tab("Audio", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Volume");
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props {
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
    });

    add_tab("Game", [this](Rml::Element *content) {
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

#ifndef __ANDROID__
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
        rightPane.add_text("Minigames are not available yet. End-of-turn minigames are skipped "
                           "without awarding coins, so you can keep playing the board.");
    });

    add_tab("Mods", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Game Speed");

        config_bool_select(leftPane, rightPane,
            {
                .key = "Fast Forward",
                .helpText = "Speed up the entire game as fast as your device allows. This changes gameplay speed, not just FPS, and can make play difficult.<br/><br/>For smoother motion at normal speed, use Unlocked FPS in Video instead. Turn off to return to normal speed.",
                .getValue = [] { return cfg().tickHz == 0.0; },
                .setValue = [](bool value) {
                    cfg().tickHz = value ? 0.0 : 60.0;
                    mp6_tick_rate_refresh();
                },
                .isDisabled = [] { return getenv("MP6_TICK_HZ") != NULL; },
                .isModified = [] { return cfg().tickHz != 60.0; },
            });


        leftPane.add_section("Camera");

        config_bool_select(leftPane, rightPane,
            {
                .key = "Free Camera",
                .helpText = "Explore the scene with a freely moving camera while the game keeps running.<br/><br/>"
#ifdef __ANDROID__
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

    });

    add_tab("Advanced", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
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
    });

#ifdef __ANDROID__
    add_tab("Touch Controls", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
        leftPane.add_section("On-screen Controller");
        config_bool_select(leftPane, rightPane, {
            .key = "Show Touch Controls",
            .helpText = "Play with a full GameCube controller on your screen. Turn off when using a Bluetooth or USB controller. The settings gear stays visible.",
            .getValue = [] { return cfg().touch.enabled != 0; },
            .setValue = [](bool value) { cfg().touch.enabled = value; },
            .isModified = [] { return !cfg().touch.enabled; },
        });
        leftPane.register_control(leftPane.add_child<NumberButton>(NumberButton::Props {
            .key = "Control Size",
            .getValue = [] { return cfg().touch.size; },
            .setValue = [](int value) { cfg().touch.size = std::clamp(value, 75, 150); cfg_save(); },
            .isModified = [] { return cfg().touch.size != 100; },
            .min = 75, .max = 150, .step = 5, .suffix = "%",
        }), rightPane, [](Pane &pane) {
            pane.clear(); pane.add_text("Make buttons and sticks easier to reach. Applies immediately. Use Edit Layout to spread out larger controls.");
        });
        leftPane.register_control(leftPane.add_child<NumberButton>(NumberButton::Props {
            .key = "Control Opacity",
            .getValue = [] { return cfg().touch.opacity; },
            .setValue = [](int value) { cfg().touch.opacity = std::clamp(value, 20, 100); cfg_save(); },
            .isModified = [] { return cfg().touch.opacity != 55; },
            .min = 20, .max = 100, .step = 5, .suffix = "%",
        }), rightPane, [](Pane &pane) {
            pane.clear(); pane.add_text("Lower values let you see more of the game through the controller. Pressed buttons light up immediately.");
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
                pane.clear(); pane.add_text("Start the game, then open the gear > Touch Controls > Edit Layout to move controls over the game view.");
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
    });
#endif

    if (mInGame) {
        add_tab("Save States", [this](Rml::Element *content) {
            auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
            auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

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

                leftPane.register_control(leftPane.add_button("Save").on_pressed([slot] {
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

                leftPane.register_control(leftPane.add_button("Load").on_pressed([slot] {
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
#ifdef __ANDROID__
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
#ifdef __ANDROID__
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
        });
    }

    add_tab("About", [this](Rml::Element *content) {
        auto &pane = add_child<Pane>(content, Pane::Type::Uncontrolled);
        pane.add_section("Mario Party 6 \xE2\x80\x94 Native Port");
        pane.add_text(fmt::format("Version {}", port_version()));
        pane.add_rml("An in-progress native port of Mario Party 6 for computers and Android. "
                     "Minigames are not available yet.<br/><br/>"
                     "Menu design and framework: TwilitRealm / Mario Party R&amp;D (Partyboard). "
                     "Built with the Mario Party 6 decompilation and Aurora.<br/><br/>"
                     "Requires game files from your own copy of Mario Party 6 (USA). "
                     "No Nintendo game assets are included.");
    });

    if (initialTab > 0) {
        set_active_tab(initialTab);
    }
}

bool SettingsWindow::focus_content()
{
    /* [MP6] focus the first row of the active tab's first pane (used by the
     * prelaunch first-button jump to land on CONTENT ROOT). */
    if (!mContentComponents.empty()) {
        return mContentComponents.front()->focus();
    }
    return false;
}

void SettingsWindow::update()
{
    /* [MP6] their prelaunch-verification-modal hook dropped -- no disc
     * verification flow in our content model. */

    /* [MP6] Save States tab: a queued save/load was serviced at a frame
     * boundary since we last looked -- rebuild the ACTIVE tab so slot
     * timestamps/compatibility lines are fresh (cheap: once per serviced
     * request, not per frame; rebuilding a non-saves tab is harmless). */
    if (mInGame) {
        const unsigned int gen = mp6_savestate_ui_generation();
        if (gen != mSeenSavestateGen) {
            mSeenSavestateGen = gen;
            if (visible()) {
                refresh_active_tab();
            }
        }
    }
    Window::update();
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
