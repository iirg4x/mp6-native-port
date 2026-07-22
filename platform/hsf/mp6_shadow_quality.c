/* MP6 native port -- Mods-page Shadow Quality: origin-site scale helper.
 * See shim/include/mp6_shadow_quality.h for the full contract and the
 * citation of every consumer this feeds.
 *
 * Lives in platform/hsf/ (alongside mp6_freecam.c and
 * mp6_widescreen_extrude.c) because it needs game/memory.h (HEAPID/
 * HuMemHeapPtrGet/HuMemMaxMemorySizeGet) -- COMMON_FLAGS' decomp -I's make
 * that available in BOTH build modes, so this one TU (tools/build.py's
 * PLATFORM_SOURCES_COMMON) covers both, split internally by #ifdef
 * MP6_HEADLESS_BUILD exactly like platform/null/shims_manual.c already
 * does for mp6_widescreen_enabled()/mp6_widescreen_scale_factor().
 *
 * HOST STATICS (carved): the clamp-log latch below is host-owned log
 * wiring that must not ride savestates, so this TU takes the standard
 * mp6_host_section.h carve-out exactly like select_button.cpp documents.
 */
#include "game/memory.h" /* HEAPID, HuMemHeapPtrGet, HuMemMaxMemorySizeGet */
#include "mp6_shadow_quality.h"

#include <stdio.h>
#include <stdlib.h>

#include "mp6_host_section.h"

/* One [SHADOW] line per distinct clamp (host log latch, never captured). */
static int loggedRequested;

int mp6_shadow_effective_size(int size)
{
    int requested = mp6_shadow_quality_scale(size);
    int eff = requested;

    if (requested <= 1) {
        return size; /* native/off: exact pass-through, the byte-identical
                      * contract lives here. */
    }
#ifdef MP6_HEADLESS_BUILD
    /* No renderer, no offscreen shadow pass: the in-EFB pass's own detail
     * ceiling is 2x (dst == the 2x-supersampled src, pixel for pixel).
     * Structurally unreachable today -- mp6_shadow_quality_scale() is
     * fixed 1 headless -- kept as the documented fallback shape should
     * that ever change. */
    if (eff > 2) {
        eff = 2;
        if (loggedRequested != requested) {
            loggedRequested = requested;
            printf("[SHADOW] %dx requested -- 2x is the detail ceiling of the "
                   "EFB-bounded shadow pass (no offscreen shadow pass in this "
                   "build); effective 2x\n", requested);
            fflush(stdout);
        }
    }
#else
    /* Windowed/aurora build: >2x is REAL -- Hu3DShadowExec renders the
     * pass into a dedicated offscreen target via mp6_shadow_offscreen_
     * begin()/end() below (aurora's GX_AURORA_BEGIN/END_OFFSCREEN, where
     * viewport/scissor/copy map 1:1), so the full heap-clamped requested
     * scale flows through. 1x/2x keep their exact pre-offscreen paths. */
    (void)loggedRequested;
#endif
    return size * eff;
}

/* ------------------------------------------------------------------------
 * Per-shadow-lifetime size latch (FINDING #6a). mp6_shadow_effective_size()
 * reads the config LIVE, but Hu3DShadowExec and the hsfdraw bind run every
 * frame off Hu3DShadow->size -- so raising Shadow Quality on an ALREADY-
 * created shadow would (1) resize its offscreen target mid-run, contradicting
 * the "takes effect the next time a scene loads" contract the Mods UI states,
 * and (2) resolve a GXSetTexCopyDst of more texels than the create-time
 * HuMemDirectMalloc(HEAP_MODEL, allocSide*allocSide) buffer holds -- a
 * model-heap overflow. Latch the effective size when the buffer is created and
 * replay it for that buffer's lifetime instead of re-reading config live.
 *
 * Tiny fixed LRU table keyed by the buf pointer (only a handful of shadows are
 * ever live). The create site always runs before the first exec, and re-
 * latches when a pointer is reused across scenes. These statics are carved out
 * (this TU includes mp6_host_section.h), matching the pre-fix behavior that a
 * restored shadow re-reads live config -- so no new savestate coupling: a
 * buffer with no latch entry falls back to the live effective size. */
