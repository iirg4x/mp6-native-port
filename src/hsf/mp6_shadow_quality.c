/* MP6 native port -- Mods-page Shadow Quality: origin-site scale helper.
 * See include/mp6_shadow_quality.h for the full contract and the
 * citation of every consumer this feeds.
 *
 * Lives in src/hsf/ (alongside mp6_freecam.c and
 * mp6_widescreen_extrude.c) because it needs game/memory.h (HEAPID/
 * HuMemHeapPtrGet/HuMemMaxMemorySizeGet) -- COMMON_FLAGS' decomp -I's make
 * that available in BOTH build modes, so this one TU (tools/build.py's
 * PLATFORM_SOURCES_COMMON) covers both, split internally by #ifdef
 * MP6_HEADLESS_BUILD exactly like src/null/shims_manual.c already
 * does for mp6_widescreen_enabled()/mp6_widescreen_scale_factor().
 *
 * SAVESTATE OWNERSHIP: the size-latch table below is deterministic game
 * state tied to the shadow allocations captured in HEAP_MODEL, so this TU is
 * deliberately NOT put in mp6_host_section.h. Restoring the heap and the
 * table together preserves the allocation's exact create-time quality. The
 * clamp-log latch riding along is harmless diagnostic state.
 */
#include "game/memory.h" /* HEAPID, HuMemHeapPtrGet, HuMemMaxMemorySizeGet */
#include "game/hu3d.h"
#include "mp6_boot.h" /* mp6_heap_block_data_size: restored allocation fingerprint */
#include "mp6_shadow_quality.h"
#include "mp6_enhancements.h" /* mp6_enh_shadow_quality -- the switch's one front door */

#include <stdio.h>
#include <stdlib.h>

/* One [SHADOW] line per distinct clamp in the current game timeline. */
static int loggedRequested;

/* Called between frames. Keep the authored size/frustum and replace only
 * the backing texels. Allocate before retiring the old map so a low-memory
 * failure cannot discard a working shadow or strand a renderer reference. */
void mp6_shadow_apply_quality(void)
{
#ifndef MP6_HEADLESS_BUILD
    int i, active = 0, changed = 0;
    for (i = 0; i < HU3D_CAM_MAX; ++i) {
        HU3D_SHADOW *shadow = &Hu3DShadowBuf[i];
        int side, bytes;
        void *replacement;
        if (!(Hu3DShadowCamBit & (1 << i)) || !shadow->buf || !shadow->size) continue;
        ++active;
        side = mp6_shadow_effective_size(shadow->size);
        bytes = side * side;
        if (mp6_shadow_latched_size(shadow->buf, shadow->size) == side) continue;
        if (HuMemMaxMemorySizeGet(HuMemHeapPtrGet(HEAP_MODEL)) <= bytes) continue;
        replacement = HuMemDirectMalloc(HEAP_MODEL, bytes);
        if (!replacement) continue;
        mp6_shadow_release_buffer(shadow->buf);
        HuMemDirectFree(shadow->buf);
        shadow->buf = replacement;
        mp6_shadow_latch_size(shadow->buf, side);
        ++changed;
        printf("[SHADOW] live quality: camera %d, %dx%d texels\n", i, side, side);
    }
    printf("[SHADOW] quality applied: requested=%dx, active=%d, replaced=%d\n",
           mp6_enh_shadow_quality(), active, changed);
    fflush(stdout);
#endif
}

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

/* Effective size belongs to a backing allocation, not just the current
 * preference. Live changes replace and re-latch the buffer between frames;
 * exec/replay must never copy more texels than that allocation can hold.
 *
 * Tiny fixed LRU table keyed by the buf pointer (only a handful of shadows are
 * ever live). The create site always runs before the first exec, and re-
 * latches when a pointer is reused across scenes. The table lives in ordinary
 * writable image state and is captured/restored with the game heap. Capacity
 * is retained as an additional guard for pointer reuse and old savestates: if
 * an entry and its allocation disagree, reconstruct from the allocation rather
 * than reusing a stale size or consulting live config. */
