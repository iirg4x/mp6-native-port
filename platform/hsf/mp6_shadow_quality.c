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
    /* The shadow pass is EFB-unit-bounded (the pristine 240 cap IS the
     * 480px EFB box over the 2x supersample) and aurora already renders +
     * resolves it at window-scaled physical resolution -- scale_copy_dst()
     * multiplies the dst texel count by window/EFB on its own. So the only
     * information-bearing port-side raise is the copy DESTINATION's
     * EFB-unit texel count, and its ceiling is the src region's own size:
     * 2x (dst == the 2x-supersampled src, pixel for pixel). Beyond that is
     * pure upsampling -- deeper gains need the offscreen shadow pass +
     * mip-chain work in the renderer, not a bigger number here. */
    if (eff > 2) {
        eff = 2;
        if (loggedRequested != requested) {
            loggedRequested = requested;
            printf("[SHADOW] %dx requested -- 2x is the detail ceiling of the "
                   "EFB-bounded shadow pass (dst texels = its supersampled "
                   "src); effective 2x until the offscreen shadow pass "
                   "lands\n", requested);
            fflush(stdout);
        }
    }
    return size * eff;
}

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

    if (requested <= 1 || baseSize <= 0) {
        return 1; /* native/off: zero heap queries, zero [SHADOW] log noise */
    }

    /* requested is one of {2,4,8,16} here -- both sources above
     * (launcher_core.cpp's config parser and the env-lever clamp right
     * above) already tolerant-clamp anything else to 1, caught by the
     * <=1 return above. Step down until the resulting baseSize*scale
     * square fits inside
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
