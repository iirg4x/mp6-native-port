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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

#include "launcher_state.hpp"
#include "overlay.hpp"
#include "prelaunch.hpp"
#include "ui.hpp"

extern "C" {
#include "host.h" /* mp6_host_monotonic_ns / mp6_host_sleep_ns / mp6_host_save_dir */

/* Additive hooks in existing platform files (see their own comments): */
void mp6_dvd_set_root_override(const char *filesRoot, const char *fstPath); /* platform/dvd/dvd_files.c */
int mp6_dvd_probe_root(char *filesRootOut, size_t n);                       /* platform/dvd/dvd_files.c */
void mp6_audio_set_master_gain(float gain);                                 /* platform/audio/audio_out_sdl.c */
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
    g_cfg.showFps = 0;
    g_cfg.fpsCorner = 0;
    g_cfg.tickHz = 60.0;
    g_cfg.contentRoot[0] = '\0';
    g_cfg.masterVolume = 100;
}

/* =======================================================================
 * 2. Flat JSON read/write (tolerant by construction: unknown keys are
 * ignored, malformed input keeps defaults for the rest).
 * ======================================================================= */

static const char *js_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    return p;
}

static const char *js_parse_string(const char *p, char *buf, size_t n)
{
    size_t i = 0;
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && (*p == '"' || *p == '\\' || *p == '/')) {
            c = *p++;
        }
        if (i + 1 < n) buf[i++] = c;
    }
    if (*p != '"') return NULL;
    buf[i] = '\0';
    return p + 1;
}

static void mp6_launcher_parse_config(const char *text)
{
    char key[64];
    char sval[1024];
    const char *p = js_skip_ws(text);

    if (*p != '{') {
        printf("[LAUNCHER] config.json does not start with '{' -- using defaults\n");
        return;
    }
    p = js_skip_ws(p + 1);

    while (*p && *p != '}') {
        p = js_parse_string(p, key, sizeof(key));
        if (p == NULL) { printf("[LAUNCHER] config.json: malformed key -- stopping parse, defaults kept for the rest\n"); return; }
        p = js_skip_ws(p);
        if (*p != ':') { printf("[LAUNCHER] config.json: missing ':' after \"%s\" -- stopping parse\n", key); return; }
        p = js_skip_ws(p + 1);

        if (*p == '"') { /* string value */
            p = js_parse_string(p, sval, sizeof(sval));
            if (p == NULL) { printf("[LAUNCHER] config.json: malformed string for \"%s\" -- stopping parse\n", key); return; }
            if (strcmp(key, "video.window_mode") == 0) {
                g_cfg.windowMode = (strcmp(sval, "fullscreen") == 0) ? MP6_WINMODE_FULLSCREEN : MP6_WINMODE_WINDOWED;
            } else if (strcmp(key, "video.backend") == 0) {
                snprintf(g_cfg.backend, sizeof(g_cfg.backend), "%s", sval);
            } else if (strcmp(key, "game.content_root") == 0) {
                snprintf(g_cfg.contentRoot, sizeof(g_cfg.contentRoot), "%s", sval);
            } /* unknown string keys: ignored */
        } else if (strncmp(p, "true", 4) == 0 || strncmp(p, "false", 5) == 0) {
            int v = (*p == 't');
            p += v ? 4 : 5;
            if      (strcmp(key, "launcher.skip") == 0)       g_cfg.skipLauncher = v;
            else if (strcmp(key, "video.aspect_locked") == 0) g_cfg.aspectLocked = v;
            else if (strcmp(key, "video.vsync") == 0)         g_cfg.vsync = v;
            else if (strcmp(key, "video.show_fps") == 0)      g_cfg.showFps = v;
        } else { /* number */
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p) { printf("[LAUNCHER] config.json: malformed value for \"%s\" -- stopping parse\n", key); return; }
            p = end;
            if      (strcmp(key, "video.window_scale") == 0)  g_cfg.windowScale = (float)v;
            else if (strcmp(key, "video.fps_corner") == 0) {
                g_cfg.fpsCorner = (int)v;
                if (g_cfg.fpsCorner < 0 || g_cfg.fpsCorner > 3) g_cfg.fpsCorner = 0;
            }
            else if (strcmp(key, "game.tick_hz") == 0)        g_cfg.tickHz = (v >= 0.0 ? v : 60.0);
            else if (strcmp(key, "audio.master_volume") == 0) {
                g_cfg.masterVolume = (int)v;
                if (g_cfg.masterVolume < 0) g_cfg.masterVolume = 0;
                if (g_cfg.masterVolume > 100) g_cfg.masterVolume = 100;
            }
        }
        p = js_skip_ws(p);
    }
}

static void js_escape(const char *src, char *dst, size_t n)
{
    size_t i = 0;
    for (; *src && i + 2 < n; src++) {
        if (*src == '"' || *src == '\\') dst[i++] = '\\';
        dst[i++] = *src;
    }
    dst[i] = '\0';
}

