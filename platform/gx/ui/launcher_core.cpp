/* MP6 native port -- launcher core.
 *
 * OUR file (not ripped): everything the launcher does BESIDES drawing:
 *
 *   1. the flat mp6_config.json model (keys, defaults, tolerant parser,
 *      save-on-change discipline),
 *   2. mp6_launcher_decide_mode()'s automation-skip truth table (the
 *      contract, canonical in docs/TESTING.md: tick-budget argv /
 *      --input-script / MP6_AUTO_START_TICKS / MP6_LAUNCHER=0 boot
 *      byte-identically with ZERO [LAUNCHER] lines),
 *   3. live settings application (window mode/size/aspect, volume) and the
 *      boot-time game settings (tick rate, content root override),
 *   4. the runtime HSF wordmark/watermark decode from the user's own disc
 *      files (NO Nintendo art in the repo), served to the UI through
 *      aurora's RmlUi runtime-texture provider,
 *   5. the pre-boot menu loop shell + the in-game overlay hook.
 *
 * The MENU ITSELF is not drawn here at all: it is partyboard's own RmlUi
 * implementation, adapted into this directory (docs/PARTYBOARD_PROVENANCE.md)
 * with our settings content. This TU pushes those documents (Overlay,
 * Prelaunch) and pumps their frame loop; platform/main_native.c consumes
 * the six-function C seam.
 */

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/rmlui.hpp> /* register_texture_provider (AURORA_ENABLE_RMLUI) */
#include <SDL3/SDL.h>
#include <zlib.h> /* title.bin entry inflate (same zlib-ng build game/decode.c links) */

#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

#include "launcher_state.hpp"
#include "launcher_hsf_safe.h"
#include "overlay.hpp"
#include "prelaunch.hpp"
#include "settings.hpp" /* the persistent in-game menu instance (section 9) */
#include "ui.hpp"

extern "C" {
#include "host.h" /* mp6_host_monotonic_ns / mp6_host_sleep_ns / mp6_host_save_dir */
#include "mp6_json.h" /* strict, complete JSON lexical validation */
#include "mp6_path.h" /* checked operational path copy/join */
#include "mp6_utf8_file.h" /* Windows UTF-8 config/content files */
#include "mp6_savestate.h" /* in-game menu: savestate result toasts (section 9) */

/* SAVESTATE CARVE-OUT. Placing this AFTER this TU's own
 * includes is load-bearing, not stylistic: it is a #pragma clang section that
 * redirects every file-scope definition FOLLOWING it. As a -include (before all
 * headers) it also captured the decomp headers' C TENTATIVE definitions --
 * dolphin/os.h:27 `u32 __OSBusClock;` and friends -- turning those common
 * symbols into strong per-TU definitions and breaking the link with duplicate
 * symbol errors. Here, headers keep their normal linkage and only this file's
 * own statics move. tools/build.py asserts this line exists in every TU listed
 * in HOST_STATE_SECTION_SOURCES. */
#include "mp6_host_section.h"


/* Additive hooks in existing platform files (see their own comments). Kept
 * as narrow declarations here so this Aurora-header TU never imports the
 * decomp's competing dolphin.h universe. */
int mp6_dvd_set_root_override(const char *filesRoot, const char *fstPath);
int mp6_dvd_probe_root(char *filesRootOut, size_t n);
int mp6_dvd_validate_disc_root(const char *discRoot, char *err, size_t errn);
int mp6_import_recover_disc_root(const char *destDiscRoot);
void mp6_audio_set_master_gain(float gain);                                 /* platform/audio/audio_out_sdl.c */
#ifdef __ANDROID__
const char *SDL_GetAndroidExternalStoragePath(void);
const char *SDL_GetAndroidInternalStoragePath(void);
#endif
}

#ifndef MP6_PORT_VERSION
#define MP6_PORT_VERSION "dev"
#endif

/* =======================================================================
 * 1. Config model.
 * ======================================================================= */

static Mp6LauncherConfig g_cfg;
static int g_launcherMode; /* config loaded + settings apply (interactive-style launch) */
static char g_configPath[1024];
static char g_saveDirAbs[1024];

static void mp6_launcher_defaults(void)
{
    g_cfg.skipLauncher = 0;
    g_cfg.windowMode = MP6_WINMODE_WINDOWED;
    g_cfg.windowScale = 0.0f;
    g_cfg.aspectLocked = 1;
    g_cfg.vsync = 1;
    snprintf(g_cfg.backend, sizeof(g_cfg.backend), "auto");
    g_cfg.aa = MP6_AA_OFF; /* Anti-Aliasing: default off -- byte-identical, the sacred contract */
    g_cfg.showFps = 0;
    g_cfg.fpsCorner = 0;
    g_cfg.tickHz = 60.0;
    g_cfg.contentRoot[0] = '\0';
    g_cfg.masterVolume = 100;
    g_cfg.widescreen = 0; /* default OFF -- existing 4:3 pillarboxed behavior, byte-unchanged */
    g_cfg.shadowQuality = 1; /* Shadow Quality: default native -- byte-identical, the sacred contract */
    g_cfg.unlockedFps = 0; /* Unlocked FPS: default OFF -- presentation stays 1:1 with the 60Hz tick */
}

/* =======================================================================
 * 2. Flat JSON read/write. Unknown keys are ignored, but syntax is strict
 * and the parse is transactional: malformed input applies no prefix of
 * settings and leaves the caller's defaults intact.
 * ======================================================================= */

enum Mp6ConfigKeyBit : uint32_t {
    MP6_CFG_SKIP = 1u << 0,
    MP6_CFG_WINDOW_MODE = 1u << 1,
    MP6_CFG_WINDOW_SCALE = 1u << 2,
    MP6_CFG_VSYNC = 1u << 3,
    MP6_CFG_BACKEND = 1u << 4,
    MP6_CFG_SHOW_FPS = 1u << 5,
    MP6_CFG_FPS_CORNER = 1u << 6,
    MP6_CFG_TICK_HZ = 1u << 7,
    MP6_CFG_CONTENT_ROOT = 1u << 8,
    MP6_CFG_MASTER_VOLUME = 1u << 9,
    MP6_CFG_WIDESCREEN = 1u << 10,
    MP6_CFG_SHADOW_QUALITY = 1u << 11,
    MP6_CFG_UNLOCKED_FPS = 1u << 12,
    MP6_CFG_AA = 1u << 13,
    MP6_CFG_OLD_MSAA = 1u << 14,
};

static uint32_t js_known_key_bit(const char *key)
{
    if (strcmp(key, "launcher.skip") == 0) return MP6_CFG_SKIP;
    if (strcmp(key, "video.window_mode") == 0) return MP6_CFG_WINDOW_MODE;
    if (strcmp(key, "video.window_scale") == 0) return MP6_CFG_WINDOW_SCALE;
    if (strcmp(key, "video.vsync") == 0) return MP6_CFG_VSYNC;
    if (strcmp(key, "video.backend") == 0) return MP6_CFG_BACKEND;
    if (strcmp(key, "video.show_fps") == 0) return MP6_CFG_SHOW_FPS;
    if (strcmp(key, "video.fps_corner") == 0) return MP6_CFG_FPS_CORNER;
    if (strcmp(key, "game.tick_hz") == 0) return MP6_CFG_TICK_HZ;
    if (strcmp(key, "game.content_root") == 0) return MP6_CFG_CONTENT_ROOT;
    if (strcmp(key, "audio.master_volume") == 0) return MP6_CFG_MASTER_VOLUME;
    if (strcmp(key, "video.widescreen") == 0) return MP6_CFG_WIDESCREEN;
    if (strcmp(key, "video.shadow_quality") == 0) return MP6_CFG_SHADOW_QUALITY;
    if (strcmp(key, "video.unlocked_fps") == 0) return MP6_CFG_UNLOCKED_FPS;
    if (strcmp(key, "video.aa") == 0) return MP6_CFG_AA;
    if (strcmp(key, "video.msaa") == 0) return MP6_CFG_OLD_MSAA;
    return 0;
}

static bool js_known_string_key(uint32_t bit)
{
    return bit == MP6_CFG_WINDOW_MODE || bit == MP6_CFG_BACKEND ||
           bit == MP6_CFG_CONTENT_ROOT;
}

static bool js_backend_valid(const char *value)
{
    return strcmp(value, "auto") == 0 || strcmp(value, "d3d12") == 0 ||
           strcmp(value, "d3d11") == 0 || strcmp(value, "vulkan") == 0 ||
           strcmp(value, "metal") == 0 || strcmp(value, "opengl") == 0 ||
           strcmp(value, "opengles") == 0 || strcmp(value, "webgpu") == 0;
}