#define MP6_SHADOW_LATCH_SLOTS 16
static const void *s_latchBuf[MP6_SHADOW_LATCH_SLOTS];
static int s_latchSize[MP6_SHADOW_LATCH_SLOTS];
static unsigned int s_latchClock[MP6_SHADOW_LATCH_SLOTS];
static unsigned int s_latchTick;

void mp6_shadow_latch_size(const void *buf, int effSize)
{
    int i, victim;
    if (buf == NULL) {
        return;
    }
    ++s_latchTick;
    for (i = 0; i < MP6_SHADOW_LATCH_SLOTS; ++i) {
        if (s_latchBuf[i] == buf) { /* re-latch a reused buffer */
            s_latchSize[i] = effSize;
            s_latchClock[i] = s_latchTick;
            return;
        }
    }
    victim = 0;
    for (i = 0; i < MP6_SHADOW_LATCH_SLOTS; ++i) {
        if (s_latchBuf[i] == NULL) { /* prefer an empty slot */
            victim = i;
            break;
        }
        if (s_latchClock[i] < s_latchClock[victim]) { /* else evict LRU */
            victim = i;
        }
    }
    s_latchBuf[victim] = buf;
    s_latchSize[victim] = effSize;
    s_latchClock[victim] = s_latchTick;
}

int mp6_shadow_latched_size(const void *buf, int nativeSize)
{
    int i;
    for (i = 0; i < MP6_SHADOW_LATCH_SLOTS; ++i) {
        if (s_latchBuf[i] == buf) {
            s_latchClock[i] = ++s_latchTick;
            return s_latchSize[i];
        }
    }
    return mp6_shadow_effective_size(nativeSize); /* not latched: live fallback */
}

#ifdef MP6_HEADLESS_BUILD
/* No renderer: the offscreen bracket is unreachable (scale is fixed 1),
 * these exist so the shared hsfman.c patch links -- see the header. */
void mp6_shadow_offscreen_begin(int sidePx) { (void)sidePx; }
void mp6_shadow_offscreen_end(void) {}
void mp6_shadow_offscreen_scissor(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
}
#else
/* Aurora extension entry points (external_refs/repos/aurora,
 * include/dolphin/gx/GXAurora.h + aurora-patches/0014): declared locally
 * instead of including the aurora header because this TU compiles in BOTH
 * build modes with the decomp's own dolphin headers, not aurora's --
 * same C-linkage seam as mp6_launcher_cfg_shadow_quality() below.
 * GXSetScissorRender is base aurora (GX_AURORA_LOAD_SCISSOR_RENDER, already
 * in the vendored tree -- no patch), used to dodge the 11-bit SU_SCIS
 * register overflow the header comment on mp6_shadow_offscreen_scissor
 * documents. */
extern void GXCreateFrameBuffer(unsigned int width, unsigned int height);
extern void GXRestoreFrameBuffer(void);
extern void GXSetTexCopyMipGen(unsigned int enable);
extern void GXSetScissorRender(unsigned int left, unsigned int top,
                               unsigned int wd, unsigned int ht);

void mp6_shadow_offscreen_scissor(int x, int y, int w, int h)
{
    GXSetScissorRender((unsigned int)x, (unsigned int)y,
                       (unsigned int)w, (unsigned int)h);
}

void mp6_shadow_offscreen_begin(int sidePx)
{
    GXCreateFrameBuffer((unsigned int)sidePx, (unsigned int)sidePx);
    GXSetTexCopyMipGen(1); /* the resolve inside this bracket carries a
                            * full mip chain (trilinear-minified receiver
                            * sampling); cleared again in end() below */
}

void mp6_shadow_offscreen_end(void)
{
    GXSetTexCopyMipGen(0);
    GXRestoreFrameBuffer();
}
#endif

#ifndef MP6_HEADLESS_BUILD
/* Aurora/windowed build only: launcher_core.cpp (Aurora-only TU, see
 * tools/build.py's PLATFORM_AURORA_ONLY) owns the actual mp6_config.json-
 * backed value -- same cross-TU C-linkage pattern main_native.c already
 * uses for mp6_launcher_cfg_widescreen()/mp6_launcher_cfg_aspect_locked(). */
extern int mp6_launcher_cfg_shadow_quality(void);
#endif

