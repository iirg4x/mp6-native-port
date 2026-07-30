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
#include "mp6_freecam.h"   /* [MP6] Mods tab: freecam toggle (shim/include) */
#include "mp6_path.h"      /* checked content-root assignment */
#include "mp6_savestate.h" /* [MP6] Save States tab: slot request/probe seam */

/* [MP6] Mods tab, live Widescreen flip (aurora_bridge.c; see the Mods tab
 * builder below for the honest live-vs-scene-load semantics). */
extern "C" void mp6_widescreen_set_enabled(int enabled);
extern "C" void mp6_bridge_apply_content_aspect_policy(int aspectLockedCfg);

/* [MP6] Anti-Aliasing: 1 = the selected mode may be pushed to aurora NOW,
 * 0 = it is restart-pending because an init-time mechanism (MSAA/SSAA) already
 * owns this session. See launcher_core.cpp for the full contract. */
extern "C" int mp6_launcher_aa_apply_live(int aa);

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
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

    /* Anti-Aliasing (video.aa -- launcher_state.hpp Mp6AaMode has the full
     * contract): ONE select button drives every AA mechanism. Off and
     * MSAA 4x keep P1's aurora_initialize-time "Takes effect next launch"
     * shape; FXAA is a live post-process (aurora_set_post_aa) that applies
     * the instant it is selected. (SSAA 1.5x/2x are appended, desktop-only,
     * by the P3 lane.) */
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
        { MP6_AA_MSAA4X, "MSAA 4x" },
        { MP6_AA_FXAA, "FXAA" },
        { MP6_AA_SSAA15, "SSAA 1.5x" },
        { MP6_AA_SSAA2X, "SSAA 2x" },
    } };
#else
    constexpr std::array<AaPreset, 3> kAaPresets = { {
        { MP6_AA_OFF, "Off" },
        { MP6_AA_MSAA4X, "MSAA 4x" },
        { MP6_AA_FXAA, "FXAA" },
    } };