static int mp6_launcher_parse_config(const char *text)
{
    enum ValueKind { VALUE_OTHER, VALUE_STRING, VALUE_BOOL, VALUE_NUMBER };
    Mp6LauncherConfig parsed = g_cfg;
    char key[64];
    char sval[1024];
    const char *p = mp6_json_skip_ws(text);
    uint32_t seen = 0;
    bool first = true;

    if (p == nullptr || *p != '{') goto malformed;
    p = mp6_json_skip_ws(p + 1);

    while (*p != '}') {
        uint32_t bit;
        uint32_t seenBefore;
        ValueKind kind = VALUE_OTHER;
        bool boolValue = false;
        double numberValue = 0.0;
        const char *valueEnd = nullptr;

        if (*p == '\0') goto malformed;
        if (!first) {
            if (*p != ',') goto malformed;
            p = mp6_json_skip_ws(p + 1);
            if (*p == '}') goto malformed; /* no trailing comma */
        }
        first = false;
        p = mp6_json_parse_string(p, key, sizeof(key));
        if (p == nullptr) goto malformed;
        bit = js_known_key_bit(key);
        seenBefore = seen;
        if (bit != 0 && (seen & bit) != 0) goto malformed; /* ambiguous duplicate */
        seen |= bit;
        p = mp6_json_skip_ws(p);
        if (*p != ':') goto malformed;
        p = mp6_json_skip_ws(p + 1);

        if (*p == '"') { /* string value */
            valueEnd = mp6_json_parse_string(p,
                                              js_known_string_key(bit) ? sval : nullptr,
                                              js_known_string_key(bit) ? sizeof(sval) : 0);
            if (valueEnd == nullptr) goto malformed;
            kind = VALUE_STRING;
        } else if (strncmp(p, "true", 4) == 0) {
            valueEnd = p + 4;
            boolValue = true;
            kind = VALUE_BOOL;
        } else if (strncmp(p, "false", 5) == 0) {
            valueEnd = p + 5;
            boolValue = false;
            kind = VALUE_BOOL;
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            valueEnd = mp6_json_parse_number(p, &numberValue);
            if (valueEnd == nullptr) goto malformed;
            kind = VALUE_NUMBER;
        } else {
            valueEnd = mp6_json_skip_value(p); /* strict object/array/null for unknown/future keys */
            if (valueEnd == nullptr) goto malformed;
        }

        if (kind == VALUE_STRING) {
            if (bit == MP6_CFG_WINDOW_MODE) {
                parsed.windowMode = strcmp(sval, "fullscreen") == 0
                                        ? MP6_WINMODE_FULLSCREEN : MP6_WINMODE_WINDOWED;
            } else if (bit == MP6_CFG_BACKEND) {
                snprintf(parsed.backend, sizeof(parsed.backend), "%s",
                         js_backend_valid(sval) ? sval : "auto");
            } else if (bit == MP6_CFG_CONTENT_ROOT) {
                if (mp6_path_copy_checked(parsed.contentRoot,
                                          sizeof(parsed.contentRoot), sval) != 0) {
                    printf("[LAUNCHER] config.json: game.content_root is too long -- keeping auto resolution\n");
                    parsed.contentRoot[0] = '\0';
                }
            }
        } else if (kind == VALUE_BOOL) {
            /* "video.aspect_locked" is deliberately NOT read anymore (the
             * "Lock 4:3" row was removed -- Widescreen is the one aspect
             * switch); an old config's key falls through to the tolerant
             * unknown-key ignore below, and the built-in default (locked
             * whenever Widescreen is off) applies. */
            if      (bit == MP6_CFG_SKIP)          parsed.skipLauncher = boolValue;
            else if (bit == MP6_CFG_VSYNC)         parsed.vsync = boolValue;
            else if (bit == MP6_CFG_SHOW_FPS)      parsed.showFps = boolValue;
            else if (bit == MP6_CFG_WIDESCREEN)    parsed.widescreen = boolValue;
            else if (bit == MP6_CFG_UNLOCKED_FPS)  parsed.unlockedFps = boolValue;
        } else if (kind == VALUE_NUMBER) {
            const double v = numberValue;
            if      (bit == MP6_CFG_WINDOW_SCALE) {
                parsed.windowScale = (v >= 0.0 && v <= 16.0) ? (float)v : 0.0f;
            }
            else if (bit == MP6_CFG_FPS_CORNER) {
                parsed.fpsCorner = (v >= 0.0 && v <= 3.0 && v == std::floor(v)) ? (int)v : 0;
            }
            else if (bit == MP6_CFG_TICK_HZ) {
                parsed.tickHz = (v == 0.0 || (v >= 1.0 && v <= 1000.0)) ? v : 60.0;
            }
            else if (bit == MP6_CFG_MASTER_VOLUME) {
                parsed.masterVolume = (v >= 0.0 && v <= 100.0 && v == std::floor(v))
                                          ? (int)v : 100;
            }
            else if (bit == MP6_CFG_SHADOW_QUALITY) {
                /* Tolerant by construction (this file's own header
                 * comment): anything outside the five valid scales --
                 * missing key, a future downgrade, or hand-edited JSON --
                 * falls back to 1 (native), never a crash or an
                 * out-of-range shadowP->size downstream. */
                parsed.shadowQuality = (v == 1.0 || v == 2.0 || v == 4.0 ||
                                        v == 8.0 || v == 16.0) ? (int)v : 1;
            }
            /* "video.fi_mode" is deliberately NOT read anymore (the "FPS
             * Smoothing Mode" row was removed -- model-level interpolation is
             * the one and only mechanism Unlocked FPS uses). An old config's
             * key falls through to the tolerant unknown-key ignore at the end
             * of this chain, exactly like the retired "video.aspect_locked"
             * above: the value is consumed by strtod and discarded, no error,
             * no parse stop. */
            else if (bit == MP6_CFG_AA) {
                /* Anti-Aliasing (Mp6AaMode): tolerant by construction, same
                 * shape as shadow_quality just above -- anything outside the
                 * known modes (missing key, a future value, hand-edited JSON)
                 * falls back to OFF, never a bogus mechanism downstream. */
                if (v == (double)MP6_AA_OFF || v == (double)MP6_AA_MSAA4X ||
                    v == (double)MP6_AA_FXAA || v == (double)MP6_AA_SSAA15 ||
                    v == (double)MP6_AA_SSAA2X) {
                    parsed.aa = (int)v;
                } else {
                    parsed.aa = MP6_AA_OFF;
                }
            }
            else if (bit == MP6_CFG_OLD_MSAA) {
                /* Backward-compat migration: P1 shipped a standalone
                 * video.msaa key; an existing pre-unification config carries
                 * msaa=4 and no video.aa. Map that onto the unified enum.
                 * Upgrade-only (never clobbers a video.aa the same file might
                 * also carry); new configs write video.aa exclusively. */
                if ((seenBefore & MP6_CFG_AA) == 0 && v == 4.0) {
                    parsed.aa = MP6_AA_MSAA4X;
                }
            }
        }
        p = mp6_json_skip_ws(valueEnd);
        if (*p != ',' && *p != '}') goto malformed;
    }
    p = mp6_json_skip_ws(p + 1);
    if (*p != '\0') goto malformed;
    g_cfg = parsed;
    return 1;

malformed:
    printf("[LAUNCHER] config.json is malformed, ambiguous, or out of range -- using defaults\n");
    return 0;
}

