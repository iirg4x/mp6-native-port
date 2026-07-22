/* MP6 native port -- debug lever: MP6_SHADOW_DUMP. See shim/include/
 * mp6_shadow_dump.h for the full contract and the citation of what this
 * proves and why it's needed (aurora's shadow-copy resolve is GPU-only,
 * so a CPU texdump of Hu3DShadow->buf reads stale heap garbage).
 *
 * Lives in platform/gx/ (not platform/hsf/, where mp6_shadow_quality.c
 * lives -- this is debug/verification tooling, not a gameplay-affecting
 * origin-site helper) but still needs game/hu3d.h for Hu3DShadow, exactly
 * like mp6_freecam.c/mp6_shadow_quality.c do for their own decomp structs
 * -- COMMON_FLAGS' decomp -I's make that available in BOTH build modes,
 * so this one TU (tools/build.py's PLATFORM_SOURCES_COMMON) covers both,
 * split internally by #ifdef MP6_HEADLESS_BUILD.
 *
 * HOST STATICS (carved): the env-latch and dump counter below describe
 * the RUNNING process, not game state, and must not ride a savestate --
 * same category platform/gx/framescope.c's own header comment documents
 * for its arming state (tools/build.py's HOST_STATE_SECTION_SOURCES lists
 * this file for exactly that reason). */
#include "mp6_shadow_dump.h"

#include "game/hu3d.h" /* HU3D_SHADOW, extern HU3D_SHADOW *Hu3DShadow; */

#include <stdio.h>
#include <stdlib.h>

#include "mp6_host_section.h"

#if defined(MP6_HEADLESS_BUILD) || defined(__ANDROID__)

/* No-op in two builds:
 *  - headless: no renderer, no GPU, no copy texture to read back.
 *  - Android: MP6_SHADOW_DUMP is a DESKTOP verification lever only (you
 *    inspect the dumped PNGs on a dev box, never on a phone), and the
 *    Android aurora backend archive does not carry aurora-patches/0016's
 *    aurora_gx_debug_dump_copy_png (that readback capability is desktop-
 *    only) -- referencing it here would be an undefined symbol at the
 *    Android link. Same reasoning applies to any desktop-debug lever that
 *    calls an aurora debug-patch symbol; keep them out of the Android TU.
 * See the header's "windowed (Aurora) build only" note. */
void mp6_shadow_dump_tick(void) {}

#else

extern long mp6_tick_count;

/* Aurora extension entry point (external_refs/repos/aurora,
 * aurora-patches/0016): declared locally instead of including any aurora
 * header -- same C-linkage seam mp6_shadow_quality.c's GXCreateFrameBuffer/
 * GXRestoreFrameBuffer/GXSetTexCopyMipGen already use (this TU compiles
 * with the decomp's own dolphin headers, not aurora's). Looks up the
 * resolved copy texture at `dest` (aurora::gx::g_gxState.copyTextures,
 * keyed by that exact pointer -- the same map a later GXInitTexObj bind of
 * it consults) and writes an 8-bit grayscale PNG to `path`, using aurora's
 * own libpng-backed writer. Returns nonzero (true) on success -- false if
 * `dest` has no resolved copy (yet), or the PNG write failed. */
extern int aurora_gx_debug_dump_copy_png(const void *dest, const char *path);

/* Spread dumps across the WHOLE armed run instead of clustering all of them
 * in the first few ticks after Hu3DShadow appears: the map's content at the
 * very first tick it exists (e.g. before a character/tag has settled into
 * view) is not necessarily representative, and the verification schedule
 * this lever is meant for (docs reference: file-select navigation via
 * MP6_AUTO_START_TICKS) spans thousands of ticks. One dump every
 * MP6_SHADOW_DUMP_INTERVAL ticks, capped at MP6_SHADOW_DUMP_MAX total so an
 * accidentally-long run never spams build/ with megabyte grayscale PNGs (a
 * 16x/3072-square dump is ~9.4MB uncompressed) -- 30 dumps 180 ticks (3s at
 * 60Hz) apart covers 90s of run time from Hu3DShadow's first appearance. */
#define MP6_SHADOW_DUMP_INTERVAL 180
#define MP6_SHADOW_DUMP_MAX 30

static int mp6ShadowDumpArmed = -2;   /* -2 = env not parsed yet, -1 = disabled, 1 = armed */
static int mp6ShadowDumpCount;        /* successful dumps so far, capped at MP6_SHADOW_DUMP_MAX */
static long mp6ShadowDumpLastTick = -1000000; /* far enough back that the first eligible tick always dumps */

static int mp6ShadowDumpIsArmed(void)
{
    if (mp6ShadowDumpArmed == -2) {
        const char *e = getenv("MP6_SHADOW_DUMP");
        mp6ShadowDumpArmed = (e != NULL && e[0] != '\0' && atoi(e) != 0) ? 1 : -1;
    }
    return mp6ShadowDumpArmed == 1;
}

/* Called once per tick (platform/gx/aurora_bridge.c's VIWaitForRetrace,
 * right after that tick's aurora_end_frame() -- the same hook point
 * framescope.c's mp6_fs_frame_end() already uses, guaranteeing any shadow
 * copy this tick made is already resolved). Unarmed: one cached getenv()
 * and an early return, zero further cost -- see mp6_shadow_dump_h's
 * "OFF BY DEFAULT, ZERO COST UNARMED" contract. */
void mp6_shadow_dump_tick(void)
{
    char path[64];

    if (!mp6ShadowDumpIsArmed() || mp6ShadowDumpCount >= MP6_SHADOW_DUMP_MAX) {
        return;
    }
    if (Hu3DShadow == NULL || Hu3DShadow->buf == NULL) {
        return; /* no shadow map on this screen (yet) -- file select is the
                  * earliest screen that creates one, see mp6_shadow_dump.h */
    }
    if (mp6_tick_count - mp6ShadowDumpLastTick < MP6_SHADOW_DUMP_INTERVAL) {
        return; /* sampled recently enough -- see the interval/spread comment above */
    }
    mp6ShadowDumpLastTick = mp6_tick_count;

    snprintf(path, sizeof(path), "build/shadowdump_%ld.png", mp6_tick_count);
    if (aurora_gx_debug_dump_copy_png(Hu3DShadow->buf, path)) {
        mp6ShadowDumpCount++;
        printf("[SHADOWDUMP] tick=%ld dest=%p size=%d wrote %s (%d/%d)\n",
               mp6_tick_count, Hu3DShadow->buf, (int)Hu3DShadow->size, path,
               mp6ShadowDumpCount, MP6_SHADOW_DUMP_MAX);
    } else {
        printf("[SHADOWDUMP] tick=%ld dest=%p: no resolved copy yet (or PNG write failed)\n",
               mp6_tick_count, Hu3DShadow->buf);
    }
    fflush(stdout);
}

#endif /* MP6_HEADLESS_BUILD */
