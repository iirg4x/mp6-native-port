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

#include <algorithm>
#include <array>
#include <aurora/aurora.h>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>

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

} // namespace

SettingsWindow::SettingsWindow(bool prelaunch, int initialTab)
    : mPrelaunch(prelaunch)
{
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

        config_bool_select(leftPane, rightPane,
            {
                .key = "Lock 4:3 Aspect Ratio",
                .helpText = "Lock the game's aspect ratio to the original. Also constrains the window "
                            "shape while windowed.<br/><br/>Disabled while the MP6_FREE_ASPECT "
                            "environment lever is set (it keeps absolute priority).",
                .getValue = [] { return cfg().aspectLocked != 0; },
                .setValue =
                    [](bool value) {
                        cfg().aspectLocked = value ? 1 : 0;
                        apply_display();
                    },
                .isDisabled = [] { return getenv("MP6_FREE_ASPECT") != NULL; },
                .isModified = [] { return cfg().aspectLocked != 1; },
            });

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

        /* [MP6]  the guided import path -- disc
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
    Window::update();
}

} // namespace mp6::ui