static void mp6_launcher_config_save(void)
{
    char rootEsc[sizeof(g_cfg.contentRoot) * 6u + 1u];
    char temporary[sizeof(g_configPath) + 32u];
    FILE *f = nullptr;
    unsigned int attempt;
    int failed;
    if (g_configPath[0] == '\0') return;
    if (!mp6_json_escape_string(g_cfg.contentRoot, rootEsc, sizeof(rootEsc))) {
        printf("[LAUNCHER] content path is too large to encode in config.json\n");
        return;
    }
    for (attempt = 0; attempt < 32u; ++attempt) {
        int written = snprintf(temporary, sizeof(temporary), "%s.tmp.%02u",
                               g_configPath, attempt);
        if (written < 0 || (size_t)written >= sizeof(temporary)) break;
        f = mp6_fopen_utf8(temporary, "wbx");
        if (f != nullptr) break;
    }
    if (f == NULL) {
        printf("[LAUNCHER] could not create a temporary config for %s\n", g_configPath);
        return;
    }
    /* "video.aspect_locked" is no longer written (row removed; the key in
     * an existing file is ignored on read, so old configs stay valid). */
    failed = fprintf(f,
            "{\n"
            "    \"launcher.skip\": %s,\n"
            "    \"video.window_mode\": \"%s\",\n"
            "    \"video.window_scale\": %g,\n"
            "    \"video.vsync\": %s,\n"
            "    \"video.backend\": \"%s\",\n"
            "    \"video.show_fps\": %s,\n"
            "    \"video.fps_corner\": %d,\n"
            "    \"game.tick_hz\": %g,\n"
            "    \"game.content_root\": \"%s\",\n"
            "    \"audio.master_volume\": %d,\n"
            "    \"video.widescreen\": %s,\n"
            "    \"video.shadow_quality\": %d,\n"
            "    \"video.unlocked_fps\": %s,\n"
            "    \"video.aa\": %d\n"
            "}\n",
            g_cfg.skipLauncher ? "true" : "false",
            g_cfg.windowMode == MP6_WINMODE_FULLSCREEN ? "fullscreen" : "windowed",
            (double)g_cfg.windowScale,
            g_cfg.vsync ? "true" : "false",
            g_cfg.backend,
            g_cfg.showFps ? "true" : "false",
            g_cfg.fpsCorner,
            g_cfg.tickHz,
            rootEsc,
            g_cfg.masterVolume,
            g_cfg.widescreen ? "true" : "false", /* additive key */
            g_cfg.shadowQuality, /* Shadow Quality: additive key */
            g_cfg.unlockedFps ? "true" : "false", /* Unlocked FPS: additive key */
            g_cfg.aa) < 0; /* Anti-Aliasing: unified video.aa enum, appended last so any external tooling scraping the first N keys positionally (none known) is unaffected */
    if (fflush(f) != 0 || ferror(f)) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed || mp6_replace_utf8(temporary, g_configPath) != 0) {
        (void)mp6_remove_utf8(temporary);
        printf("[LAUNCHER] config save failed; previous config is unchanged\n");
    }
}

/* =======================================================================
 * 3. Config location: next to the exe.
 * ======================================================================= */

static void mp6_launcher_resolve_paths(void)
{
#ifdef __ANDROID__
    /* SDL_GetBasePath() is "./" on Android
     * (the app has no exe directory and cwd is "/", unwritable) -- the
     * config lives under the same base dir mp6_android_main publishes for
     * disc/save data (external files dir preferred), which is exported as
     * MP6_HOST_BASE BEFORE aurora_main runs. */
    const char *hostBase = getenv("MP6_HOST_BASE");
    if (hostBase != NULL && hostBase[0] != '\0') {
        if (mp6_path_join_checked(g_configPath, sizeof(g_configPath), hostBase,
                                  "mp6_config.json") != 0) {
            printf("[LAUNCHER] MP6_HOST_BASE is too long for config path -- config disabled\n");
        }
    } else {
        g_configPath[0] = '\0'; /* no writable base: defaults only, no save */
    }
#else
    const char *base = SDL_GetBasePath();
    if (base != NULL && base[0] != '\0') {
        if (mp6_path_join_checked(g_configPath, sizeof(g_configPath), base,
                                  "mp6_config.json") != 0) {
            printf("[LAUNCHER] executable path is too long for config path -- config disabled\n");
        }
    } else {
        mp6_path_copy_checked(g_configPath, sizeof(g_configPath), "mp6_config.json"); /* cwd fallback */
    }
#endif

    {
#if defined(__ANDROID__)
        /* Android's host seam already returns the complete absolute
         * <MP6_HOST_BASE>/saves target. Never prefix the base a second time. */
        if (mp6_host_save_dir(g_saveDirAbs, sizeof(g_saveDirAbs)) != 0) {
            g_saveDirAbs[0] = '\0';
        }
#else
        char rel[64];
        if (mp6_host_save_dir(rel, sizeof(rel)) != 0) {
            mp6_path_copy_checked(rel, sizeof(rel), "saves");
        }
#if defined(_WIN32)
        if (mp6_fullpath_utf8(g_saveDirAbs, sizeof(g_saveDirAbs), rel) != 0) {
            mp6_path_copy_checked(g_saveDirAbs, sizeof(g_saveDirAbs), rel);
        }
#else
        mp6_path_copy_checked(g_saveDirAbs, sizeof(g_saveDirAbs), rel);
#endif
#endif
    }
}

/* =======================================================================
 * 4. C API: mode decision + config accessors (the automation-compatibility
 * contract lives here).
 * ======================================================================= */

static void mp6_launcher_capture_initials(void);

extern "C" int mp6_launcher_decide_mode(int hasNumericArg, int hasInputScript, int *outShowMenu)
{
    /* Skip-logic truth table (docs/TESTING.md quotes this):
     *
     *   MP6_LAUNCHER=0                        -> automation mode (no config, no menu)
     *   MP6_LAUNCHER=1                        -> launcher mode + menu, always
     *   (unset) numeric tick-budget argv      -> automation mode
     *   (unset) --input-script present        -> automation mode
     *   (unset) MP6_AUTO_START_TICKS nonempty -> automation mode
     *   (unset) plain interactive launch      -> launcher mode; menu unless launcher.skip
     *
     * Automation mode boots byte-for-byte as if the launcher did not exist:
     * config.json is not even read, so no user setting can perturb a
     * harness run, and it prints ZERO [LAUNCHER] lines. */
    const char *ovr = getenv("MP6_LAUNCHER");
    const char *ast = getenv("MP6_AUTO_START_TICKS");
    int automation = (hasNumericArg || hasInputScript || (ast != NULL && ast[0] != '\0'));
    int forceMenu = 0;

    if (ovr != NULL && ovr[0] == '0') {
        g_launcherMode = 0;
    } else if (ovr != NULL && ovr[0] == '1') {
        g_launcherMode = 1;
        forceMenu = 1;
    } else {
        g_launcherMode = !automation;
    }

    if (g_launcherMode) {
        FILE *f;
        mp6_launcher_defaults();
        mp6_launcher_resolve_paths();
        f = mp6_fopen_utf8(g_configPath, "rb");
        if (f != NULL) {
            static char text[8192];
            size_t got = fread(text, 1, sizeof(text) - 1, f);
            int extra = fgetc(f);
            int readFailed = ferror(f);
            text[got] = '\0';
            fclose(f);
            if (readFailed || extra != EOF) {
                printf("[LAUNCHER] config rejected: file exceeds %zu bytes or could not be read completely\n",
                       sizeof(text) - 1);
            } else {
                if (mp6_launcher_parse_config(text)) {
                    printf("[LAUNCHER] config loaded from %s\n", g_configPath);
                } else {
                    printf("[LAUNCHER] config rejected; defaults retained for this launch\n");
                }
            }
        } else {
            printf("[LAUNCHER] no config at %s -- defaults (file is created on first settings change)\n",
                   g_configPath);
        }
        mp6_launcher_capture_initials();
        *outShowMenu = forceMenu ? 1 : !g_cfg.skipLauncher;
    } else {
        *outShowMenu = 0;
    }
    fflush(stdout);
    return g_launcherMode;
}

/* 1 in automation/straight-boot mode (numeric tick-budget argv / --input-script
 * / MP6_AUTO_START_TICKS / MP6_LAUNCHER=0), 0 in interactive launcher mode.
 * Read by the tick throttle (aurora_bridge.c) to default automation runs to
 * free-run pacing -- a drive only needs to REACH a state, not run in real time.
 * Valid only after mp6_launcher_decide_mode() has run (main_native.c, before
 * the game loop -- so it is set well before the throttle's first-tick init). */
extern "C" int mp6_launcher_is_automation(void)
{
    return g_launcherMode ? 0 : 1;
}

/* Launch-time values of the two next-launch settings, for the restart-
 * pending check (the ripped prelaunch shows its "Apply Options" modal when
 * these differ from the current config). */
static char g_initialBackend[24];
static int g_initialVsync = 1;
static int g_initialMsaa = 1;

/* Anti-Aliasing: the aurora_initialize-time (restart-pending) projection of
 * the unified video.aa enum. Only MSAA is an init-time value in P2 -- FXAA is
 * a live post-process (never restart-pending), so Off and FXAA both project
 * to a sample count of 1. This is the single mapping P1's cfg_msaa accessor,
 * the initials snapshot and restart_pending() all share, keeping the MSAA
 * behavior byte-identical to P1 while the field that feeds it is unified. */
static int mp6_aa_to_msaa(int aa)
{
    return aa == MP6_AA_MSAA4X ? 4 : 1;
}