static void mp6_launcher_config_save(void)
{
    char rootEsc[2048];
    FILE *f;
    if (g_configPath[0] == '\0') return;
    f = fopen(g_configPath, "wb");
    if (f == NULL) {
        printf("[LAUNCHER] could not write %s\n", g_configPath);
        return;
    }
    js_escape(g_cfg.contentRoot, rootEsc, sizeof(rootEsc));
    fprintf(f,
            "{\n"
            "    \"launcher.skip\": %s,\n"
            "    \"video.window_mode\": \"%s\",\n"
            "    \"video.window_scale\": %g,\n"
            "    \"video.aspect_locked\": %s,\n"
            "    \"video.vsync\": %s,\n"
            "    \"video.backend\": \"%s\",\n"
            "    \"video.show_fps\": %s,\n"
            "    \"video.fps_corner\": %d,\n"
            "    \"game.tick_hz\": %g,\n"
            "    \"game.content_root\": \"%s\",\n"
            "    \"audio.master_volume\": %d\n"
            "}\n",
            g_cfg.skipLauncher ? "true" : "false",
            g_cfg.windowMode == MP6_WINMODE_FULLSCREEN ? "fullscreen" : "windowed",
            (double)g_cfg.windowScale,
            g_cfg.aspectLocked ? "true" : "false",
            g_cfg.vsync ? "true" : "false",
            g_cfg.backend,
            g_cfg.showFps ? "true" : "false",
            g_cfg.fpsCorner,
            g_cfg.tickHz,
            rootEsc,
            g_cfg.masterVolume);
    fclose(f);
}

/* =======================================================================
 * 3. Config location: next to the exe.
 * ======================================================================= */