#define MP6_SHADOW_LATCH_SLOTS 16
static const void *s_latchBuf[MP6_SHADOW_LATCH_SLOTS];
static int s_latchSize[MP6_SHADOW_LATCH_SLOTS];
static uint32_t s_latchCapacity[MP6_SHADOW_LATCH_SLOTS];
static unsigned int s_latchClock[MP6_SHADOW_LATCH_SLOTS];
static unsigned int s_latchTick;

/* Recover the create-time effective side from the direct-malloc block that
 * savestates already capture. HuMemDirectMalloc rounds a requested square up
 * to allocator alignment; testing the supported power-of-two scales from
 * largest to smallest therefore identifies the exact allocation without
 * adding host-only data to the savestate format. */
static int mp6_shadow_size_from_buffer(const void *buf, int nativeSize)
{
    uint32_t capacity = mp6_heap_block_data_size(buf);
    int scale;

    if (nativeSize <= 0 || capacity == 0) {
        return nativeSize;
    }
    for (scale = 16; scale >= 1; scale /= 2) {
        uint64_t side = (uint64_t)(unsigned int)nativeSize * (uint64_t)(unsigned int)scale;
        if (side * side <= (uint64_t)capacity) {
            return (int)side;
        }
    }
    return nativeSize;
}

void mp6_shadow_latch_size(const void *buf, int effSize)
{
    int i, victim;
    uint32_t capacity;
    if (buf == NULL) {
        return;
    }
    capacity = mp6_heap_block_data_size(buf);
    ++s_latchTick;
    for (i = 0; i < MP6_SHADOW_LATCH_SLOTS; ++i) {
        if (s_latchBuf[i] == buf) { /* re-latch a reused buffer */
            s_latchSize[i] = effSize;
            s_latchCapacity[i] = capacity;
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
    s_latchCapacity[victim] = capacity;
    s_latchClock[victim] = s_latchTick;
}

int mp6_shadow_latched_size(const void *buf, int nativeSize)
{
    int i;
    uint32_t capacity;
    if (buf == NULL) {
        return nativeSize;
    }
    capacity = mp6_heap_block_data_size(buf);
    for (i = 0; i < MP6_SHADOW_LATCH_SLOTS; ++i) {
        if (s_latchBuf[i] == buf) {
            if (capacity != 0 && capacity == s_latchCapacity[i]) {
                s_latchClock[i] = ++s_latchTick;
                return s_latchSize[i];
            }
            /* Same address, different allocation. Do not let a stale/evicted
             * latch size this copy. */
            s_latchBuf[i] = NULL;
            s_latchSize[i] = 0;
            s_latchCapacity[i] = 0;
            s_latchClock[i] = 0;
            break;
        }
    }
    /* A miss can occur after LRU eviction or pointer reuse. Reconstruct from
     * the allocation, then cache it; consulting live config here can claim
     * more texels than the buffer owns. */
    {
        int restoredSize = mp6_shadow_size_from_buffer(buf, nativeSize);
        mp6_shadow_latch_size(buf, restoredSize);
        return restoredSize;
    }
}

static void mp6_shadow_forget_latch(const void *buf)
{
    int i;
    if (buf == NULL) {
        return;
    }
    for (i = 0; i < MP6_SHADOW_LATCH_SLOTS; ++i) {
        if (s_latchBuf[i] == buf) {
            s_latchBuf[i] = NULL;
            s_latchSize[i] = 0;
            s_latchCapacity[i] = 0;
            s_latchClock[i] = 0;
        }
    }
}

#ifdef MP6_HEADLESS_BUILD
/* No renderer: the offscreen bracket is unreachable (scale is fixed 1),
 * these exist so the shared hsfman.c patch links -- see the header. */
void mp6_shadow_offscreen_begin(int sidePx) { (void)sidePx; }
void mp6_shadow_offscreen_end(void) {}
void mp6_shadow_release_buffer(const void *buf) { mp6_shadow_forget_latch(buf); }
void mp6_shadow_offscreen_scissor(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
}
#else
/* Aurora extension entry points (external_refs/repos/aurora,
 * include/dolphin/gx/GXAurora.h + aurora-patches/0014): declared locally
 * instead of including the aurora header because this TU compiles in BOTH
 * build modes with the decomp's own dolphin headers, not aurora's -- the
 * same plain C-linkage seam trick the rest of this file uses.
 * GXSetScissorRender is base aurora (GX_AURORA_LOAD_SCISSOR_RENDER, already
 * in the vendored tree -- no patch), used to dodge the 11-bit SU_SCIS
 * register overflow the header comment on mp6_shadow_offscreen_scissor
 * documents. */
extern void GXCreateFrameBuffer(unsigned int width, unsigned int height);
extern void GXRestoreFrameBuffer(void);
extern void GXSetTexCopyMipGen(unsigned int enable);
extern void GXSetScissorRender(unsigned int left, unsigned int top,
                               unsigned int wd, unsigned int ht);
extern void GXDestroyCopyTex(void *dest);
extern void GXDestroyFrameBufferCache(void);

void mp6_shadow_release_buffer(const void *buf)
{
    if (buf == NULL) {
        return;
    }
    mp6_shadow_forget_latch(buf);
    /* FIFO-ordered renderer retirement: discard every copy geometry for this
     * CPU destination and the quality-sized offscreen color/depth target. */
    GXDestroyCopyTex((void *)buf);
    GXDestroyFrameBufferCache();
}

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

int mp6_shadow_quality_scale(int baseSize)
{
#ifdef MP6_HEADLESS_BUILD
    /* Fixed native -- and deliberately NOT a read of the enhancements seam,
     * even though that seam links here (it is in build.py's
     * PLATFORM_SOURCES_COMMON). There is no shadow renderer in this build at
     * all: mp6_shadow_offscreen_begin/end/scissor above are empty in the
     * headless half of this very file, so a scale above 1 would size a map
     * nothing ever draws into. The switch has no consumer here to wire to.
     * Every automated headless gate (docs/TESTING.md) is therefore unaffected
     * by this feature by construction, exactly as before. */
    (void)baseSize;
    return 1;
#else
    /* THE SHADOW-QUALITY DECISION, in the one place the scale is chosen.
     *
     * Two sources, in this order:
     *
     *   1. MP6_SHADOW_QUALITY -- the pre-existing per-feature lever,
     *      unchanged, and it still wins outright at its own consumption site,
     *      which is the priority docs/SETTINGS.md promises the legacy levers
     *      keep. Same shape as MP6_WIDESCREEN (aurora_bridge.c's
     *      mp6_widescreen_enabled()) and MP6_TICK_HZ (docs/TESTING.md: "it
     *      wins"). An off-ladder value degrades to 1/retail rather than to a
     *      guess.
     *
     *   2. the ENHANCEMENTS SEAM (include/mp6_enhancements.h), which is
     *      the single front door for this switch and resolves, in its own
     *      documented order, MP6_ENH_SHADOW_QUALITY -> MP6_ENH_PRESET -> the
     *      value the launcher published from mp6_config.json -> RETAIL. It
     *      ladder-clamps too, so both sources agree on what "invalid" means.
     *
     * Step 2 replaces a direct mp6_launcher_cfg_shadow_quality() read, which
     * was the config and ONLY the config. That is why the Enhancements row's
     * own claim -- that MP6_ENH_SHADOW_QUALITY and MP6_ENH_PRESET override
     * video.shadow_quality, which is why the row greys itself out while
     * either is set (settings.cpp) -- was not true of the engine: both levers
     * resolved correctly inside the seam and were then thrown away here.
     *
     * The automation contract holds BY CONSTRUCTION rather than by a
     * `g_launcherMode ?` branch: automation mode never calls
     * mp6_enh_set_values(), so the seam's store is still at its retail
     * initializer and this answers 1 -- the same fixed 1 the old accessor
     * hard-coded off the launcher path. */
    const char *envSq = getenv("MP6_SHADOW_QUALITY");
    int requested;
    int scale;

    if (envSq != NULL && envSq[0] != '\0') {
        int v = atoi(envSq);
        requested = (v == 1 || v == 2 || v == 4 || v == 8 || v == 16) ? v : 1;
    } else {
        requested = mp6_enh_shadow_quality();
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