/* Anti-Aliasing P3 (SSAA): the aurora_initialize-time supersampling factor for
 * the unified video.aa enum. 1.0 = off/native (Off/MSAA/FXAA all project here);
 * SSAA is a restart-pending init value like MSAA, so it is snapshotted at
 * launch and compared in restart_pending(). */
static float mp6_aa_to_ssaa(int aa)
{
    if (aa == MP6_AA_SSAA15) return 1.5f;
    if (aa == MP6_AA_SSAA2X) return 2.0f;
    return 1.0f;
}

static float g_initialSsaa = 1.0f;

/* Anti-Aliasing: what aurora was ACTUALLY initialized with this session, as
 * reported by main_native.c once it has resolved config + env levers. Not the
 * same thing as the saved config: an MP6_MSAA/MP6_SSAA/MP6_FXAA env lever wins
 * over the config in both modes, so only main_native.c knows the truth.
 *
 * This is what makes the AA modes mutually exclusive AT RUNTIME. MSAA and SSAA
 * are aurora_initialize-time (restart-pending); FXAA is a live post-process. So
 * launching under MSAA/SSAA and then selecting FXAA used to STACK the two until
 * the next restart. mp6_launcher_aa_apply_live() below refuses that: FXAA only
 * goes live when this session has no init-time AA, otherwise the selection is
 * saved and deferred -- which is exactly what the row's "takes effect next
 * launch" text already claims. */
static int   g_sessionMsaa = 1;
static float g_sessionSsaa = 1.0f;
static int   g_sessionPostAa = 0; /* 1 = FXAA is live right now */

extern "C" void mp6_launcher_note_session_aa(int msaa, float ssaa, int postAa)
{
    g_sessionMsaa = msaa;
    g_sessionSsaa = ssaa;
    g_sessionPostAa = postAa ? 1 : 0;
}

/* 1 when this session came up with an init-time AA mechanism (MSAA or SSAA)
 * already active -- i.e. FXAA cannot also be turned on without stacking. */
extern "C" int mp6_launcher_session_init_aa_active(void)
{
    return (g_sessionMsaa > 1 || g_sessionSsaa > 1.0f) ? 1 : 0;
}

/* Decides whether selecting `aa` can take effect live, and records what the
 * session's post-process AA will be as a result. Returns 1 when the caller
 * should push the change to aurora now, 0 when it is deferred to next launch.
 *
 * Turning FXAA OFF is always live (clearing a post-process can never stack);
 * turning it ON is live only when no init-time AA is running this session. */
extern "C" int mp6_launcher_aa_apply_live(int aa)
{
    if (aa != MP6_AA_FXAA) {
        g_sessionPostAa = 0;
        return 1;
    }
    if (mp6_launcher_session_init_aa_active()) {
        return 0; /* MSAA/SSAA owns this session -- restart-pending, not stacked */
    }
    g_sessionPostAa = 1;
    return 1;
}

static void mp6_launcher_capture_initials(void)
{
    snprintf(g_initialBackend, sizeof(g_initialBackend), "%s", g_cfg.backend);
    g_initialVsync = g_cfg.vsync;
    g_initialMsaa = mp6_aa_to_msaa(g_cfg.aa);
    g_initialSsaa = mp6_aa_to_ssaa(g_cfg.aa);
}

static AuroraBackend mp6_backend_from_id(const char *id)
{
    if (strcmp(id, "d3d12") == 0)    return BACKEND_D3D12;
    if (strcmp(id, "d3d11") == 0)    return BACKEND_D3D11;
    if (strcmp(id, "vulkan") == 0)   return BACKEND_VULKAN;
    if (strcmp(id, "metal") == 0)    return BACKEND_METAL;
    if (strcmp(id, "opengl") == 0)   return BACKEND_OPENGL;
    if (strcmp(id, "opengles") == 0) return BACKEND_OPENGLES;
    if (strcmp(id, "webgpu") == 0)   return BACKEND_WEBGPU;
    return BACKEND_AUTO;
}

extern "C" int mp6_launcher_cfg_backend(void)
{
    return g_launcherMode ? (int)mp6_backend_from_id(g_cfg.backend) : (int)BACKEND_AUTO;
}

extern "C" int mp6_launcher_cfg_vsync(void)
{
    return g_launcherMode ? g_cfg.vsync : 1;
}

/* A5: resolved "lock 4:3 aspect" preference for
 * mp6_bridge_apply_content_aspect_policy() (aurora_bridge.c), called once
 * right before GameMain() -- same launcher-mode-vs-automation-default
 * shape as the two accessors above. Automation's fixed 1 matches this
 * config key's own default (mp6_launcher_defaults() below) and pre-A5's
 * behavior when the config file is never read. */
extern "C" int mp6_launcher_cfg_aspect_locked(void)
{
    return g_launcherMode ? g_cfg.aspectLocked : 1;
}

/* Resolved "dynamic true-widescreen"
 * preference for mp6_widescreen_set_enabled() (aurora_bridge.c), called
 * once right before GameMain() alongside mp6_bridge_apply_content_aspect_
 * policy() above -- same launcher-mode-vs-automation-default shape as
 * every other accessor in this section. Automation's fixed 0 matches this
 * config key's own default (mp6_launcher_defaults() above) and is what
 * every existing automated gate already assumes (a widescreen-ON gate is
 * a new, additional scenario -- not a
 * change to what automation mode has always done). */
extern "C" int mp6_launcher_cfg_widescreen(void)
{
    return g_launcherMode ? g_cfg.widescreen : 0;
}

/* Shadow Quality (shim/include/mp6_shadow_quality.h): the configured
 * shadow-map linear scale in launcher mode, or a fixed 1 (native) in
 * automation/pre-launcher-init -- same "automation never sees a non-
 * default value" contract as mp6_launcher_cfg_widescreen() above.
 * mp6_shadow_quality_scale() (platform/hsf/mp6_shadow_quality.c) is the
 * actual origin-site helper hsfman.c's patched Hu3DShadow* functions call;
 * it reads this to get the user's own preference, then clamps for
 * HEAP_MODEL headroom before returning the EFFECTIVE scale. */
extern "C" int mp6_launcher_cfg_shadow_quality(void)
{
    return g_launcherMode ? g_cfg.shadowQuality : 1;
}

/* Anti-Aliasing P1 (aurora/include/aurora/aurora.h AuroraConfig.msaa): the
 * configured MSAA sample count in launcher mode, or a fixed 1 (off) in
 * automation/pre-launcher-init -- same "automation never sees a non-
 * default value" contract as every other accessor in this section. Read
 * once at aurora_initialize time (main_native.c), like backend/vsync --
 * it is a restart-pending setting for the same reason (the multisampled
 * targets are created at that call, not re-created live). */
extern "C" int mp6_launcher_cfg_msaa(void)
{
    return g_launcherMode ? mp6_aa_to_msaa(g_cfg.aa) : 1;
}

/* Anti-Aliasing P2 (FXAA): the live post-process AA mode for the current
 * config, as an AuroraPostAA value (aurora/include/aurora/aurora.h) --
 * AURORA_POST_AA_FXAA (1) when video.aa selects FXAA, else AURORA_POST_AA_NONE
 * (0). Applied via aurora_set_post_aa the moment it is selected AND once at
 * boot (main_native.c), so unlike msaa it is NOT restart-pending. Same
 * "automation never sees a non-default value" contract as cfg_msaa above. */
extern "C" int mp6_launcher_cfg_post_aa(void)
{
    return (g_launcherMode && g_cfg.aa == MP6_AA_FXAA) ? 1 : 0;
}

/* Anti-Aliasing P3 (SSAA): the configured supersampling factor (AuroraConfig.
 * ssaa) in launcher mode, or 1.0 (native) in automation. Restart-pending like
 * msaa -- read once at aurora_initialize (main_native.c). Desktop-only: the
 * SSAA rows never appear on Android, so g_cfg.aa is never an SSAA value there,
 * and main_native.c only forwards this off __ANDROID__ anyway. */
extern "C" float mp6_launcher_cfg_ssaa(void)
{
    return g_launcherMode ? mp6_aa_to_ssaa(g_cfg.aa) : 1.0f;
}

/* Unlocked FPS (shim/include/mp6_unlocked_fps.h): the configured
 * tick-decoupled-presentation preference in launcher mode, or a fixed 0
 * (off -- present-count == tick-count exactly) in automation/pre-
 * launcher-init -- the same "automation never sees a non-default value"
 * contract as mp6_launcher_cfg_shadow_quality() above. Read LIVE every
 * tick by frame_interp.c, so the Mods toggle applies immediately; the
 * MP6_UNLOCKED_FPS env lever is resolved (and wins) inside
 * mp6_unlocked_fps_enabled(), not here. */
