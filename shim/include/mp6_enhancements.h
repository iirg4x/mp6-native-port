/* MP6 native port -- the ENHANCEMENTS seam.
 *
 * The port ships enhanced BY DEFAULT and every enhancement has a faithful-off
 * switch. This header is the single front door every consumer uses to ask
 * "is enhancement X on, and how far?" -- so a consumer never reads the
 * launcher config, never parses an environment variable, and never has to
 * know which of the two it is looking at.
 *
 * WHAT IS AND IS NOT AN ENHANCEMENT. An enhancement is a deliberate,
 * user-visible improvement over what the GameCube did, and it is therefore
 * switchable. Bug fixes, seam guards, and host-portability work are NOT
 * enhancements -- they are always on and have no switch. VSync is a host
 * display preference, not an enhancement; it stays on the Video page.
 *
 * The six switches (docs/SETTINGS.md has the user-facing table):
 *   1. Widescreen        -- dynamic true-wide render + camera/cull/UI re-derivation
 *   2. Unlocked FPS      -- interpolated presents between authored 60Hz ticks
 *   3. Shadow quality    -- the projected shadow map's linear scale ladder
 *   4. Anti-aliasing     -- the MSAA/SSAA/FXAA ladder
 *   5. Extended SFX voices -- the mixer's voice table, 16 (retail) or 32
 *   6. Expanded heaps    -- HuMem capacities at retail size or x4
 *
 * RESOLUTION ORDER, identical for every accessor below:
 *
 *   1. that switch's own MP6_ENH_* environment lever, when set,
 *   2. else MP6_ENH_PRESET (vanilla | vanilla-plus | modern), when set,
 *   3. else the value the launcher published from mp6_config.json,
 *   4. else -- and this is the load-bearing default -- RETAIL.
 *
 * Step 4 is why this header is safe to consult from anywhere, including the
 * headless build and automation mode. docs/TESTING.md's automation contract
 * is that an automated invocation boots byte-identically to a build with no
 * launcher at all; automation never reads mp6_config.json, so nothing ever
 * calls mp6_enh_set_values() there and every accessor answers exactly what
 * the GameCube did. The MP6_ENH_* levers exist precisely so a scripted gate
 * can still opt INTO an enhancement without a config file.
 *
 * The launcher's own "Modern by default" lives one layer up, in the config
 * defaults (platform/gx/ui/launcher_core.cpp): a first interactive run with
 * no mp6_config.json gets the Modern preset and publishes it here. That is a
 * user-facing default, not a default of this seam.
 *
 * PRESETS. Three named tiers plus a derived "Custom":
 *   Vanilla      -- exactly the GameCube. Every switch at retail.
 *   Vanilla Plus -- the GameCube, smooth and clean: the authored 4:3
 *                   composition is preserved, technical quality is raised.
 *   Modern       -- full modern presentation, widescreen included.
 * Selecting a preset writes all six values. The preset LABEL is never
 * authoritative: it is derived from the six values with
 * mp6_enh_preset_derive(), which answers Custom when they match no tier.
 * That is what keeps a hand-edited config, an old config, and the UI from
 * ever disagreeing about which tier is showing.
 *
 * The preset table and the derivation are PURE (no config, no environment,
 * no I/O) so they are unit-testable on their own -- tools/enh_preset_selftest.c
 * compiles platform/enh/mp6_enhancements.c and drives them directly.
 */
#ifndef MP6_ENHANCEMENTS_H
#define MP6_ENHANCEMENTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Resolved accessors -- the seam. Cheap; safe to call before the launcher
 * has run (they answer retail until it publishes).
 *
 * WHEN A CHANGE TAKES EFFECT is the consumer's contract, not this seam's.
 * Every accessor here answers live, every time; but two consumers LATCH the
 * answer because their state cannot be resized under a running game, so
 * changing those two mid-session is restart-pending. The latch belongs to
 * the consumer because only the consumer knows when its state is quiescent.
 * Each declaration below says which it is; a restart-pending switch must
 * also be reported by restart_pending() (platform/gx/ui/launcher_core.cpp),
 * which is what actually offers the user the relaunch.
 * --------------------------------------------------------------------- */

int mp6_enh_widescreen(void);      /* 0 = fixed 4:3, 1 = dynamic true-wide. LIVE. */
int mp6_enh_unlocked_fps(void);    /* 0 = present == tick, 1 = interpolated presents. LIVE. */
int mp6_enh_shadow_quality(void);  /* shadow-map linear scale: 1 (retail), 2, 4, 8, 16. LIVE. */
int mp6_enh_aa_mode(void);         /* Mp6EnhAaMode below; 0 = off (retail). Restart-pending. */

