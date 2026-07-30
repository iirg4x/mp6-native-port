/* MP6 native port -- the enhancements seam (shim/include/mp6_enhancements.h
 * carries the full contract; read it first).
 *
 * Deliberately dependency-light: <stdlib.h> for getenv, <string.h> for the
 * name parsing, and nothing else. No decomp header, no Aurora header, no
 * SDL. That is a requirement, not a style preference, for three reasons:
 *
 *   1. this TU is in build.py's PLATFORM_SOURCES_COMMON, so it links into
 *      BOTH the windowed and the --headless build -- the heap-scale and
 *      voice-count consumers exist in both,
 *   2. it is compiled and driven directly by tools/enh_preset_selftest.c
 *      (tools/test_enhancements_contract.py), which has neither of those
 *      universes available,
 *   3. it must be callable from anywhere, including before the launcher
 *      exists at all.
 *
 * The store below is HOST-owned, never game state: it describes the settings
 * of the RUNNING process. A savestate captured with 32 voices must not
 * silently re-point a 16-voice session's settings when it is restored -- the
 * consumer lanes version their own tables for that. Hence the carve-out
 * include at the bottom of this file's include block and this TU's
 * membership in build.py's HOST_STATE_SECTION_SOURCES.
 */

#include "mp6_enhancements.h"

#include <stdlib.h>
#include <string.h>

/* SAVESTATE CARVE-OUT: host-owned statics (the published settings block)
 * must not be captured or restored. Must sit AFTER this TU's own includes and
 * at preprocessor TOP LEVEL (build.py rejects a conditionally-nested include).
 * See mp6_host_section.h. */
#include "mp6_host_section.h"


/* =======================================================================
 * 1. The preset table. PURE data -- the single place the three tiers are
 * spelled out, so the UI, the config defaults, the docs table and the unit
 * test can never drift apart.
 * ======================================================================= */

/* Anti-aliasing: the tiers pick FXAA, not MSAA, as the "AA on" rung.
 * Two reasons, both measured elsewhere in this port and both recorded in the
 * settings UI's own help text:
 *   - FXAA is a live post-process (aurora_set_post_aa), so switching presets
 *     changes AA immediately; MSAA and SSAA are aurora_initialize-time and
 *     would make every preset switch restart-pending,
 *   - MSAA is a documented performance trap on some mobile GPUs (a ~110 ->
 *     ~30 fps collapse on one device, backend-independent; see settings.cpp),
 *     and a shipped default must not be a coin flip on the user's phone.
 * A user who wants MSAA/SSAA picks it directly -- which derives Custom, which
 * is exactly what Custom is for.
 *
 * Shadow quality: 4x is the lowest rung that renders the shadow pass into the
 * dedicated offscreen target with a mip-resolved chain (1x is retail, 2x is
 * the lighter in-framebuffer path). 8x/16x work but are the rungs most likely
 * to hit mp6_shadow_quality_scale()'s HEAP_MODEL step-down, so they are a
 * deliberate user choice rather than a shipped default. */
static const Mp6EnhValues kPresetTable[] = {
    /* [0] unused slot for MP6_ENH_PRESET_CUSTOM -- Custom carries no values
     * of its own; asking for it yields the shipped default tier. */
    { 0, 0, 1, MP6_ENH_AA_OFF, 16, 1 },
    /* [1] MP6_ENH_PRESET_VANILLA -- exactly the GameCube. */
    { 0, 0, 1, MP6_ENH_AA_OFF, 16, 1 },
    /* [2] MP6_ENH_PRESET_VANILLA_PLUS -- the GameCube, smooth and clean:
     * authored 4:3 composition preserved, technical quality raised. */
    { 0, 1, 4, MP6_ENH_AA_FXAA, 32, 4 },
    /* [3] MP6_ENH_PRESET_MODERN -- full modern presentation. */
    { 1, 1, 4, MP6_ENH_AA_FXAA, 32, 4 },
};

#define MP6_ENH_PRESET_COUNT ((int)(sizeof(kPresetTable) / sizeof(kPresetTable[0])))

void mp6_enh_preset_values(int preset, Mp6EnhValues *out)
{
    if (out == NULL) {
        return;
    }
    if (preset <= MP6_ENH_PRESET_CUSTOM || preset >= MP6_ENH_PRESET_COUNT) {
        preset = MP6_ENH_PRESET_DEFAULT;
    }
    *out = kPresetTable[preset];
}

void mp6_enh_defaults(Mp6EnhValues *out)
{
    mp6_enh_preset_values(MP6_ENH_PRESET_DEFAULT, out);
}