extern "C" int mp6_launcher_cfg_unlocked_fps(void)
{
    return g_launcherMode ? g_cfg.unlockedFps : 0;
}

/* =======================================================================
 * 5. Applying settings.
 * ======================================================================= */

static SDL_Window *g_window;

static void mp6_launcher_apply_display(void)
{
    if (g_window == NULL) return;

    if (g_cfg.windowMode == MP6_WINMODE_FULLSCREEN) {
        SDL_SetWindowFullscreen(g_window, true); /* SDL3: borderless desktop fullscreen */
    } else {
        SDL_SetWindowFullscreen(g_window, false);
        if (g_cfg.windowScale > 0.0f) {
            int w = (int)(640.0f * g_cfg.windowScale + 0.5f);
            int h = (int)(480.0f * g_cfg.windowScale + 0.5f);
            SDL_SetWindowSize(g_window, w, h);
        }
    }

    /* Window-shape preference only: constrain (or free) interactive
     * resizes now that the user's saved config is known. The CONTENT
     * framebuffer fit (AuroraSetViewportPolicy) is a gameplay-only
     * concern -- A5 moved it to mp6_bridge_apply_content_aspect_policy()
     * (aurora_bridge.c), applied once right before GameMain() via
     * mp6_launcher_cfg_aspect_locked() above. Before A5 this function
     * also flipped AuroraSetViewportPolicy live, which quietly
     * letterboxed the RmlUi launcher itself (menu/wordmark/watermark/
     * disc-info/version-info) for its ENTIRE lifetime on any non-4:3
     * display. The MP6_FREE_ASPECT env
     * lever keeps absolute priority in BOTH directions. */
    /* Widescreen ON means "the window
     * is freely resizable to any shape, and the render/camera/2D-shim all
     * track whatever shape it ends up" -- the exact opposite of
     * aspectLocked's 4:3 window-shape constraint, so widescreen wins
     * outright when both are set (a user turning Widescreen on on top of
     * an old "Lock 4:3" preference gets widescreen -- aspectLocked's OWN
     * value is untouched in the saved config either way, so turning
     * Widescreen back off later restores whatever aspectLocked preference
     * was already there). MP6_FREE_ASPECT keeps absolute top priority in
     * both directions, unchanged from before widescreen support existed. */
    if (getenv("MP6_FREE_ASPECT") == NULL) {
        /* WS: the MP6_WIDESCREEN env lever forces the free/widescreen window
         * shape here too -- same inline getenv semantics as
         * mp6_widescreen_enabled() (aurora_bridge.c) and MP6_FREE_ASPECT
         * above -- so the lever is consistent across the interactive
         * launcher's window-shape decision and automation mode (where it
         * already worked). A fresh interactive config never sets
         * g_cfg.widescreen, so without this an MP6_WIDESCREEN=1 launch would
         * still 4:3-lock and SDL would shrink a wide window (2200x720 ->
         * 960x720). Default-OFF (no env, widescreen off in config) is
         * byte-unchanged: envWide is 0 and the 4:3 aspectLocked branch runs
         * exactly as today. */
        const char *forceWide = getenv("MP6_WIDESCREEN");
        int envWide = (forceWide != NULL && forceWide[0] != '\0' && forceWide[0] != '0');
        if (g_cfg.widescreen || envWide) {
            SDL_SetWindowAspectRatio(g_window, 0.0f, 0.0f);
        } else if (g_cfg.aspectLocked) {
            SDL_SetWindowAspectRatio(g_window, 4.0f / 3.0f, 4.0f / 3.0f);
        } else {
            SDL_SetWindowAspectRatio(g_window, 0.0f, 0.0f);
        }
    }
}

static void mp6_launcher_apply_volume(void)
{
    mp6_audio_set_master_gain((float)g_cfg.masterVolume / 100.0f);
}

/* Content-root override validation uses the same parser and mandatory-file
 * manifest as the runtime DVD layer. This keeps a malformed/partial FST from
 * becoming "ready" in the launcher and crashing later in GameMain. */
static int mp6_launcher_validate_root(const char *root, char *err, size_t errn)
{
    if (root[0] == '\0') { if (errn) err[0] = '\0'; return 0; }
    return mp6_dvd_validate_disc_root(root, err, errn) ? 1 : -1;
}

extern "C" void mp6_launcher_apply_game_settings(void)
{
    if (!g_launcherMode) return;

    if (g_cfg.tickHz != 60.0 && getenv("MP6_TICK_HZ") == NULL) {
        char v[32];
        snprintf(v, sizeof(v), "%g", g_cfg.tickHz);
#ifdef _WIN32
        _putenv_s("MP6_TICK_HZ", v);
#else
        setenv("MP6_TICK_HZ", v, 0);
#endif
        printf("[LAUNCHER] tick rate from config: MP6_TICK_HZ=%s (config game.tick_hz; env would win if set)\n", v);
    }

    if (g_cfg.contentRoot[0] != '\0') {
        char err[1200];
        if (mp6_launcher_validate_root(g_cfg.contentRoot, err, sizeof(err)) > 0) {
            char files[1100], fst[1100];
            if (mp6_path_join_checked(files, sizeof(files), g_cfg.contentRoot, "files") != 0 ||
                mp6_path_join_checked(fst, sizeof(fst), g_cfg.contentRoot, "sys/fst.bin") != 0 ||
                mp6_dvd_set_root_override(files, fst) != 0) {
                printf("[LAUNCHER] configured game.content_root paths are too long -- using automatic resolution\n");
            }
        } else {
            printf("[LAUNCHER] configured game.content_root failed validation (%s) -- using automatic resolution\n", err);
        }
    }

    mp6_launcher_apply_volume();
    fflush(stdout);
}

/* =======================================================================
 * 6. Content-root state cache (revalidated on demand, not per frame --
 * validation hits the filesystem).
 * ======================================================================= */

static int g_rootState = -2;       /* -2 unprobed; else validate_root() result for the CONFIGURED root */
static int g_autoRootOk = 0;
static char g_autoRoot[1200];
static char g_rootErr[1200];
static char g_activeRootDisplay[1200];

static void mp6_recover_known_content_roots(void)
{
    if (g_cfg.contentRoot[0] != '\0') {
        mp6_import_recover_disc_root(g_cfg.contentRoot);
    }
#ifdef __ANDROID__
    {
        const char *bases[] = { SDL_GetAndroidExternalStoragePath(), SDL_GetAndroidInternalStoragePath() };
        for (const char *base : bases) {
            if (base != nullptr && base[0] != '\0') {
                char root[1200];
                if (mp6_path_join_checked(root, sizeof(root), base, "mp6/GP6E01") == 0) {
                    mp6_import_recover_disc_root(root);
                }
            }
        }
    }
#else
    {
        const char *base = SDL_GetBasePath();
        if (base != nullptr && base[0] != '\0') {
            char root[1200];
            if (mp6_path_join_checked(root, sizeof(root), base, "content/GP6E01") == 0) {
                mp6_import_recover_disc_root(root);
            }
        }
    }
#endif
}

static void mp6_refresh_content_state(void)
{
    mp6_recover_known_content_roots();
    g_rootState = mp6_launcher_validate_root(g_cfg.contentRoot, g_rootErr, sizeof(g_rootErr));
    g_autoRootOk = mp6_dvd_probe_root(g_autoRoot, sizeof(g_autoRoot)) != 0;
    if (g_rootState > 0) {
        mp6_path_copy_checked(g_activeRootDisplay, sizeof(g_activeRootDisplay), g_cfg.contentRoot);
    } else if (g_autoRootOk) {
        /* auto root is the FILES dir; display its parent (the disc root) */
        if (mp6_path_copy_checked(g_activeRootDisplay, sizeof(g_activeRootDisplay), g_autoRoot) != 0) {
            g_autoRootOk = 0;
            g_activeRootDisplay[0] = '\0';
            return;
        }
        size_t len = strlen(g_activeRootDisplay);
        if (len > 6 && strcmp(g_activeRootDisplay + len - 6, "/files") == 0) g_activeRootDisplay[len - 6] = '\0';
    } else {
        g_activeRootDisplay[0] = '\0';
    }
}

/* C-side probe for main_native.c's onboarding
 * decision -- "is bootable game content actually present right now?".
 * Launcher mode only by contract (call after mp6_launcher_decide_mode said
 * launcher); automation boots never consult it, so the L1 zero-[LAUNCHER]
 * byte-compat contract is untouched. Forces a fresh probe: this is the
 * boot-time re-validation the onboarding flow specifies. */
