/* MP6 native port -- Enhancements "Expanded heaps": the process-wide scale
 * latch. The arithmetic lives in shim/include/mp6_heap_scale.h (pure, and
 * driven directly by tools/heap_scale_selftest.c); this file is only the
 * stateful half.
 *
 * WHY A LATCH RATHER THAN A LIVE READ. The setting is consumed at two
 * moments that are thousands of instructions apart: platform/os/arena.c
 * reserves the game arena during mp6_arena_init(), and
 * platform/os/malloc_direct.c sizes the four fixed HuMem heaps inside
 * HuMemInitAll() much later, once GameMain has reached HuSysInit's InitMem.
 * A value that could differ between those two reads would hand the second
 * one an arena that cannot hold the table it just computed -- which is not a
 * subtle failure (`HuMem> Failed OSAlloc`, an all-NULL HeapTbl, and a crash
 * on the first allocation), but it is an avoidable one. Reading once and
 * replaying the answer makes the two structurally unable to disagree.
 *
 * SAVESTATE OWNERSHIP: host-owned, hence the carve-out include below. The
 * latch describes the reservation THIS invocation made, exactly like
 * savestate.c's own statics and like the tick budget mp6_savestate_restore
 * re-latches across its commit loop. If it were ordinary restorable image
 * state, loading a state captured at scale 1 into a process running at
 * scale 4 would install `1` over the latch -- and mp6_arena_size(), which
 * feeds every subsequent capture's recorded arena size AND the region
 * table, would then report 256 MB for a 550 MB reservation. A memory-image
 * format that misstates its own extents is how a restore silently writes
 * the wrong bytes; the carve-out is what keeps the reported size equal to
 * the reservation that was actually made. Nothing needs the CAPTURED
 * scale: the captured heap capacities travel in HeapSizeTbl (ordinary
 * restorable .data) and the captured arena extent travels in the file
 * header, so the restored world stays self-consistent on its own terms.
 */
#include "mp6_heap_scale.h"
#include "mp6_enhancements.h"

#include <stdio.h>

#include "mp6_host_section.h"

static unsigned int g_scale;   /* 0 = not yet resolved */

unsigned int mp6_heap_scale_active(void)
{
    if (g_scale == 0u) {
        g_scale = mp6_heap_scale_clamp((long)mp6_enh_heap_scale());
        if (g_scale != MP6_HEAP_SCALE_RETAIL) {
            /* Announced only when the enhancement is ON. Retail stays
             * silent so the headless boot log the log-diff gate compares
             * (docs/TESTING.md gate 2) is byte-unchanged with the
             * enhancement off. */
            printf("[BOOT] heap scale: x%u -- HuMem fixed capacities %u MB, arena %u MB "
                   "(retail: x1, %u MB, %u MB)\n",
                   g_scale,
                   (MP6_HEAP_RETAIL_FIXED_TOTAL * g_scale) / (1024u * 1024u),
                   mp6_heap_arena_bytes_for_scale(g_scale) / (1024u * 1024u),
                   MP6_HEAP_RETAIL_FIXED_TOTAL / (1024u * 1024u),
                   MP6_ARENA_RETAIL_BYTES / (1024u * 1024u));
            fflush(stdout);
        }
    }
    return g_scale;
}

unsigned int mp6_heap_arena_bytes(void)
{
    return mp6_heap_arena_bytes_for_scale(mp6_heap_scale_active());
}