static int enh_values_equal(const Mp6EnhValues *a, const Mp6EnhValues *b)
{
    return a->widescreen == b->widescreen && a->unlockedFps == b->unlockedFps &&
           a->shadowQuality == b->shadowQuality && a->aa == b->aa &&
           a->sfxVoices == b->sfxVoices && a->heapScale == b->heapScale;
}

int mp6_enh_preset_derive(const Mp6EnhValues *v)
{
    Mp6EnhValues probe;
    int preset;
    if (v == NULL) {
        return MP6_ENH_PRESET_CUSTOM;
    }
    /* Compare against the SANITIZED value set: a config carrying
     * shadow_quality 7 is not "Vanilla plus a weird shadow setting", it is a
     * config whose shadow setting degraded to retail -- and the label must
     * describe what the engine will actually do. */
    probe = *v;
    mp6_enh_values_sanitize(&probe);
    for (preset = MP6_ENH_PRESET_CUSTOM + 1; preset < MP6_ENH_PRESET_COUNT; ++preset) {
        if (enh_values_equal(&probe, &kPresetTable[preset])) {
            return preset;
        }
    }
    return MP6_ENH_PRESET_CUSTOM;
}

const char *mp6_enh_preset_name(int preset)
{
    switch (preset) {
        case MP6_ENH_PRESET_VANILLA:
            return "Vanilla";
        case MP6_ENH_PRESET_VANILLA_PLUS:
            return "Vanilla Plus";
        case MP6_ENH_PRESET_MODERN:
            return "Modern";
        default:
            return "Custom";
    }
}

/* Lowercases and folds '-'/'_'/' ' to a single space so the three spellings a
 * user, a config writer and a docs table naturally produce all parse the
 * same. Truncation is fine: any name longer than the buffer cannot match. */