extern "C" int mp6_launcher_content_ready(void)
{
    if (!g_launcherMode) return 1; /* automation: never gate the boot */
    mp6_refresh_content_state();
    return g_rootState > 0 || (g_rootState == 0 && g_autoRootOk);
}

/* Resolves the active files root (configured override if valid, else the
 * auto-detected one) into buf; returns 0 on failure. */
static int mp6_active_files_root(char *buf, size_t n)
{
    char err[8];
    if (g_cfg.contentRoot[0] != '\0' && mp6_launcher_validate_root(g_cfg.contentRoot, err, sizeof(err)) > 0) {
        return mp6_path_join_checked(buf, n, g_cfg.contentRoot, "files") == 0;
    }
    return mp6_dvd_probe_root(buf, n) != 0;
}

/* =======================================================================
 * 7. Runtime-decoded wordmark/watermark (NO Nintendo art in the repo;
 * decoded from the user's own extracted disc at runtime). Served through
 * aurora's RmlUi runtime-texture provider ("mp6tex://wordmark" /
 * "mp6tex://watermark").
 *
 * data/title.bin layout (game/data.c GetFileInfo + game/decode.c
 * HuDecodeZlib, all u32 big-endian): entry-offset table at +4; each entry
 * is {u32 rawLen, u32 decodeType, payload}; decodeType 7 = zlib with
 * {u32 unused, u32 compLen, zlib stream}. Entries 0x13-0x15 are the title
 * HSFs ("HSFV037" magic + 21 {ofs,num} sections); the bitmap section
 * (index 9) records are 32 bytes, pixel data GC-tiled at poolBase =
 * secBase + num*32. "mp6_title_e" is 432x128 RGB5A3; "mario_ex" 384x384.
 * ======================================================================= */

struct Mp6LogoAssets {
    bool tried;
    unsigned char *wordmarkRgba; /* malloc'd once, retained for process lifetime */
    int wordmarkW, wordmarkH;
    unsigned char *watermarkRgba;
    int watermarkW, watermarkH;
};
static Mp6LogoAssets g_logo;

static unsigned mp6_be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3]; }
static unsigned mp6_be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | (unsigned)p[1]; }

/* RGB5A3 (GX_TF_RGB5A3): 4x4 texel tiles of big-endian u16. MSB set =
 * opaque RGB555; clear = A3RGB444. Matches tools/gc_tex_decode.py and the
 * GX hardware exactly. */
static void mp6_decode_rgb5a3(const unsigned char *src, int w, int h, unsigned char *rgba)
{
    const unsigned char *p = src;
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 4) {
            for (int i = 0; i < 16; i++, p += 2) {
                int x = tx + (i & 3), y = ty + (i >> 2);
                if (x >= w || y >= h) continue;
                unsigned v = mp6_be16(p);
                unsigned char *o = rgba + (size_t)(y * w + x) * 4;
                if (v & 0x8000) {
                    unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                    o[0] = (unsigned char)((r << 3) | (r >> 2));
                    o[1] = (unsigned char)((g << 3) | (g >> 2));
                    o[2] = (unsigned char)((b << 3) | (b >> 2));
                    o[3] = 255;
                } else {
                    unsigned a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
                    o[0] = (unsigned char)(r * 17);
                    o[1] = (unsigned char)(g * 17);
                    o[2] = (unsigned char)(b * 17);
                    o[3] = (unsigned char)((a << 5) | (a << 2) | (a >> 1));
                }
            }
        }
    }
}

/* Inflates archive entry `idx` of title.bin (NULL on any mismatch); caller
 * frees. Reads only the bytes it needs (the file is ~2 MB). */
static unsigned char *mp6_titlebin_entry(FILE *f, int idx, unsigned *outLen)
{
    unsigned char hdr[8], word[4];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(word, 1, 4, f) != 4) return NULL;
    unsigned count = mp6_be32(word);
    if ((unsigned)idx >= count || count > 4096) return NULL;
    if (fseek(f, 4 + idx * 4, SEEK_SET) != 0 || fread(word, 1, 4, f) != 4) return NULL;
    unsigned entryOfs = mp6_be32(word);
    if (fseek(f, (long)entryOfs, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) return NULL;
    unsigned rawLen = mp6_be32(hdr);
    unsigned decType = mp6_be32(hdr + 4);
    if (rawLen == 0 || rawLen > 64u * 1024u * 1024u) return NULL;

    if (decType == 7) { /* HU_DECODE_TYPE_ZLIB */
        unsigned char zhdr[8];
        if (fread(zhdr, 1, 8, f) != 8) return NULL;
        unsigned compLen = mp6_be32(zhdr + 4);
        if (compLen == 0 || compLen > 64u * 1024u * 1024u) return NULL;
        unsigned char *comp = (unsigned char *)malloc(compLen);
        unsigned char *raw = (unsigned char *)malloc(rawLen);
        if (comp == NULL || raw == NULL) { free(comp); free(raw); return NULL; }
        if (fread(comp, 1, compLen, f) != compLen) { free(comp); free(raw); return NULL; }
        uLongf dstLen = rawLen;
        int zr = uncompress(raw, &dstLen, comp, compLen);
        free(comp);
        if (zr != Z_OK || dstLen != rawLen) { free(raw); return NULL; }
        *outLen = rawLen;
        return raw;
    }
    if (decType == 0) { /* HU_DECODE_TYPE_NONE */
        unsigned char *raw = (unsigned char *)malloc(rawLen);
        if (raw == NULL) return NULL;
        if (fread(raw, 1, rawLen, f) != rawLen) { free(raw); return NULL; }
        *outLen = rawLen;
        return raw;
    }
    return NULL; /* other decode types don't occur for the title HSFs */
}

/* Finds bitmap `name` (RGB5A3 only) in an inflated HSF blob; returns a
 * malloc'd RGBA8 buffer (caller frees/retains) or NULL. */
static unsigned char *mp6_hsf_find_rgb5a3(const unsigned char *blob, unsigned len, const char *name, int *outW, int *outH)
{
    Mp6LauncherRgb5a3View view;
    unsigned char *rgba;
    if (!mp6_launcher_hsf_find_rgb5a3_view(blob, len, name, &view)) return NULL;
    rgba = (unsigned char *)malloc((size_t)view.width * (size_t)view.height * 4u);
    if (rgba == NULL) return NULL;
    mp6_decode_rgb5a3(view.pixels, view.width, view.height, rgba);
    *outW = view.width;
    *outH = view.height;
    return rgba;
}

/* Sets the interactive window title + icon. Launcher mode only; automation
 * keeps aurora's own appName title untouched. Title format = app name +
 * space + version (their portmain window-title shape; the name is ours). */
static void mp6_window_brand(void)
{
    if (g_window != NULL) {
        static char title[96];
        snprintf(title, sizeof(title), "Mario Party 6 \xE2\x80\x94 Native Port %s", MP6_PORT_VERSION);
        SDL_SetWindowTitle(g_window, title);
    }
}

static void mp6_window_icon_from_wordmark(const unsigned char *rgba, int w, int h)
{
    /* The '6'+star art occupies the wordmark's right end; crop a square
     * region there for the window icon. */
    if (g_window == NULL || w < 128 || h < 128) return;
    const int side = 120;
    int x0 = w - side - 4;
    int y0 = (h - side) / 2;
    static unsigned char icon[120 * 120 * 4];
    for (int y = 0; y < side; y++) {
        memcpy(icon + (size_t)y * side * 4, rgba + ((size_t)(y0 + y) * w + x0) * 4, (size_t)side * 4);
    }
    SDL_Surface *surf = SDL_CreateSurfaceFrom(side, side, SDL_PIXELFORMAT_RGBA32, icon, side * 4);
    if (surf != NULL) {
        SDL_SetWindowIcon(g_window, surf);
        SDL_DestroySurface(surf);
    }
}

