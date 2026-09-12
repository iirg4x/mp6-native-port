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
#include "mp6_touch_config.hpp"

enum Mp6WindowMode {
    MP6_WINMODE_WINDOWED = 0,
    MP6_WINMODE_FULLSCREEN = 1, /* SDL3 borderless desktop fullscreen */
};

/* One mutually exclusive AA mode. Optimized Windows builds apply the complete
 * selection between frames. Legacy shared backends latch MSAA/SSAA at startup
 * and allow FXAA only when neither is active. SSAA is desktop-only. */
enum Mp6AaMode {
    MP6_AA_OFF = 0,
    MP6_AA_MSAA4X = 1,
    MP6_AA_FXAA = 2,
    MP6_AA_SSAA15 = 3,
    MP6_AA_SSAA2X = 4,
};

struct Mp6LauncherConfig {
    int skipLauncher;       /* launcher.skip */
    int windowMode;         /* video.window_mode */
    float windowScale;      /* video.window_scale: 0 = leave alone; 1/1.5/2/3 = 640x480 * s */
    int aspectLocked;       /* video.aspect_locked */
    int vsync;              /* video.vsync -- the BOOT state; applies LIVE when toggled */
    char backend[24];       /* video.backend (next launch) */
    /* video.display: which attached output the window opens on. "auto" (the
     * default) = the output with the HIGHEST REFRESH RATE; "primary" = the OS
     * default placement, i.e. exactly the behaviour that predates this key;
     * anything else = a case-insensitive match against SDL_GetDisplayName,
     * degrading to "auto" with one printed line when nothing matches.
     *
     * This is the ONE key in this struct whose default deliberately does NOT
     * preserve the previous behaviour. A window that opens on the OS-default
     * primary is capped by the primary's refresh rate with vsync on, which on
     * a machine whose primary is a 75 Hz virtual display means presents can
     * never exceed 75/s no matter what the engine can do. Resolving an absent
     * key to "primary" would leave every existing config in exactly that
     * state, which is the state this key exists to fix. */
    char display[64];       /* video.display */
    /* video.window_x / video.window_y: the window's last position, in absolute
     * desktop coordinates. -1 means unset, which preserves the previous
     * behaviour exactly. Written once per session at clean shutdown, never per
     * move, and refused at launch when the position no longer lands on any
     * attached output (unplugging a monitor must not hide the window). */
    int windowX;            /* video.window_x */
    int windowY;            /* video.window_y */
    /* Anti-Aliasing (video.aa): the single Mp6AaMode enum above -- the source
     * of truth for all AA. P1's MSAA sample count, P2's live FXAA post pass
     * and P3's SSAA factor are all DERIVED from this one field (see
     * mp6_launcher_cfg_msaa/_post_aa/_ssaa, launcher_core.cpp). Offscreen
     * passes (shadow map, freecam depth-peek) are unaffected -- they render
     * at a fixed sampleCount of their own regardless of this value. */
    int aa;                  /* video.aa (Mp6AaMode) */
    int ambientOcclusion;  /* enhancements.ambient_occlusion: Off/Subtle/Strong, live */
    int showFps;            /* video.show_fps */
    int fpsCorner;          /* video.fps_corner: 0 TL, 1 TR, 2 BL, 3 BR */
    double tickHz;          /* game.tick_hz: 60 default; 0 = free-run */
    char contentRoot[1024]; /* game.content_root: "" = auto */
    int masterVolume;       /* audio.master_volume: 0..100 */
    /* video.widescreen, default OFF
     * (= today's fixed 4:3 pillarboxed behavior, byte-unchanged). ON: the
     * window unlocks to free-resize (aspectLocked's own 4:3 window-shape
     * constraint is skipped regardless of aspectLocked's own value -- the
     * two are independent keys, but "locked to 4:3" and "dynamically
     * wide" are a contradiction if both were engaged at once), and the GX
     * render target / 3D camera aspect / 2D HUD placement all track the
     * LIVE window aspect continuously (src/gx/aurora_bridge.c +
     * include/mp6_widescreen.h) -- never a fixed 16:9, unlike the
     * reference ROM hack this generalizes. */
    int widescreen;         /* video.widescreen */
    /* Shadow Quality (Mods tab; include/mp6_shadow_quality.h has the
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
    /* Unlocked FPS (Mods tab; include/mp6_unlocked_fps.h has the full
     * contract): tick-decoupled presentation -- game logic stays 60Hz,
     * extra display-rate frames replay the retained real-tick GX stream with
     * identity-paired matrices rewritten during the tick throttle's idle window.
     * Binary; default OFF (exact no-op: zero extra begin/end-frame pairs,
     * present-count == tick-count). Applies live -- frame_interp.c reads
     * the accessor every tick, no restart. MP6_UNLOCKED_FPS env wins over
     * this when set (automation never reads the config).
     *
     * There is no mechanism sub-setting: the retired "FPS Smoothing Mode"
     * row / video.fi_mode key selected an unsafe Hu3DExec re-run mode. An old config carrying
     * video.fi_mode is ignored by the tolerant parser (launcher_core.cpp),
     * exactly like the retired video.aspect_locked. */
    int unlockedFps;        /* video.unlocked_fps */
    /* Extended SFX voices (enhancements.sfx_voices): the mixer's voice table
     * size -- 16 is retail, 32 is the raise. UI + config only in this lane;
     * the mixer-side consumer (and the savestate version bump the table-size
     * change needs) land in the audio lane. Stored as the COUNT, not a
     * boolean, so a future third rung needs no key change. */
    int sfxVoices;          /* enhancements.sfx_voices: 16 or 32 */
    /* Expanded heaps (enhancements.heap_scale): the multiplier applied to
     * HuMem's capacities over HeapSizeTbl -- 1 is retail, 4 is the raise.
     * UI + config only in this lane; the HuMem-side consumer (and its
     * configured-size audit) land in the memory lane. */
    int heapScale;          /* enhancements.heap_scale: 1 or 4 */
    Mp6TouchConfig touch;   /* touch.* -- live, host-owned Android overlay */
};

namespace mp6::ui {

/* --- config store (launcher_core.cpp) --- */
Mp6LauncherConfig &cfg();
void cfg_save();            /* persist immediately (their setValue+config::Save() pairing) */

/* --- Enhancements presets (include/mp6_enhancements.h has the contract) ---
 * The settings UI never derives or applies a preset itself: the pure table and
 * the derivation live in src/enh/mp6_enhancements.c (which is what the
 * unit test drives), and these two bridge them to the config store. */
int enh_preset_current();          /* Mp6EnhPreset derived from the six live values */
void enh_preset_apply(int preset); /* write a tier's six values into cfg(); caller saves/applies */

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
