/* MP6 native port -- launcher state bridge.
 *
 * OUR file (not ripped): the interface between the MP6 launcher core
 * (launcher_core.cpp -- config model, mode decision, settings application,
 * disc-asset decode) and the RIPPED partyboard UI framework in this
 * directory (see docs/PARTYBOARD_PROVENANCE.md). The ripped widgets consume
 * plain getter/setter lambdas over this state where partyboard's originals
 * consumed its ConfigVar<T> registry -- our flat mp6_config.json store
 * (unchanged keys) stays the persistence layer.
 */
#pragma once

#include <cstddef>

enum Mp6WindowMode {
    MP6_WINMODE_WINDOWED = 0,
    MP6_WINMODE_FULLSCREEN = 1, /* SDL3 borderless desktop fullscreen */
};

struct Mp6LauncherConfig {
    int skipLauncher;       /* launcher.skip */
    int windowMode;         /* video.window_mode */
    float windowScale;      /* video.window_scale: 0 = leave alone; 1/1.5/2/3 = 640x480 * s */
    int aspectLocked;       /* video.aspect_locked */
    int vsync;              /* video.vsync (next launch) */
    char backend[24];       /* video.backend (next launch) */
    int showFps;            /* video.show_fps */
    int fpsCorner;          /* video.fps_corner: 0 TL, 1 TR, 2 BL, 3 BR */
    double tickHz;          /* game.tick_hz: 60 default; 0 = free-run */
    char contentRoot[1024]; /* game.content_root: "" = auto */
    int masterVolume;       /* audio.master_volume: 0..100 */
    /* WS2 (docs/WS2_DYNAMIC_WIDESCREEN.md): video.widescreen, default OFF
     * (= today's fixed 4:3 pillarboxed behavior, byte-unchanged). ON: the
     * window unlocks to free-resize (aspectLocked's own 4:3 window-shape
     * constraint is skipped regardless of aspectLocked's own value -- the
     * two are independent keys, but "locked to 4:3" and "dynamically
     * wide" are a contradiction if both were engaged at once), and the GX
     * render target / 3D camera aspect / 2D HUD placement all track the
     * LIVE window aspect continuously (platform/gx/aurora_bridge.c +
     * shim/include/mp6_widescreen.h) -- never a fixed 16:9, unlike the
     * reference ROM hack this generalizes. */
    int widescreen;         /* video.widescreen */
    /* Shadow Quality (Mods tab; shim/include/mp6_shadow_quality.h has the
     * full contract): the real-time projected shadow map's LINEAR scale,
     * applied at Hu3DShadowCreate time. 1 = native (byte-identical
     * off -- the sacred contract); valid non-native values are 2/4/8/16.
     * The tolerant load-time parser (launcher_core.cpp) maps anything else
     * back to 1. The EFFECTIVE scale actually applied can be smaller than
     * this if HEAP_MODEL doesn't have the headroom -- see
     * mp6_shadow_quality_scale(); this field always stores the user's own
     * last selection, not the clamped result, so re-selecting it after
     * freeing memory some other way can pick up the full setting again. */
    int shadowQuality;      /* video.shadow_quality */
};

namespace mp6::ui {

/* --- config store (launcher_core.cpp) --- */
Mp6LauncherConfig &cfg();
void cfg_save();            /* persist immediately (their setValue+config::Save() pairing) */

/* --- restart-pending (backend/vsync captured at launch) --- */
bool restart_pending();

/* --- settings application (live) --- */
void apply_display();       /* window mode/size/aspect (validates env levers) */
void apply_volume();        /* SDL stream master gain */

/* --- content root --- */
int validate_root(const char *root, char *err, size_t errn); /* 1 ok, 0 empty, -1 bad */
int auto_root(char *buf, size_t n);                          /* auto-detected files root; 0 if none */
bool content_ready();       /* configured-or-auto root currently valid (cached) */
void refresh_content_state();
const char *active_root_display(); /* for the prelaunch detail line */

/* --- misc display strings --- */
const char *port_version(); /* MP6_PORT_VERSION (git short hash) */
const char *save_dir_abs(); /* absolute save folder path (display / open) */

/* --- run-loop flags (prelaunch buttons -> mp6_launcher_run_menu loop) --- */
void request_play();
void request_quit();
bool play_requested();
bool quit_requested();

/* --- disc-decoded branding (RmlUi runtime-texture provider "mp6tex") --- */
bool wordmark_available();  /* provider serves mp6tex://wordmark */
bool watermark_available(); /* provider serves mp6tex://watermark */

/* --- resource resolution ([MP6]: res/ next to cwd, else next to exe) --- */
const char *resource_base();                 /* absolute res/ dir, forward slashes */
/* Replaces every `"res/` in a ripped RML source string with the resolved
 * absolute base -- keeps the ripped document sources byte-close to
 * partyboard's originals while loading from our layout. */
void format_document_source(const char *raw, char *out, size_t n);

} // namespace mp6::ui