#endif

    /* ------------------------------------------------------------------
     * ENHANCEMENTS ([MP6], ours -- not partyboard's).
     *
     * The port ships enhanced by default and every enhancement has a
     * faithful-off switch. The six switches live in ONE named group with a
     * preset selector on top; the preset table and the label derivation are
     * NOT here -- they are pure C in platform/enh/mp6_enhancements.c (which
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
        { MP6_ENH_PRESET_VANILLA, "Vanilla",
          "Exactly the GameCube. Every switch at its retail value -- 4:3, one "
          "present per 60Hz tick, retail shadow map, no anti-aliasing, 16 sound "
          "effect voices, retail heaps." },
        { MP6_ENH_PRESET_VANILLA_PLUS, "Vanilla Plus",
          "The GameCube, smooth and clean. The authored 4:3 composition is kept "
          "exactly as the game framed it; only technical quality goes up -- "
          "smoother motion, anti-aliasing, sharper shadows, and the higher "
          "voice/heap limits." },
        { MP6_ENH_PRESET_MODERN, "Modern",
          "Full modern presentation: everything Vanilla Plus has, plus dynamic "
          "widescreen. This is what a fresh install comes up as." },
    } };

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

    /* The comparison table shown beside the preset row. Built from the same
     * pure preset table the engine uses (mp6_enh_preset_values), so it cannot
     * describe a tier the code does not actually apply. */
    Rml::String enh_preset_matrix_rml()
    {
        auto shadowLabel = [](int q) { return q == 1 ? Rml::String { "retail" } : fmt::format("{}x", q); };
        auto aaLabel = [](int aa) {
            for (const auto &p : kAaPresets) {
                if (p.mode == aa) return Rml::String { p.label };
            }
            return Rml::String { "Off" };
        };
        Rml::String rml = "<div class=\"enh-matrix\">";
        rml += "<div class=\"enh-matrix-row enh-matrix-head\"><div class=\"enh-matrix-label\"></div>";
        for (const auto &tier : kEnhPresetTiers) {
            rml += fmt::format("<div class=\"enh-matrix-val\">{}</div>", escape(tier.label));
        }
        rml += "</div>";

        struct MatrixRow {
            const char *label;
            std::function<Rml::String(const Mp6EnhValues &)> cell;
        };
        const std::array<MatrixRow, 6> rows = { {
            { "Widescreen", [](const Mp6EnhValues &v) { return Rml::String { v.widescreen ? "on" : "off" }; } },
            { "Unlocked FPS", [](const Mp6EnhValues &v) { return Rml::String { v.unlockedFps ? "on" : "off" }; } },
            { "Shadows", [&](const Mp6EnhValues &v) { return shadowLabel(v.shadowQuality); } },
            { "Anti-aliasing", [&](const Mp6EnhValues &v) { return aaLabel(v.aa); } },
            { "SFX voices", [](const Mp6EnhValues &v) { return fmt::format("{}", v.sfxVoices); } },
            { "Heaps", [](const Mp6EnhValues &v) { return v.heapScale == 1 ? Rml::String { "retail" } : fmt::format("{}x", v.heapScale); } },
        } };
        for (const auto &row : rows) {
            rml += fmt::format("<div class=\"enh-matrix-row\"><div class=\"enh-matrix-label\">{}</div>",
                escape(row.label));
            for (const auto &tier : kEnhPresetTiers) {
                Mp6EnhValues v;
                mp6_enh_preset_values(tier.preset, &v);
                rml += fmt::format("<div class=\"enh-matrix-val\">{}</div>", escape(row.cell(v)));
            }
            rml += "</div>";
        }
        rml += "</div>";
        return rml;
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
                            "Applies immediately. Turning it off can show tearing.",
                .getValue = [] { return cfg().vsync != 0; },
                /* LIVE, not next-launch. aurora_enable_vsync() has been in the
                 * pinned aurora and exported all along (include/aurora/gfx.h);
                 * it simply had no caller in this port, which is the only
                 * reason this row ever said "takes effect next launch".
                 * mp6_display_set_vsync is a no-op when the value already
                 * matches, and config_bool_select's own setValue wrapper
                 * early-returns on an unchanged value anyway, so the surface
                 * cannot be reconfigured by merely re-selecting the current
                 * state. video.vsync keeps its meaning as the BOOT state and
                 * is still saved, so the next launch comes up where the
                 * session left off. */
                .setValue = [](bool value) {
                    cfg().vsync = value ? 1 : 0;
                    mp6_display_set_vsync(value ? 1 : 0);
                },
                .isModified = [] { return cfg().vsync != 1; },
            });

        /* Which attached output the window opens on. Next-launch by nature:
         * aurora creates and SHOWS the window inside aurora_initialize, so the
         * choice is consumed before this UI exists. Not restart-PENDING
         * though -- see restart_pending() in launcher_core.cpp for why moving
         * a window must not raise a relaunch prompt. */
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
                        snprintf(cfg().display, sizeof(cfg().display), "auto");
                        cfg_save();
                    });
                pane.add_button({
                                    .text = "Primary",
                                    .isSelected = [] { return SDL_strcasecmp(cfg().display, "primary") == 0; },
                                })
                    .on_pressed([] {
                        snprintf(cfg().display, sizeof(cfg().display), "primary");
                        cfg_save();
                    });
                /* One row per attached output, labelled with its exact refresh
                 * rate so "Auto" is not a black box -- the user can see which
                 * display Auto would pick and pin it by name instead. The list
                 * is built from the LIVE display set, so unplugging a monitor
                 * simply stops offering it (a config still naming it degrades
                 * to auto at launch, with a printed line). */
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
                            label = fmt::format("{} -- {}x{} @ {:.3f} Hz", name, mode->w, mode->h, hz);
                        }
                        pane.add_button({
                                            .text = label,
                                            .isSelected = [name] { return SDL_strcasecmp(cfg().display, name.c_str()) == 0; },
                                        })
                            .on_pressed([name] {
                                snprintf(cfg().display, sizeof(cfg().display), "%s", name.c_str());
                                cfg_save();
                            });
                    }
                    SDL_free(ids);
                }
                pane.add_rml("<br/>Takes effect next launch. Auto picks the attached display "
                             "with the highest refresh rate.<br/><br/>With VSync on, the "
                             "display's refresh rate is the hard cap on how many frames per "
                             "second you can see.");
            });

        /* Anti-Aliasing MOVED OUT of this tab: it is one of the six port
         * ENHANCEMENTS and now sits in that group, under its preset
         * selector. video.aa itself is untouched -- same enum, same
         * live-vs-restart contract, same env levers. VSync stays here: it
         * is a host display preference, not an enhancement. */
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
                        char checked[sizeof(cfg().contentRoot)];
                        if (mp6_path_copy_checked(checked, sizeof(checked), value.c_str()) != 0) {
                            push_toast({ .type = "content", .title = "Content Root",
                                .content = "Path is too long; the previous content root was not changed.",
                                .duration = std::chrono::seconds(4) });
                            return;
                        }
                        memcpy(cfg().contentRoot, checked, strlen(checked) + 1);
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

        /* The guided import path -- disc
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
            const char *dir = save_dir_abs();
            int written = snprintf(url, sizeof(url), "file:///%s", dir != nullptr ? dir : "");
            if (written < 0 || static_cast<size_t>(written) >= sizeof(url)) return;
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
     * ENHANCEMENTS ([MP6], ours). THE named group the whole enhancements
     * policy hangs off: the port ships enhanced by default, and every one of
     * these six has a faithful-off switch. Four of them used to live
     * elsewhere -- Anti-Aliasing on Video, Dynamic Widescreen / Unlocked FPS
     * / Shadow Quality on Mods -- and are rehomed here UNCHANGED: same
     * config keys, same live-vs-next-launch contracts, same env levers.
     * Two are new (voices, heaps).
     *
     * What is NOT here, deliberately: VSync (a host display preference, not
     * an enhancement -- it stays on Video), Freecam (a debug tool -- it stays
     * on Mods), and every bug fix or seam guard in the port (those are always
     * on and have no switch at all).
     * ------------------------------------------------------------------ */
    add_tab("Enhancements", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Enhancements");

        /* THE PRESET SELECTOR, atop the group. Selecting a tier writes all
         * six values; changing any single row afterwards makes the six stop
         * matching a tier and the label derives to "Custom" on the very next
         * frame -- there is no stored "is custom" flag to get out of sync,
         * because the label is never stored at all (mp6_enh_preset_derive). */
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Preset",
                                      .getValue = [] { return Rml::String { mp6_enh_preset_name(enh_preset_current()) }; },
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
                pane.add_rml("<br/>Sets all six switches below at once. Change any one of them "
                             "afterwards and this reads <b>Custom</b> -- the name always describes "
                             "the switches, never the other way round.<br/><br/>");
                pane.add_rml(enh_preset_matrix_rml());
                for (const auto &tier : kEnhPresetTiers) {
                    pane.add_rml(fmt::format("<br/><b>{}</b> &mdash; {}", escape(tier.label),
                        escape(tier.blurb)));
                }
                pane.add_rml("<br/><br/>Anti-aliasing, shadow quality and the two limits apply on "
                             "the next launch or scene load; widescreen and unlocked FPS apply "
                             "immediately.");
            });

        /* Dynamic true-widescreen -- NOT the fixed 16:9 a binary ROM patch is
         * stuck with. Flipping it MID-GAME also flips the live engine flag
         * (window shape, render width, 2D layer track it within a tick) -- but
         * scene-load-time registrations (per-scene camera widening, backdrop
         * extension strips, the floor dup) only register when a scene loads
         * with Widescreen on, so the full effect lands on the next scene
         * change. Said in the help text rather than papered over. */
        config_bool_select(leftPane, rightPane,
            {
                .key = "Widescreen",
                .helpText = "Render at the window's own live aspect ratio instead of fixed 4:3 -- "
                            "widen the window to any shape (16:9, 21:9, ...) and the 3D camera/HUD "
                            "track it continuously, no stretching. This is dynamic, not a fixed "
                            "16:9 crop.<br/><br/>Flipping it during play "
                            "applies the window shape, render width and HUD immediately; the "
                            "scene's own cameras and widescreen backdrop extensions register at "
                            "scene load, so it fully applies on the next scene change.<br/><br/>"
                            "Off is the game's authored 4:3 composition, exactly as framed on the "
                            "GameCube.<br/><br/>"
                            "Disabled while the MP6_FREE_ASPECT or MP6_ENH_WIDESCREEN environment "
                            "lever is set (they keep absolute priority).",
                .getValue = [] { return cfg().widescreen != 0; },
                .setValue =
                    [inGame = mInGame](bool value) {
                        cfg().widescreen = value ? 1 : 0;
                        enh_apply_widescreen_live(value, inGame);
                    },
                .isDisabled = [] { return getenv("MP6_FREE_ASPECT") != NULL || getenv("MP6_ENH_WIDESCREEN") != NULL; },
                .isModified = [] { return cfg().widescreen != enh_shipped().widescreen; },
            });

        /* Unlocked FPS (shim/include/mp6_unlocked_fps.h has the full
         * contract): tick-decoupled presentation. Game logic stays the
         * design-rate 60Hz tick; the tick throttle's idle window presents
         * extra frames from the retained real-tick GX stream, with matrices
         * paired by stable model/camera identity and advanced along the last
         * two ticks' motion. Game callbacks are never re-run. Saved to the
         * config and applied live -- frame_interp.c re-reads the accessor
         * every tick. */
        config_bool_select(leftPane, rightPane,
            {
                .key = "Unlocked FPS",
                .helpText = "Present extra in-between frames at your display's refresh rate "
                            "while game logic keeps its original 60 Hz tick -- 3D motion "
                            "(characters, boards, camera) is interpolated between ticks. "
                            "UI, effects and screen transitions stay 60Hz.<br/><br/>Applies "
                            "immediately; needs the standard 60 Hz tick (it does nothing "
                            "under a free-run tick).<br/><br/>Disabled while the "
                            "MP6_UNLOCKED_FPS or MP6_ENH_UNLOCKED_FPS environment lever is set "
                            "(they win).",
                .getValue = [] { return cfg().unlockedFps != 0; },
                .setValue = [](bool value) { cfg().unlockedFps = value ? 1 : 0; },
                .isDisabled = [] { return getenv("MP6_UNLOCKED_FPS") != NULL || getenv("MP6_ENH_UNLOCKED_FPS") != NULL; },
                .isModified = [] { return cfg().unlockedFps != enh_shipped().unlockedFps; },
            });

        /* Shadow Quality (shim/include/mp6_shadow_quality.h has the full
         * contract): raises the real-time projected shadow map's
         * resolution (Hu3DShadow*, game/hsfman.c + game/hsfdraw.c) --
         * same lighting, same shadow shape, strictly more texels sampled
         * through the same live-projected texcoord. Read at shadow-CREATE
         * time, so flipping it mid-game doesn't retroactively resize a
         * map that's already up -- said in the description below rather
         * than papered over, same as Widescreen's own row.
         * Five options {1,2,4,8,16}. 8x/16x are REAL as of the offscreen
         * scissor fix: commits 3f179cd/2d599c4 had clamped the ceiling to
         * 8x then 4x because 8x's offscreen map dumped as a ~0.4% corner
         * sliver and 16x as uniform 0/0 -- root-caused (see
         * platform/hsf/mp6_shadow_quality.c) to the caster pass's
         * GXSetScissor overflowing the GameCube's 11-bit SU_SCIS register
         * once the offscreen pass scaled it past 4x, NOT an offscreen-
         * bracket size limit. Setting that scissor through aurora's
         * render-pixel path (mp6_shadow_offscreen_scissor / GXSetScissor-
         * Render) instead makes 8x/16x dump as full-scene ~14%-coverage
         * maps like 1x/2x/4x. Anything that still doesn't fit the model
         * heap steps down (mp6_shadow_quality_scale's heap-fit loop,
         * logged) -- which is why the presets stop at 4x. */
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
                pane.add_rml("<br/>Raises the texel resolution of the game's real-time projected "
                             "shadows (the blob under characters and hosts) -- same lighting, same "
                             "shadow size and shape, just sharper edges. 1x is the original game's "
                             "own resolution (byte-identical).<br/><br/>"
                             "4x and up render the shadow pass into a dedicated offscreen target "
                             "at the full scaled resolution, resolved with a mip chain so the "
                             "sharper map stays clean at a distance -- real added detail. 2x keeps "
                             "the lighter in-framebuffer path. 16x is the maximum. A setting "
                             "whose shadow map doesn't fit in the model heap steps down "
                             "(logged), which is why the presets stop at 4x.<br/><br/>"
                             "Takes effect the next time a board or scene loads -- not "
                             "retroactively on a shadow map that's already up.<br/><br/>"
                             "Disabled while the MP6_SHADOW_QUALITY or MP6_ENH_SHADOW_QUALITY "
                             "environment lever is set (they win).");
            });

        /* Anti-Aliasing (video.aa): one button over the unified Mp6AaMode
         * enum, rehomed from the Video tab byte-for-byte. isDisabled mirrors
         * every other lever-backed row: greyed out while any AA test lever is
         * set, because those already win over whatever is saved here. */
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
                            /* One AA mechanism at a time. Selecting any
                             * non-FXAA mode clears a previously-live FXAA
                             * immediately (that direction can never stack).
                             * Selecting FXAA applies live ONLY when this
                             * session did not initialize with MSAA/SSAA --
                             * those latch at aurora_initialize, so turning
                             * FXAA on over them would run both until restart.
                             * mp6_launcher_aa_apply_live() makes that call and
                             * records the result; when it defers, the row's
                             * own "takes effect next launch" promise (and the
                             * prelaunch restart prompt, via restart_pending())
                             * is what the user gets. */
                            enh_apply_aa_live(mode);
                            cfg_save();
                        });
                }
                Rml::String aaHelp = "<br/>Anti-aliasing smooths jagged edges. Only one mode runs at a "
                             "time. <b>MSAA 4x</b> multisamples polygon edges (takes effect next "
                             "launch). <b>FXAA</b> is a fast post-process with near-zero cost; it "
                             "applies immediately, except when this session started with "