static void mp6_logo_ensure(void)
{
    if (g_logo.tried) return;
    g_logo.tried = true;

    char filesRoot[1100];
    if (!mp6_active_files_root(filesRoot, sizeof(filesRoot))) {
        printf("[LAUNCHER] wordmark: no valid game content root -- using text fallback\n");
        fflush(stdout);
        return;
    }
    char binPath[1200];
    if (mp6_path_join_checked(binPath, sizeof(binPath), filesRoot, "data/title.bin") != 0) {
        printf("[LAUNCHER] wordmark: content path is too long -- using text fallback\n");
        fflush(stdout);
        return;
    }
    FILE *f = mp6_fopen_utf8(binPath, "rb");
    if (f == NULL) {
        printf("[LAUNCHER] wordmark: %s not found -- using text fallback\n", binPath);
        fflush(stdout);
        return;
    }

    /* The title-scene HSF is entry 0x14 today; scan its neighbors too so
     * a layout shift degrades to a scan. */
    static const int kTitleEntries[] = { 0x14, 0x13, 0x15 };
    for (size_t e = 0; e < sizeof(kTitleEntries) / sizeof(kTitleEntries[0]); e++) {
        if (g_logo.wordmarkRgba != NULL && g_logo.watermarkRgba != NULL) break;
        unsigned blobLen = 0;
        unsigned char *blob = mp6_titlebin_entry(f, kTitleEntries[e], &blobLen);
        if (blob == NULL) continue;

        if (g_logo.wordmarkRgba == NULL) {
            int w = 0, h = 0;
            unsigned char *rgba = mp6_hsf_find_rgb5a3(blob, blobLen, "mp6_title_e", &w, &h);
            if (rgba != NULL) {
                g_logo.wordmarkRgba = rgba; /* retained: the RmlUi provider serves from it */
                g_logo.wordmarkW = w;
                g_logo.wordmarkH = h;
                mp6_window_icon_from_wordmark(rgba, w, h);
                printf("[LAUNCHER] wordmark: decoded mp6_title_e %dx%d (RGB5A3) from %s entry 0x%x at runtime\n",
                       w, h, binPath, kTitleEntries[e]);
            }
        }
        if (g_logo.watermarkRgba == NULL) {
            int w = 0, h = 0;
            unsigned char *rgba = mp6_hsf_find_rgb5a3(blob, blobLen, "mario_ex", &w, &h);
            if (rgba != NULL) {
                g_logo.watermarkRgba = rgba;
                g_logo.watermarkW = w;
                g_logo.watermarkH = h;
                printf("[LAUNCHER] watermark: decoded mario_ex %dx%d (RGB5A3) from %s entry 0x%x at runtime\n",
                       w, h, binPath, kTitleEntries[e]);
            }
        }
        free(blob);
    }
    fclose(f);
    if (g_logo.wordmarkRgba == NULL) {
        printf("[LAUNCHER] wordmark: mp6_title_e not found in %s -- using text fallback\n", binPath);
    }
    fflush(stdout);
}

/* RmlUi runtime-texture provider: serves the decoded disc art to the ripped
 * documents as mp6tex://wordmark and mp6tex://watermark. */
static void mp6_register_texture_provider(void)
{
    aurora::rmlui::register_texture_provider("mp6tex", [](std::string_view name) -> std::optional<aurora::rmlui::RuntimeTexture> {
        mp6_logo_ensure();
        if (name == "mp6tex://wordmark" || name == "wordmark") {
            if (g_logo.wordmarkRgba == NULL) return std::nullopt;
            return aurora::rmlui::RuntimeTexture {
                (uint32_t)g_logo.wordmarkW, (uint32_t)g_logo.wordmarkH,
                std::span<const std::byte>((const std::byte *)g_logo.wordmarkRgba,
                                           (size_t)g_logo.wordmarkW * g_logo.wordmarkH * 4),
                false,
            };
        }
        if (name == "mp6tex://watermark" || name == "watermark") {
            if (g_logo.watermarkRgba == NULL) return std::nullopt;
            return aurora::rmlui::RuntimeTexture {
                (uint32_t)g_logo.watermarkW, (uint32_t)g_logo.watermarkH,
                std::span<const std::byte>((const std::byte *)g_logo.watermarkRgba,
                                           (size_t)g_logo.watermarkW * g_logo.watermarkH * 4),
                false,
            };
        }
        return std::nullopt;
    });
}

/* =======================================================================
 * 8. Resource resolution ([MP6]): res/ under the CWD if present (the
 * documented run-from-repo-root workflow), else next to the exe (build.py
 * stages a copy there for double-click runs).
 * ======================================================================= */

static char g_resBase[1200];