/* 16 (retail) or 32. LATCHED by platform/audio/msm_bridge.c at msmSysInit and
 * held for the whole run: a voice's SLOT INDEX is its identity (mixer order,
 * first-free allocation order, savestate restore target), so shrinking the
 * table under live voices would strand them and growing it inside a scene
 * would change allocation order mid-scene. Restart-pending, the same shape AA
 * already had. */
int mp6_enh_sfx_voices(void);

/* 1 (retail) or 4. LATCHED process-wide by platform/os/heap_scale.c on first
 * resolve: the host arena reservation and HuMemInitAll run thousands of
 * instructions apart, and an arena sized for one scale under heaps sized for
 * another is unrecoverable. Restart-pending. */
int mp6_enh_heap_scale(void);

/* ---------------------------------------------------------------------
 * Value model.
 * --------------------------------------------------------------------- */

/* Mirrors Mp6AaMode (platform/gx/ui/launcher_state.hpp), which is the C++
 * side's own name for the same numbers. launcher_core.cpp static_asserts the
 * two agree, so this header stays consumable by plain C that must not pull in
 * the launcher's C++ headers. */
enum Mp6EnhAaMode {
    MP6_ENH_AA_OFF = 0,
    MP6_ENH_AA_MSAA4X = 1,
    MP6_ENH_AA_FXAA = 2,
    MP6_ENH_AA_SSAA15 = 3,
    MP6_ENH_AA_SSAA2X = 4
};

enum Mp6EnhPreset {
    MP6_ENH_PRESET_CUSTOM = 0,
    MP6_ENH_PRESET_VANILLA = 1,
    MP6_ENH_PRESET_VANILLA_PLUS = 2,
    MP6_ENH_PRESET_MODERN = 3
};

/* The shipped default tier. Changing this one constant changes what a fresh
 * install comes up as, and nothing else -- every other site derives. */
#define MP6_ENH_PRESET_DEFAULT MP6_ENH_PRESET_MODERN

typedef struct Mp6EnhValues {
    int widescreen;    /* 0 / 1 */
    int unlockedFps;   /* 0 / 1 */
    int shadowQuality; /* 1 / 2 / 4 / 8 / 16 */
    int aa;            /* Mp6EnhAaMode */
    int sfxVoices;     /* 16 / 32 */
    int heapScale;     /* 1 / 4 */
} Mp6EnhValues;

/* ---------------------------------------------------------------------
 * Pure preset logic (no config, no environment, no I/O).
 * --------------------------------------------------------------------- */

/* Fills *out with the six values of `preset`. MP6_ENH_PRESET_CUSTOM and any
 * unknown value fill the shipped default tier -- "Custom" names a set of
 * values, it does not carry any of its own. */
void mp6_enh_preset_values(int preset, Mp6EnhValues *out);

/* The shipped defaults: mp6_enh_preset_values(MP6_ENH_PRESET_DEFAULT, out). */
void mp6_enh_defaults(Mp6EnhValues *out);

/* The derived label: the tier whose six values *v matches exactly, else
 * MP6_ENH_PRESET_CUSTOM. This is the ONLY way a preset name is ever produced
 * -- the name stored in the config is a write-only convenience. */
int mp6_enh_preset_derive(const Mp6EnhValues *v);

/* Display name ("Vanilla", "Vanilla Plus", "Modern", "Custom"). Never NULL. */
const char *mp6_enh_preset_name(int preset);

/* Parse a stored/typed name back to a preset. Case-insensitive; spaces,
 * hyphens and underscores are interchangeable ("Vanilla Plus" ==
 * "vanilla-plus" == "VANILLA_PLUS"). Unknown text -> MP6_ENH_PRESET_CUSTOM. */
int mp6_enh_preset_from_name(const char *name);

/* Forces every field onto its ladder. An out-of-ladder field is replaced by
 * its RETAIL value, never by a preset value -- the same tolerance the flat
 * config parser has always had for video.shadow_quality / video.aa: garbage
 * degrades to the faithful setting rather than to a guess. */
void mp6_enh_values_sanitize(Mp6EnhValues *v);

/* ---------------------------------------------------------------------
 * The published store. The launcher writes; everything else reads.
 * --------------------------------------------------------------------- */

/* Publish the user's resolved settings. Sanitizes as it stores. Called by the
 * launcher after config load and after every settings change; never called in
 * automation mode, which is what keeps automation at retail. */
void mp6_enh_set_values(const Mp6EnhValues *v);

/* The currently published values, WITHOUT the environment levers applied --
 * i.e. what the config says, which is what the settings UI must show and
 * write back. Use the accessors above for what the engine should actually do. */
void mp6_enh_get_values(Mp6EnhValues *out);

#ifdef __cplusplus
}
#endif

#endif /* MP6_ENHANCEMENTS_H */