#ifndef __ANDROID__
                             "MSAA/SSAA"
#else
                             "MSAA"
#endif
                             " -- then it too takes effect next launch, rather than stacking."
#ifndef __ANDROID__
                             " <b>SSAA 1.5x/2x</b> supersample the whole scene -- the strongest "
                             "quality, the highest cost (takes effect next launch)."
#endif
                             " <b>Off</b> is the original game's own rendering (byte-identical)."
                             "<br/><br/>The presets pick FXAA rather than MSAA: it is the only "
                             "rung that applies without a restart, and MSAA is a known "
                             "performance trap on some mobile GPUs."
                             "<br/><br/>Disabled while the MP6_MSAA / MP6_FXAA / MP6_SSAA / "
                             "MP6_ENH_AA environment levers are set (they override video.aa). If "
                             "several request enabled modes: MP6_MSAA &gt; MP6_SSAA &gt; MP6_FXAA.";
#ifdef __ANDROID__
                if (cfg().aa == MP6_AA_MSAA4X) {
                    /* Make the trap visible (savestate-x1): on-device data across
                     * two Android GPUs shows this is GPU-dependent, not backend-
                     * dependent -- an earlier wording blamed OpenGL specifically,
                     * but the collapse (~110fps -> ~30fps on one GPU, not the
                     * other, same scene) shows up under EVERY graphics backend
                     * tried on the affected GPU, so the backend was never the
                     * variable. Consistent with this port's own pass accounting
                     * (file select: 6 render passes / 5 framebuffer copies vs 3/2
                     * on a plain screen -- every copy breaks the pass, and under
                     * MSAA each break carries a full-attachment, sample-count-
                     * sized resolve): a tile-based GPU with less tile-memory
                     * headroom pays that traffic every frame; one with more
                     * absorbs it. No single setting or backend switch fixes it
                     * for every device, so this only flags the possibility
                     * rather than naming a culprit that isn't consistently one. */
                    aaHelp += "<br/><br/>MSAA can be very expensive on some mobile GPUs -- if the "
                              "framerate drops sharply on certain screens, try FXAA or turn "
                              "anti-aliasing off.";
                }
