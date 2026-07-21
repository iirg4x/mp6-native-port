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
#include "string_button.hpp"
#include "ui.hpp"

#include <SDL3/SDL_misc.h> /* [MP6] SDL_OpenURL for the save-folder action */
#include <SDL3/SDL_filesystem.h> /* [MP6] SDL_GetPathInfo for savestate slot stat */
#include <SDL3/SDL_time.h>       /* [MP6] SDL_TimeToDateTime for slot timestamps */

#include <algorithm>
#include <array>
#include <aurora/aurora.h>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>

#include "mp6_freecam.h"   /* [MP6] Mods tab: freecam toggle (shim/include) */
#include "mp6_savestate.h" /* [MP6] Save States tab: slot request/probe seam */

/* [MP6] Mods tab, live Widescreen flip (aurora_bridge.c; see the Mods tab
 * builder below for the honest live-vs-scene-load semantics). */
extern "C" void mp6_widescreen_set_enabled(int enabled);
extern "C" void mp6_bridge_apply_content_aspect_policy(int aspectLockedCfg);

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md): host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {
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
            return "incompatible (other build)";
        }
        if (probe != MP6_SAVESTATE_OK) {
            return "unreadable (not a savestate)";
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

    /* ------------------------------------------------------------------
     * VIDEO ([MP6] content: window mode/size, 4:3 lock, FPS corner,
     * backend, vsync -- the L3 row set on their widgets)
     * ------------------------------------------------------------------ */
    add_tab("Video", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Display");

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
                pane.add_rml("<br/>Applies immediately. Fullscreen is borderless desktop fullscreen.");
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
                pane.add_rml("<br/>Client-area presets (4:3). Applies immediately in windowed mode.");
            });

        /* [MP6] the "Lock 4:3 Aspect Ratio" row was REMOVED here (user
         * direction: redundant -- Widescreen on the Mods tab is the one
         * aspect switch). The config key is simply no longer read or
         * written (the tolerant parser ignores it in old files); the
         * internal default stays locked-4:3 whenever Widescreen is off,
         * and the MP6_FREE_ASPECT env lever keeps absolute priority.
         * The Dynamic Widescreen toggle itself moved to the Mods tab. */

        /* Their FPS row, verbatim mechanism over our two keys. */
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

        leftPane.add_section("Graphics");

        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Graphics Backend",
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
                pane.add_rml("<br/>Takes effect next launch.");
            });

        config_bool_select(leftPane, rightPane,
            {
                .key = "VSync",
                .helpText = "Synchronizes the frame rate to your monitor's refresh rate.<br/><br/>"
                            "Takes effect next launch.",
                .getValue = [] { return cfg().vsync != 0; },
                .setValue = [](bool value) { cfg().vsync = value ? 1 : 0; },
                .isModified = [] { return cfg().vsync != 1; },
            });
    });

    /* ------------------------------------------------------------------
     * AUDIO ([MP6] content: master volume)
     * ------------------------------------------------------------------ */
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
    });

    /* ------------------------------------------------------------------
     * GAME ([MP6] content: content root + save dir + tick rate)
     * ------------------------------------------------------------------ */
    add_tab("Game", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Content");

        leftPane.register_control(
            leftPane.add_child<StringButton>(StringButton::Props {
                .key = "Content Root",
                .getValue = [] { return Rml::String { cfg().contentRoot }; },
                .setValue =
                    [](Rml::String value) {
                        snprintf(cfg().contentRoot, sizeof(cfg().contentRoot), "%s", value.c_str());
                        cfg_save();
                        refresh_content_state();
                    },
                .maxLength = 1000,
            }),
            rightPane, [](Pane &pane) {
                pane.clear();
                char autoRoot[1100];
                const bool haveAuto = auto_root(autoRoot, sizeof(autoRoot)) != 0;
                char err[1200];
                const int state = validate_root(cfg().contentRoot, err, sizeof(err));
                Rml::String text = "Folder of the extracted GameCube disc (containing sys/fst.bin and "
                                   "files/). Leave empty to auto-detect.";
                if (state > 0) {
                    text += "<br/><br/>Configured root: valid.";
                }
                else if (state < 0) {
                    text += fmt::format("<br/><br/>Configured root: INVALID ({}).", escape(err));
                }
                else if (haveAuto) {
                    text += fmt::format("<br/><br/>Auto-detected: {}", escape(autoRoot));
                }
                else {
                    text += "<br/><br/>No content found -- set this path to your extracted disc.";
                }
                pane.add_rml(text);
            });

        /* [MP6] A4 (docs/A4_ANDROID_UI.md): the guided import path -- disc
         * image (nod-backed) or extracted folder through the system picker,
         * same dialog the prelaunch "Select Game" button opens. */
        leftPane.register_control(leftPane.add_button("Select Game...").on_pressed([this] {
            push(std::make_unique<ContentSetup>());
        }),
            rightPane, [](Pane &pane) {
                pane.clear();
                pane.add_text("Import the game files from a Mario Party 6 (USA) disc image "
                              "(.iso/.gcm/.rvz) or an already-extracted GP6E01 folder.");
            });

        leftPane.register_control(leftPane.add_button("Clear Path").on_pressed([] {
            cfg().contentRoot[0] = '\0';
            cfg_save();
            refresh_content_state();
        }),
            rightPane, [](Pane &pane) {
                pane.clear();
                pane.add_text("Clear the configured content root and return to automatic detection.");
            });

        leftPane.add_section("Save Data");

        leftPane.register_control(leftPane.add_button("Open Save Folder").on_pressed([] {
            char url[1200];
            snprintf(url, sizeof(url), "file:///%s", save_dir_abs());
            for (char *p = url; *p; p++) {
                if (*p == '\\') *p = '/';
            }
            SDL_OpenURL(url);
        }),
            rightPane, [](Pane &pane) {
                pane.clear();
                pane.add_rml(fmt::format("Memory-card saves (GCI folder) live at:<br/><br/>{}", escape(save_dir_abs())));
            });

        leftPane.add_section("Timing");

        config_bool_select(leftPane, rightPane,
            {
                .key = "Free-Run Tick",
                .helpText = "Run the game loop uncapped instead of the standard 60 Hz tick (debug "
                            "lever; T1 throttle contract).<br/><br/>Takes effect next launch. Disabled "
                            "while the MP6_TICK_HZ environment lever is set (it wins).",
                .getValue = [] { return cfg().tickHz == 0.0; },
                .setValue = [](bool value) { cfg().tickHz = value ? 0.0 : 60.0; },
                .isDisabled = [] { return getenv("MP6_TICK_HZ") != NULL; },
                .isModified = [] { return cfg().tickHz != 60.0; },
            });
    });

    /* ------------------------------------------------------------------
     * MODS ([MP6] content: freecam + widescreen -- runtime toggles)
     * ------------------------------------------------------------------ */
    add_tab("Mods", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Camera");

        config_bool_select(leftPane, rightPane,
            {
                .key = "Freecam",
                .helpText = "Fly the 3D camera freely; the game keeps running (its input is "
                            "paused only while this menu is open).<br/><br/>"
                            "Desktop: W/A/S/D move, Q/E down/up, hold RIGHT MOUSE and move "
                            "to look, wheel dollies, SHIFT fast, CTRL slow. Gamepad: right "
                            "stick looks. Touch: one-finger drag looks, two-finger drag "
                            "moves, pinch dollies.<br/><br/>Turning it off hands the camera "
                            "straight back to the game. Not saved to the config -- freecam "
                            "always starts a session off.",
                .getValue = [] { return mp6_freecam_enabled() != 0; },
                .setValue = [](bool value) { mp6_freecam_set_enabled(value ? 1 : 0); },
                .isDisabled = [this] { return !mInGame; },
                .isModified = [] { return mp6_freecam_enabled() != 0; },
            });

        leftPane.add_section("Display");

        /* WS2 (docs/WS2_DYNAMIC_WIDESCREEN.md): dynamic true-widescreen --
         * NOT the fixed 16:9 a binary ROM patch is stuck with. Moved here
         * from the Video tab; flipping it MID-GAME now also flips the live
         * engine flag (window shape, render width, 2D layer track it
         * within a tick) -- but scene-load-time registrations (per-scene
         * camera widening, backdrop extension strips, the floor dup) only
         * register when a scene loads with Widescreen on, so the full
         * effect lands on the next scene change. Said in the help text
         * rather than papered over. */
        config_bool_select(leftPane, rightPane,
            {
                .key = "Dynamic Widescreen",
                .helpText = "Render at the window's own live aspect ratio instead of fixed 4:3 -- "
                            "widen the window to any shape (16:9, 21:9, ...) and the 3D camera/HUD "
                            "track it continuously, no stretching.<br/><br/>Flipping it during play "
                            "applies the window shape, render width and HUD immediately; the "
                            "scene's own cameras and widescreen backdrop extensions register at "
                            "scene load, so it fully applies on the next scene change.<br/><br/>"
                            "Disabled while the MP6_FREE_ASPECT environment lever is set (it keeps "
                            "absolute priority).",
                .getValue = [] { return cfg().widescreen != 0; },
                .setValue =
                    [inGame = mInGame](bool value) {
                        cfg().widescreen = value ? 1 : 0;
                        apply_display();
                        if (inGame) {
                            /* Live engine flip. Order matters: the policy
                             * call reads mp6_widescreen_enabled(). NOT done
                             * pre-boot (prelaunch instance): A5 keeps the
                             * content-fit policy off until right before
                             * GameMain so the launcher itself never
                             * letterboxes -- main_native.c applies the
                             * config value on boot as always. */
                            mp6_widescreen_set_enabled(value ? 1 : 0);
                            mp6_bridge_apply_content_aspect_policy(cfg().aspectLocked);
                        }
                    },
                .isDisabled = [] { return getenv("MP6_FREE_ASPECT") != NULL; },
                .isModified = [] { return cfg().widescreen != 0; },
            });

        leftPane.add_section("Rendering");

        /* Shadow Quality (shim/include/mp6_shadow_quality.h has the full
         * contract): raises the real-time projected shadow map's
         * resolution (Hu3DShadow*, game/hsfman.c + game/hsfdraw.c) --
         * same lighting, same shadow shape, strictly more texels sampled
         * through the same live-projected texcoord. Read at shadow-CREATE
         * time, so flipping it mid-game doesn't retroactively resize a
         * map that's already up -- said in the description below rather
         * than papered over, same as Dynamic Widescreen's own row.
         * Their FPS row's exact select_button + register_control shape,
         * five options instead of six. */
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Shadow Quality",
                                      .getValue = [] { return Rml::String { fmt::format("{}x", cfg().shadowQuality) }; },
                                      .isDisabled = [] { return getenv("MP6_SHADOW_QUALITY") != NULL; },
                                      .isModified = [] { return cfg().shadowQuality != 1; },
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
                pane.add_rml("<br/>Raises the texel resolution of the game's real-time projected "
                             "shadows (the blob under characters and hosts) -- same lighting, same "
                             "shadow size and shape, just sharper edges. 1x is the original game's "
                             "own resolution (byte-identical).<br/><br/>"
                             "Right now 2x is the effective ceiling: the shadow view is bounded by "
                             "the GameCube's own framebuffer box, and 2x already captures every "
                             "pixel it renders. Higher settings safely clamp to 2x (logged) until "
                             "the renderer grows a dedicated shadow pass.<br/><br/>"
                             "Takes effect the next time a board or scene loads -- not "
                             "retroactively on a shadow map that's already up.<br/><br/>"
                             "Disabled while the MP6_SHADOW_QUALITY environment lever is set (it "
                             "wins).");
            });
    });

    /* ------------------------------------------------------------------
     * SAVE STATES ([MP6] content: 5 slot files -- in-game instance only;
     * capture/restore must run at the game's own frame boundary, so a
     * pre-boot instance has nothing meaningful to offer)
     * ------------------------------------------------------------------ */
    if (mInGame) {
        add_tab("Save States", [this](Rml::Element *content) {
            auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
            auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

            for (int slot = 1; slot <= 5; ++slot) {
                char path[520];
                mp6_savestate_slot_file(slot, path, sizeof(path));
                const Rml::String status = slot_status(path);
                leftPane.add_section(fmt::format("Slot {} \xE2\x80\x94 {}", slot, status));

                leftPane.register_control(leftPane.add_button("Save").on_pressed([slot] {
                    char p[520];
                    mp6_savestate_slot_file(slot, p, sizeof(p));
                    mp6_savestate_request_save_path(p);
                }),
                    rightPane, [slot, path = Rml::String(path)](Pane &pane) {
                        pane.clear();
                        pane.add_rml(fmt::format("Capture the whole session into slot {} at the next "
                                                 "frame boundary (takes a moment).<br/><br/>{}",
                            slot, escape(path)));
                    });

                leftPane.register_control(leftPane.add_button("Load").on_pressed([slot] {
                    char p[520];
                    mp6_savestate_slot_file(slot, p, sizeof(p));
                    const int probe = mp6_savestate_probe(p);
                    if (probe == MP6_SAVESTATE_ERR_IO) {
                        push_toast({ .type = "savestate", .title = "Load State",
                            .content = fmt::format("Slot {} is empty.", slot),
                            .duration = std::chrono::seconds(4) });
                        return;
                    }
                    if (probe == MP6_SAVESTATE_ERR_BINARY_MISMATCH) {
                        push_toast({ .type = "savestate", .title = "Load State",
                            .content = fmt::format("Slot {} is incompatible (other build).", slot),
                            .duration = std::chrono::seconds(4) });
                        return;
                    }
                    mp6_savestate_request_load_path(p);
                }),
                    rightPane, [slot, status](Pane &pane) {
                        pane.clear();
                        Rml::String text = fmt::format("Restore slot {} over the running session at the "
                                                       "next frame boundary.", slot);
                        if (status == "empty") {
                            text += "<br/><br/>This slot is empty.";
                        }
                        else if (status == "incompatible (other build)") {
                            text += "<br/><br/>This slot was captured by a DIFFERENT build of the "
                                    "port and will refuse to load -- rebuilding invalidates "
                                    "savestates.";
                        }
                        pane.add_rml(text);
                    });
            }

            leftPane.add_section("Default slot");
            leftPane.register_control(leftPane.add_button("Save (F5)").on_pressed([] {
                mp6_savestate_request_save();
            }),
                rightPane, [](Pane &pane) {
                    pane.clear();
                    pane.add_text("The unnumbered default slot the F5/F8 hotkeys use "
                                  "(MP6_SAVESTATE_PATH overrides its location).");
                });
            leftPane.register_control(leftPane.add_button("Load (F8)").on_pressed([] {
                mp6_savestate_request_load();
            }),
                rightPane, [](Pane &pane) {
                    pane.clear();
                    pane.add_text("Load the default slot (same as pressing F8 during play).");
                });
        });
    }

    /* ------------------------------------------------------------------
     * ABOUT ([MP6] content)
     * ------------------------------------------------------------------ */
    add_tab("About", [this](Rml::Element *content) {
        auto &pane = add_child<Pane>(content, Pane::Type::Uncontrolled);
        pane.add_section("Mario Party 6 \xE2\x80\x94 Native Port");
        pane.add_text(fmt::format("Version {}", port_version()));
        pane.add_rml("A native PC port of Mario Party 6 built on the matching decompilation, "
                     "running the game's own code over the aurora GX-to-WebGPU layer.<br/><br/>"
                     "This launcher UI is partyboard's (the Mario Party 4 port's) RmlUi "
                     "implementation, adapted -- see docs/PARTYBOARD_PROVENANCE.md. UI framework "
                     "credit: TwilitRealm / Mario Party R&amp;D.<br/><br/>"
                     "No game assets ship with this port: all art, audio, and data load from "
                     "your own extracted disc at runtime.");
    });

    /* [MP6] initialTab: land on a specific tab (prelaunch's content-root
     * jump passes kSettingsTabGame). */
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