int mp6_shadow_quality_scale(int baseSize)
{
#ifdef MP6_HEADLESS_BUILD
    /* No launcher/config exists in this build at all -- fixed native,
     * matching mp6_widescreen.h's shims_manual.c headless precedent.
     * Every automated headless gate (docs/TESTING.md) is unaffected by
     * this feature by construction. */
    (void)baseSize;
    return 1;
#else
    /* MP6_SHADOW_QUALITY env lever: docs/TESTING.md's "automation contract
     * (sacred)" means any run driven by MP6_AUTO_START_TICKS/--input-script/
     * a numeric tick-budget argv is automation mode, which by design NEVER
     * reads mp6_config.json (g_launcherMode stays 0, so
     * mp6_launcher_cfg_shadow_quality() would always read back 1/native) --
     * so the Mods-tab config alone is unreachable from any scripted gate or
     * screenshot drive. Same shape as MP6_WIDESCREEN (aurora_bridge.c's
     * mp6_widescreen_enabled()) and MP6_TICK_HZ (docs/TESTING.md: "it
     * wins"): an explicit env lever that works in EITHER mode, for
     * verification, layered on top of the real interactive/config path
     * rather than replacing it. Invalid/unset falls through to config. */
    const char *envSq = getenv("MP6_SHADOW_QUALITY");
    int requested;
    int scale;

    if (envSq != NULL && envSq[0] != '\0') {
        int v = atoi(envSq);
        requested = (v == 1 || v == 2 || v == 4 || v == 8 || v == 16) ? v : 1;
    } else {
        requested = mp6_launcher_cfg_shadow_quality();
    }

    /* No artificial detail ceiling: 8x/16x are real, buffer-dump verified
     * full-scene maps (build/shadowdump_8x.png ~14% coverage across a
     * 73%x57% bbox on the 1536px map; 16x the same composition on 3072px).
     *
     * History: commits 3f179cd/2d599c4 clamped the ceiling to 8x then 4x
     * because 8x's offscreen map dumped as a ~0.4% corner sliver and 16x as
     * uniform 0/0. That was NOT an offscreen-bracket-vs-size problem: the
     * root cause was the caster pass's GXSetScissor. It packs its rect into
     * the GameCube's 11-bit SU_SCIS register (max 2047, +342 guard band),
     * and the offscreen pass scaled the scissor to (size*scale*2) units --
     * fine at 4x (max coord 1869), but 8x's 3397 truncated to a ~992px
     * corner and 16x's 6453 wrapped below its own top edge into an empty
     * rect. Setting the offscreen scissor through aurora's render-pixel
     * GXSetScissorRender instead (mp6_shadow_offscreen_scissor, see the
     * header) sidesteps the packed register entirely, so the full offscreen
     * box now survives at every scale. The heap-fit step-down below is the
     * only remaining clamp -- a map that doesn't fit HEAP_MODEL still steps
     * down (logged), exactly as before. */
    if (requested <= 1 || baseSize <= 0) {
        return 1; /* native/off: zero heap queries, zero [SHADOW] log noise */
    }

    /* requested is one of {2,4,8,16} here -- both sources above
     * (launcher_core.cpp's config parser and the env lever) already
     * tolerant-clamp anything else to 1, caught by the <=1 return above.
     * Step down until the resulting baseSize*scale square fits inside
     * HEAP_MODEL's current largest free block -- the same pre-flight
     * check game/audio.c's own msmSysRegularProc already makes before a
     * HuMemDirectMalloc(HEAP_MODEL, ...) whose size isn't a fixed compile-
     * time constant. 1x (requested's own floor) is never queried: it is
     * the byte-identical native path and must stay exactly as
     * unprotected/unchanged as the pristine decomp's own code. */
    for (scale = requested; scale > 1; scale /= 2) {
        s32 side = (s32)baseSize * (s32)scale;
        s32 need = side * side;
        if (HuMemMaxMemorySizeGet(HuMemHeapPtrGet(HEAP_MODEL)) > need) {
            return scale;
        }
        printf("[SHADOW] %dx (%dpx square, %d bytes) exceeds HEAP_MODEL's largest "
               "free block -- clamping to %dx\n",
            scale, side, need, scale / 2);
        fflush(stdout);
    }
    return 1;
#endif
}