static void mp6_launcher_resolve_paths(void)
{
#ifdef __ANDROID__
    /*  SDL_GetBasePath() is "./" on Android
     * (the app has no exe directory and cwd is "/", unwritable) -- the
     * config lives under the same base dir mp6_android_main publishes for
     * disc/save data (external files dir preferred), which is exported as
     * MP6_HOST_BASE BEFORE aurora_main runs. */
    const char *hostBase = getenv("MP6_HOST_BASE");
    if (hostBase != NULL && hostBase[0] != '\0') {
        snprintf(g_configPath, sizeof(g_configPath), "%s/mp6_config.json", hostBase);
    } else {
        g_configPath[0] = '\0'; /* no writable base: defaults only, no save */
    }
#else
    const char *base = SDL_GetBasePath();
    if (base != NULL && base[0] != '\0') {
        snprintf(g_configPath, sizeof(g_configPath), "%smp6_config.json", base);
    } else {
        snprintf(g_configPath, sizeof(g_configPath), "mp6_config.json"); /* cwd fallback */
    }
#endif

    {
        char rel[64];
        if (mp6_host_save_dir(rel, sizeof(rel)) != 0) snprintf(rel, sizeof(rel), "saves");
#if defined(__ANDROID__)
        /* Display-only: saves resolve under MP6_HOST_BASE on device. */
        const char *saveBase = getenv("MP6_HOST_BASE");
        if (saveBase != NULL && saveBase[0] != '\0') {
            snprintf(g_saveDirAbs, sizeof(g_saveDirAbs), "%s/%s", saveBase, rel);
        } else
#elif defined(_WIN32)
        if (_fullpath(g_saveDirAbs, rel, sizeof(g_saveDirAbs)) == NULL)
#endif
            snprintf(g_saveDirAbs, sizeof(g_saveDirAbs), "%s", rel);
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
        f = fopen(g_configPath, "rb");
        if (f != NULL) {
            static char text[8192];
            size_t got = fread(text, 1, sizeof(text) - 1, f);
            text[got] = '\0';
            fclose(f);
            mp6_launcher_parse_config(text);
            printf("[LAUNCHER] config loaded from %s\n", g_configPath);
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

/* Launch-time values of the two next-launch settings, for the restart-
 * pending check (the ripped prelaunch shows its "Apply Options" modal when
 * these differ from the current config). */
static char g_initialBackend[24];
static int g_initialVsync = 1;

static void mp6_launcher_capture_initials(void)
{
    snprintf(g_initialBackend, sizeof(g_initialBackend), "%s", g_cfg.backend);
    g_initialVsync = g_cfg.vsync;
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
     * display -- . The MP6_FREE_ASPECT env
     * lever keeps absolute priority in BOTH directions. */
    if (getenv("MP6_FREE_ASPECT") == NULL) {
        if (g_cfg.aspectLocked) {
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

/* Content-root override validation: the load-bearing file
 * is <root>/sys/fst.bin plus a <root>/files directory. */
static int mp6_launcher_validate_root(const char *root, char *err, size_t errn)
{
    char path[1200];
    SDL_PathInfo info;
    if (root[0] == '\0') { if (errn) err[0] = '\0'; return 0; }

    snprintf(path, sizeof(path), "%s/sys/fst.bin", root);
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
        snprintf(err, errn, "not found: %s", path);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/files", root);
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        snprintf(err, errn, "not found: %s (directory)", path);
        return -1;
    }
    if (errn) err[0] = '\0';
    return 1;
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
            snprintf(files, sizeof(files), "%s/files", g_cfg.contentRoot);
            snprintf(fst, sizeof(fst), "%s/sys/fst.bin", g_cfg.contentRoot);
            mp6_dvd_set_root_override(files, fst);
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
static char g_autoRoot[1100];
static char g_rootErr[1200];
static char g_activeRootDisplay[1100];

static void mp6_refresh_content_state(void)
{
    g_rootState = mp6_launcher_validate_root(g_cfg.contentRoot, g_rootErr, sizeof(g_rootErr));
    g_autoRootOk = mp6_dvd_probe_root(g_autoRoot, sizeof(g_autoRoot)) != 0;
    if (g_rootState > 0) {
        snprintf(g_activeRootDisplay, sizeof(g_activeRootDisplay), "%s", g_cfg.contentRoot);
    } else if (g_autoRootOk) {
        /* auto root is the FILES dir; display its parent (the disc root) */
        snprintf(g_activeRootDisplay, sizeof(g_activeRootDisplay), "%s", g_autoRoot);
        size_t len = strlen(g_activeRootDisplay);
        if (len > 6 && strcmp(g_activeRootDisplay + len - 6, "/files") == 0) g_activeRootDisplay[len - 6] = '\0';
    } else {
        g_activeRootDisplay[0] = '\0';
    }
}

/*  C-side probe for main_native.c's onboarding
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
        snprintf(buf, n, "%s/files", g_cfg.contentRoot);
        return 1;
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
    if (len < 176 || memcmp(blob, "HSFV", 4) != 0) return NULL;
    unsigned bmpOfs = mp6_be32(blob + 8 + 9 * 8);
    int bmpNum = (int)mp6_be32(blob + 12 + 9 * 8);
    unsigned strOfs = mp6_be32(blob + 8 + 20 * 8);
    if (bmpNum <= 0 || bmpNum > 4096 || bmpOfs >= len || strOfs >= len) return NULL;

    for (int i = 0; i < bmpNum; i++) {
        unsigned rec = bmpOfs + (unsigned)i * 32u;
        if (rec + 32 > len) return NULL;
        unsigned nameOfs = mp6_be32(blob + rec);
        if (strOfs + nameOfs >= len) continue;
        const char *bmpName = (const char *)(blob + strOfs + nameOfs);
        size_t maxN = len - (strOfs + nameOfs);
        if (strncmp(bmpName, name, maxN) != 0) continue;

        unsigned dataFmt = blob[rec + 8];
        unsigned pixSize = blob[rec + 9];
        int w = (int)mp6_be16(blob + rec + 10);
        int h = (int)mp6_be16(blob + rec + 12);
        unsigned dataOfs = mp6_be32(blob + rec + 28);
        if (dataFmt != 5 /* HSF_BMPFMT_RGB5A3 */ || pixSize != 16 || w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
        unsigned pool = bmpOfs + (unsigned)bmpNum * 32u;
        unsigned nbytes = (unsigned)w * (unsigned)h * 2u;
        if (pool + dataOfs + nbytes > len) return NULL;

        unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
        if (rgba == NULL) return NULL;
        mp6_decode_rgb5a3(blob + pool + dataOfs, w, h, rgba);
        *outW = w;
        *outH = h;
        return rgba;
    }
    return NULL;
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
    snprintf(binPath, sizeof(binPath), "%s/data/title.bin", filesRoot);
    FILE *f = fopen(binPath, "rb");
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
    /*  res/ ships INSIDE the APK as assets
     * (gradle syncs the repo's res/ tree -- platforms/android/app/
     * build.gradle). A bare relative "res" base keeps every resource path
     * relative ("res/rml/...", "res/fonts/..."), which SDL_IOFromFile
     * transparently opens from the APK asset system -- aurora's RmlUi
     * FileInterface is SDL_IOFromFile-backed, and main_native.c pins
     * config.resourcesPath to "" so aurora never prefixes SDL's "./"
     * base path onto asset paths (AAssetManager does no "./"
     * normalization). partyboard ships this exact mechanism. */
    snprintf(g_resBase, sizeof(g_resBase), "res");
    return g_resBase;
#else
    char probe[1400];
    SDL_PathInfo info;

    snprintf(probe, sizeof(probe), "res/rml/window.rcss");
    if (SDL_GetPathInfo(probe, &info) && info.type == SDL_PATHTYPE_FILE) {
        char abs[1200];
#ifdef _WIN32
        if (_fullpath(abs, "res", sizeof(abs)) != NULL) {
            snprintf(g_resBase, sizeof(g_resBase), "%s", abs);
        } else
#endif
            snprintf(g_resBase, sizeof(g_resBase), "res");
    } else {
        const char *base = SDL_GetBasePath();
        snprintf(g_resBase, sizeof(g_resBase), "%sres", base != NULL ? base : "");
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
bool restart_pending() { return strcmp(g_cfg.backend, g_initialBackend) != 0 || g_cfg.vsync != g_initialVsync; }
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
 * (g_launcherMode false) never reaches any of this. */
extern "C" void mp6_launcher_frame_overlay(void)
{
    if (!g_launcherMode || !g_uiReady) return;
    mp6::ui::update();
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