static const char *mp6_resource_base(void)
{
    if (g_resBase[0] != '\0') return g_resBase;

#ifdef __ANDROID__
    /* res/ ships INSIDE the APK as assets
     * (gradle syncs the repo's res/ tree -- platforms/android/app/
     * build.gradle). A bare relative "res" base keeps every resource path
     * relative ("res/rml/...", "res/fonts/..."), which SDL_IOFromFile
     * transparently opens from the APK asset system -- aurora's RmlUi
     * FileInterface is SDL_IOFromFile-backed, and main_native.c pins
     * config.resourcesPath to "" so aurora never prefixes SDL's "./"
     * base path onto asset paths (AAssetManager does no "./"
     * normalization). partyboard ships this exact mechanism. */
    if (mp6_path_copy_checked(g_resBase, sizeof(g_resBase), "res") != 0) return NULL;
    return g_resBase;
#else
    char probe[1400];
    SDL_PathInfo info;

    if (mp6_path_copy_checked(probe, sizeof(probe), "res/rml/window.rcss") != 0) return NULL;
    if (SDL_GetPathInfo(probe, &info) && info.type == SDL_PATHTYPE_FILE) {
        char abs[1200];
#ifdef _WIN32
        if (mp6_fullpath_utf8(abs, sizeof(abs), "res") == 0) {
            if (mp6_path_copy_checked(g_resBase, sizeof(g_resBase), abs) != 0) return NULL;
        } else
#endif
            if (mp6_path_copy_checked(g_resBase, sizeof(g_resBase), "res") != 0) return NULL;
    } else {
        const char *base = SDL_GetBasePath();
        if (mp6_path_join_checked(g_resBase, sizeof(g_resBase), base != NULL ? base : "", "res") != 0) {
            return NULL;
        }
    }
    for (char *p = g_resBase; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    return g_resBase;
#endif /* __ANDROID__ */
}

namespace mp6::ui {

const char *resource_base() { return mp6_resource_base(); }

void format_document_source(const char *raw, char *out, size_t n)
{
    /* Replace every `"res/` attribute-value prefix with `"<abs-base>/` --
     * keeps the ripped RML document sources byte-close to partyboard's
     * while loading from wherever our res/ actually is. */
    const char *base = mp6_resource_base();
    size_t o = 0;
    for (const char *p = raw; *p && o + 1 < n;) {
        if (p[0] == '"' && strncmp(p, "\"res/", 5) == 0) {
            int wrote = snprintf(out + o, n - o, "\"%s/", base);
            if (wrote < 0 || (size_t)wrote >= n - o) break;
            o += (size_t)wrote;
            p += 5;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

/* --- state accessors for the ripped UI --- */

Mp6LauncherConfig &cfg() { return g_cfg; }
void cfg_save() { mp6_launcher_config_save(); }
/* The trailing post-AA term covers the case mp6_launcher_aa_apply_live() now
 * DEFERS: picking FXAA while MSAA/SSAA owns the session leaves the config
 * asking for FXAA that isn't live, which is a restart-pending state exactly
 * like a changed sample count. */
bool restart_pending() { return strcmp(g_cfg.backend, g_initialBackend) != 0 || g_cfg.vsync != g_initialVsync || mp6_aa_to_msaa(g_cfg.aa) != g_initialMsaa || mp6_aa_to_ssaa(g_cfg.aa) != g_initialSsaa || (g_cfg.aa == MP6_AA_FXAA ? 1 : 0) != g_sessionPostAa; }
void apply_display() { mp6_launcher_apply_display(); }
void apply_volume() { mp6_launcher_apply_volume(); }
int validate_root(const char *root, char *err, size_t errn) { return mp6_launcher_validate_root(root, err, errn); }
int auto_root(char *buf, size_t n) { return mp6_dvd_probe_root(buf, n) != 0; }
bool content_ready() { if (g_rootState == -2) mp6_refresh_content_state(); return g_rootState > 0 || (g_rootState == 0 && g_autoRootOk); }
void refresh_content_state() { mp6_refresh_content_state(); }
const char *active_root_display() { if (g_rootState == -2) mp6_refresh_content_state(); return g_activeRootDisplay; }
const char *port_version() { return MP6_PORT_VERSION; }
const char *save_dir_abs() { return g_saveDirAbs; }

static bool s_playRequested = false;
static bool s_quitRequested = false;
void request_play() { s_playRequested = true; }
void request_quit() { s_quitRequested = true; }
bool play_requested() { return s_playRequested; }
bool quit_requested() { return s_quitRequested; }

bool wordmark_available() { mp6_logo_ensure(); return g_logo.wordmarkRgba != NULL; }
bool watermark_available() { mp6_logo_ensure(); return g_logo.watermarkRgba != NULL; }

} // namespace mp6::ui

/* =======================================================================
 * 9. UI bootstrap + the pre-boot menu loop + the in-game overlay hook.
 * ======================================================================= */

static bool g_uiReady = false;

/* Boot-time display apply (main_native.c, launcher mode only) -- also
 * bootstraps the RmlUi UI stack (fonts + persistent Overlay document +
 * texture provider), so `launcher.skip` boots still get the styled FPS
 * chip. Launcher mode only: automation windows never touch any of this. */
extern "C" void mp6_launcher_apply_display_settings(void *sdlWindowPtr)
{
    if (!g_launcherMode) return;
    g_window = (SDL_Window *)sdlWindowPtr;
    mp6_launcher_apply_display();
    mp6_window_brand();

    mp6_register_texture_provider();
    g_uiReady = mp6::ui::initialize();
    if (!g_uiReady) {
        printf("[LAUNCHER] RmlUi UI stack unavailable (context/fonts) -- menu will be skipped\n");
        fflush(stdout);
        return;
    }
    /* Passive overlay document (FPS chip + toasts), alive for the whole
     * process -- their portmain pushes Overlay the same way. */
    mp6::ui::push_document(std::make_unique<mp6::ui::Overlay>(), true, true);
}

/* FPS chip + toast pump for in-game frames, via the one-line
 * aurora_bridge.c hook. The overlay document persists after boot; RmlUi
 * records its (usually empty) surface inside aurora_end_frame. With
 * show_fps off the chip element stays closed; automation mode
 * (g_launcherMode false) never reaches any of this.
 *
 * ALSO the in-game menu host: on the FIRST in-game frame (this hook only
 * runs once GameMain's own frame loop is live -- the pre-boot menu loop
 * calls mp6::ui::update() directly) it pushes ONE persistent, hidden
 * SettingsWindow (inGame=true: adds the Save States tab, hides instead of
 * closing). F10 (aurora_bridge.c hotkey), F1/gamepad Back/R+Start/3-finger
 * tap (the ripped partyboard bindings, via the SDL-event forward below),
 * and the Android gear button all toggle it; while visible,
 * input.cpp's sync_input_block PADBlockInput()s the game -- the world
 * keeps ticking, its input is paused so menu navigation never leaks. */
static mp6::ui::SettingsWindow *g_gameMenu; /* owned by the ui document stack; never closed */

extern "C" void mp6_launcher_frame_overlay(void)
{
    if (!g_launcherMode || !g_uiReady) return;
    if (g_gameMenu == nullptr) {
        g_gameMenu = static_cast<mp6::ui::SettingsWindow *>(&mp6::ui::push_document(
            std::make_unique<mp6::ui::SettingsWindow>(false, 0, /*inGame=*/true), /*show=*/false));
        mp6::ui::show_menu_notification(); /* "Press F10 ... to open menu" toast */
    }
    /* Savestate feedback: a queued save/load was serviced at the frame
     * boundary -- surface the outcome as a toast (the Save States page's
     * own labels refresh via mp6_savestate_ui_generation()). */
    {
        int wasSave = 0, result = 0;
        if (mp6_savestate_take_last_result(&wasSave, &result)) {
            Rml::String content;
            if (result == MP6_SAVESTATE_OK) {
                content = wasSave ? "State saved." : "State loaded.";
            } else if (result == MP6_SAVESTATE_ERR_BINARY_MISMATCH) {
                content = "Incompatible (other build).";
            } else {
                content = mp6_savestate_strerror(result);
            }
            mp6::ui::push_toast({
                .type = "savestate",
                .title = wasSave ? "Save State" : "Load State",
                .content = content,
                .duration = std::chrono::seconds(4),
            });
        }
    }
    mp6::ui::update();
}

/* Pre-shutdown UI teardown. MUST run BEFORE aurora_shutdown(): the UI
 * document stacks are static (carved-out) vectors of live RmlUi documents;
 * left alone, the CRT destroys them AFTER aurora_shutdown()'s
 * rmlui::shutdown()/Rml::Shutdown() has already freed the context and
 * every ElementDocument -- Document::~Document then calls Close() on a
 * dead document (observed 0xC0000005 in Rml::Context::UnloadDocument from
 * Overlay::~Overlay at exit; a silent, longstanding teardown crash for
 * ANY interactive session, surfaced by exit-code-checking harness runs).
 * mp6::ui::shutdown() clears both stacks while the context is still
 * alive, releases the input block, and marks the UI uninitialized.
 * Automation mode: g_uiReady is never set, so this is a no-op branch --
 * zero log lines, zero behavior change (the automation contract). */
extern "C" void mp6_launcher_ui_teardown(void)
{
    if (!g_uiReady) return;
    g_gameMenu = nullptr; /* owned by the stack being cleared */
    mp6::ui::shutdown();
    g_uiReady = false;
}

/* Toggle the in-game menu (F10 via aurora_bridge.c's event pump; the
 * Android touch overlay's gear button). Inert in automation mode and
 * before the first in-game frame. */
extern "C" void mp6_launcher_toggle_menu(void)
{
    if (!g_launcherMode || !g_uiReady || g_gameMenu == nullptr) return;
    g_gameMenu->toggle();
}

/* Is ANY launcher UI document visible right now? Consumed by the freecam
 * input collector (suspend flying while menus are up) and the Android
 * touch overlay (hide the pad controls under the menu -- their input is
 * PADBlockInput()ed anyway). Automation mode: always 0. */
extern "C" int mp6_launcher_menu_visible(void)
{
    if (!g_launcherMode || !g_uiReady) return 0;
    return mp6::ui::any_document_visible() ? 1 : 0;
}

/* Game-time SDL event forward into the ripped UI framework (gamepad ->
 * RmlUi nav keys, the R+Start menu chord, the 3-finger menu tap,
 * controller connect/disconnect toasts). The pre-boot menu loop already
 * forwards its own events; this is the same call for in-game frames.
 * Mouse/keyboard/touch reach the RmlUi CONTEXT through aurora's own
 * process_event -> rmlui::handle_event regardless -- this adds only the
 * mp6::ui layer's own handling on top, exactly like the pre-boot loop. */
extern "C" void mp6_launcher_forward_sdl_event(const SDL_Event *ev)
{
    if (!g_launcherMode || !g_uiReady || ev == NULL) return;
    mp6::ui::handle_event(*ev);
}

extern "C" int mp6_launcher_run_menu(void *sdlWindowPtr)
{
    bool quit = false;
    uint64_t nextFrameNs = mp6_host_monotonic_ns();

    g_window = (SDL_Window *)sdlWindowPtr; /* display settings already applied by main_native.c */

    if (!g_uiReady) {
        /* Fonts/context missing (res/ not staged?) -- boot rather than
         * strand the user in an undrawable menu. */
        printf("[LAUNCHER] menu unavailable (UI stack not initialized) -- booting directly\n");
        fflush(stdout);
        return 0;
    }

    mp6_refresh_content_state();
    mp6::ui::push_document(std::make_unique<mp6::ui::Prelaunch>(), true);

    printf("[LAUNCHER] menu open (interactive launch) -- Play boots the game; MP6_LAUNCHER=0 or a "
           "tick-budget/--input-script/MP6_AUTO_START_TICKS invocation skips this menu entirely\n");
    fflush(stdout);

    while (!mp6::ui::play_requested() && !quit) {
        const AuroraEvent *event = aurora_update();
        while (event != NULL && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) quit = true;
            if (event->type == AURORA_SDL_EVENT) mp6::ui::handle_event(event->sdl);
            ++event;
        }
        if (mp6::ui::quit_requested()) quit = true;

        if (aurora_begin_frame()) {
            mp6::ui::update();
            aurora_end_frame();
        }

        /* ~60 Hz cap (absolute deadline, same discipline as the game's
         * tick throttle) -- present usually blocks on vsync already; this
         * only matters when vsync is off or begin_frame returns false. */
        nextFrameNs += 16666667ull;
        uint64_t now = mp6_host_monotonic_ns();
        if (nextFrameNs > now) {
            mp6_host_sleep_ns(nextFrameNs - now);
        } else {
            nextFrameNs = now;
        }
    }

    if (quit) {
        printf("[LAUNCHER] quit from menu\n");
        fflush(stdout);
        return 1;
    }
    printf("[LAUNCHER] Play -- booting the game\n");
    fflush(stdout);
    return 0;
}