static void enh_normalize_name(const char *name, char *out, size_t n)
{
    size_t w = 0;
    size_t i;
    if (n == 0) {
        return;
    }
    for (i = 0; name[i] != '\0' && w + 1 < n; ++i) {
        char c = name[i];
        if (c == '-' || c == '_') {
            c = ' ';
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        /* collapse runs of separators, and drop a leading one */
        if (c == ' ' && (w == 0 || out[w - 1] == ' ')) {
            continue;
        }
        out[w++] = c;
    }
    while (w > 0 && out[w - 1] == ' ') {
        --w; /* drop a trailing separator */
    }
    out[w] = '\0';
}

int mp6_enh_preset_from_name(const char *name)
{
    char norm[32];
    if (name == NULL) {
        return MP6_ENH_PRESET_CUSTOM;
    }
    enh_normalize_name(name, norm, sizeof(norm));
    if (strcmp(norm, "vanilla") == 0) {
        return MP6_ENH_PRESET_VANILLA;
    }
    if (strcmp(norm, "vanilla plus") == 0) {
        return MP6_ENH_PRESET_VANILLA_PLUS;
    }
    if (strcmp(norm, "modern") == 0) {
        return MP6_ENH_PRESET_MODERN;
    }
    return MP6_ENH_PRESET_CUSTOM;
}

void mp6_enh_values_sanitize(Mp6EnhValues *v)
{
    if (v == NULL) {
        return;
    }
    v->widescreen = v->widescreen ? 1 : 0;
    v->unlockedFps = v->unlockedFps ? 1 : 0;
    if (v->shadowQuality != 1 && v->shadowQuality != 2 && v->shadowQuality != 4 &&
        v->shadowQuality != 8 && v->shadowQuality != 16) {
        v->shadowQuality = 1; /* retail */
    }
    if (v->aa < MP6_ENH_AA_OFF || v->aa > MP6_ENH_AA_SSAA2X) {
        v->aa = MP6_ENH_AA_OFF; /* retail */
    }
    if (v->sfxVoices != 16 && v->sfxVoices != 32) {
        v->sfxVoices = 16; /* retail */
    }
    if (v->heapScale != 1 && v->heapScale != 4) {
        v->heapScale = 1; /* retail */
    }
}

/* =======================================================================
 * 2. The published store.
 *
 * Initialized to RETAIL, not to the shipped default preset. This is the whole
 * automation contract in one initializer: --headless and every automation-mode
 * run leave mp6_enh_set_values() uncalled, so every accessor answers exactly
 * what the GameCube did and no existing gate observes a new value. The
 * launcher's "Modern by default" is a CONFIG default (launcher_core.cpp) that
 * arrives here through set_values, never a default of this file.
 * ======================================================================= */

static Mp6EnhValues g_enh = { 0, 0, 1, MP6_ENH_AA_OFF, 16, 1 };

void mp6_enh_set_values(const Mp6EnhValues *v)
{
    if (v == NULL) {
        return;
    }
    g_enh = *v;
    mp6_enh_values_sanitize(&g_enh);
}

void mp6_enh_get_values(Mp6EnhValues *out)
{
    if (out != NULL) {
        *out = g_enh;
    }
}

/* =======================================================================
 * 3. Environment levers.
 *
 * getenv() per call, like every other lever in this port
 * (mp6_widescreen_enabled, mp6_shadow_quality_scale, MP6_TICK_HZ): the
 * environment cannot change under a running process, and none of these
 * accessors sits in a per-frame path -- heap scale is read once at heap
 * init, the voice count once at mixer init, and the render switches once
 * per scene load or per tick at worst. Not caching keeps this file free of
 * initialization order and of any "reset the latch" test seam.
 *
 * An empty value ("MP6_ENH_WIDESCREEN=") counts as UNSET, so an exported-but-
 * cleared variable behaves like no variable at all -- the same reading
 * mp6_launcher_decide_mode() already gives MP6_AUTO_START_TICKS.
 * ======================================================================= */

static const char *enh_env(const char *name)
{
    const char *v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : NULL;
}

/* The base every accessor falls back to: MP6_ENH_PRESET when it names a real
 * tier, else whatever the launcher published (retail when it published
 * nothing). */
static void enh_base(Mp6EnhValues *out)
{
    const char *presetEnv = enh_env("MP6_ENH_PRESET");
    if (presetEnv != NULL) {
        const int preset = mp6_enh_preset_from_name(presetEnv);
        if (preset != MP6_ENH_PRESET_CUSTOM) {
            mp6_enh_preset_values(preset, out);
            return;
        }
    }
    *out = g_enh;
}

/* Reads an integer lever. Returns 1 and writes *out when the variable is set
 * to a decimal integer, else 0. A set-but-unparseable lever is IGNORED rather
 * than treated as zero -- "MP6_ENH_SFX_VOICES=lots" must not silently mean
 * "retail". */
static int enh_env_int(const char *name, int *out)
{
    const char *v = enh_env(name);
    char *end = NULL;
    long parsed;
    if (v == NULL) {
        return 0;
    }
    parsed = strtol(v, &end, 10);
    if (end == v || end == NULL || *end != '\0') {
        return 0;
    }
    *out = (int)parsed;
    return 1;
}

/* Boolean levers accept 0/1 (and any other integer as "on", matching
 * MP6_WIDESCREEN's existing "any value that is not 0" reading). */
static int enh_env_bool(const char *name, int *out)
{
    int raw;
    if (!enh_env_int(name, &raw)) {
        return 0;
    }
    *out = raw != 0;
    return 1;
}

int mp6_enh_widescreen(void)
{
    Mp6EnhValues v;
    int lever;
    if (enh_env_bool("MP6_ENH_WIDESCREEN", &lever)) {
        return lever;
    }
    enh_base(&v);
    return v.widescreen ? 1 : 0;
}

int mp6_enh_unlocked_fps(void)
{
    Mp6EnhValues v;
    int lever;
    if (enh_env_bool("MP6_ENH_UNLOCKED_FPS", &lever)) {
        return lever;
    }
    enh_base(&v);
    return v.unlockedFps ? 1 : 0;
}

int mp6_enh_shadow_quality(void)
{
    Mp6EnhValues v;
    enh_base(&v);
    /* An off-ladder lever degrades to retail exactly like an off-ladder
     * config value would -- the sanitize below is the one ladder. */
    (void)enh_env_int("MP6_ENH_SHADOW_QUALITY", &v.shadowQuality);
    mp6_enh_values_sanitize(&v);
    return v.shadowQuality;
}

int mp6_enh_aa_mode(void)
{
    Mp6EnhValues v;
    enh_base(&v);
    (void)enh_env_int("MP6_ENH_AA", &v.aa);
    mp6_enh_values_sanitize(&v);
    return v.aa;
}

int mp6_enh_sfx_voices(void)
{
    Mp6EnhValues v;
    enh_base(&v);
    (void)enh_env_int("MP6_ENH_SFX_VOICES", &v.sfxVoices);
    mp6_enh_values_sanitize(&v);
    return v.sfxVoices;
}

int mp6_enh_heap_scale(void)
{
    Mp6EnhValues v;
    enh_base(&v);
    (void)enh_env_int("MP6_ENH_HEAP_SCALE", &v.heapScale);
    mp6_enh_values_sanitize(&v);
    return v.heapScale;
}