#endif
                pane.add_rml(aaHelp);
            });

        /* Extended SFX voices (enhancements.sfx_voices): the mixer's voice
         * table, 16 (retail) or 32. UI + CONFIG ONLY in this lane -- the
         * mixer-side consumer, the slot-index sweep it needs and the
         * savestate version bump for a table-size change land in the audio
         * lane. mp6_enh_sfx_voices() is the seam it reads. */
        config_bool_select(leftPane, rightPane,
            {
                .key = "Extended SFX Voices",
                .helpText = "Raise the sound-effect mixer from the GameCube's 16 simultaneous "
                            "voices to 32.<br/><br/>The hardware limit is why busy moments -- a "
                            "coin shower, several players landing at once -- drop sounds on the "
                            "original. Nothing about the sounds themselves changes; there is just "
                            "room for more of them at the same time.<br/><br/>Off is the retail "
                            "16-voice table.<br/><br/>Takes effect next launch. Disabled while the "
                            "MP6_ENH_SFX_VOICES environment lever is set (it wins).",
                .getValue = [] { return cfg().sfxVoices >= 32; },
                .setValue = [](bool value) { cfg().sfxVoices = value ? 32 : 16; },
                .isDisabled = [] { return getenv("MP6_ENH_SFX_VOICES") != NULL; },
                .isModified = [] { return cfg().sfxVoices != enh_shipped().sfxVoices; },
            });

        /* Expanded heaps (enhancements.heap_scale): HuMem's capacities at
         * retail size or x4 over HeapSizeTbl. UI + CONFIG ONLY in this lane --
         * the HuMem-side consumer and its configured-size audit land in the
         * memory lane. mp6_enh_heap_scale() is the seam it reads. */
        config_bool_select(leftPane, rightPane,
            {
                .key = "Expanded Heaps",
                .helpText = "Give the game's memory heaps four times the GameCube's capacity."
                            "<br/><br/>The retail sizes were cut to fit 24 MB of console RAM. A "
                            "host has no such limit, and the extra headroom is what lets the "
                            "higher shadow settings hold their full resolution instead of "
                            "stepping down, and keeps heavy scenes from evicting data they are "
                            "about to need.<br/><br/>Off is the retail heap table."
                            "<br/><br/>Takes effect next launch. Disabled while the "
                            "MP6_ENH_HEAP_SCALE environment lever is set (it wins).",
                .getValue = [] { return cfg().heapScale >= 4; },
                .setValue = [](bool value) { cfg().heapScale = value ? 4 : 1; },
                .isDisabled = [] { return getenv("MP6_ENH_HEAP_SCALE") != NULL; },
                .isModified = [] { return cfg().heapScale != enh_shipped().heapScale; },
            });
    });

    /* ------------------------------------------------------------------
     * MODS ([MP6] content: freecam. What is left here after the rehome is
     * what actually belongs here -- a debug/creative tool that is never a
     * shipped default and is deliberately never saved to the config.)
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

        /* Widescreen / Unlocked FPS / Shadow Quality MOVED OUT of this tab.
         * They were never mods: they are three of the six port ENHANCEMENTS
         * and now live in that named group under its preset selector. Their
         * config keys, live-vs-next-scene contracts and env levers are
         * unchanged. Freecam stays -- it really is a debug/creative tool,
         * not a shipped-on default, and it is deliberately never saved. */
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

            leftPane.add_section("Security");
            leftPane.register_control(leftPane.add_button("Trusted files only"),
                rightPane, [](Pane &pane) {
                    pane.clear();
                    pane.add_rml("Savestates contain live stacks, pointers, and writable program "
                                 "data. Treat them like executable files: load only states you "
                                 "created or received from a fully trusted source. Compatibility "
                                 "checks prevent accidents, not malicious files.");
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
                    rightPane, [slot, path = Rml::String(path)](Pane &pane) {
                        pane.clear();
                        pane.add_rml(fmt::format("Capture the whole session into slot {} at the next "
                                                 "frame boundary (takes a moment).<br/><br/>{}",
                            slot, escape(path)));
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
