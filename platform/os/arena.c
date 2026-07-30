/* MP6 native port -- low-4GB game arena + OS heap/alloc family.
 *
 * The game casts pointers to u32 in several places (HuMem file tags,
 * AMEM_PTR, OSModuleHeader.prolog/epilog and jmp_buf.lr -- the last two
 * store CODE addresses truncated to u32, not just heap data). Rather than
 * chase every individual cast, the whole image is linked at a fixed low
 * base with ASLR off (tools/build.py: --image-base, --no-dynamicbase) AND
 * the game's heap arena is reserved at an explicit low base here, so
 * every category of u32<->pointer round-trip stays valid.
 *
 * The allocator behind OSAllocFromHeap is a plain bump allocator with no
 * free-list: HuMem (game/memory.c, decomp code) sub-manages real
 * allocation on top of a handful of big OS-level chunks handed out once at
 * boot (game/malloc.c's HuMemInitAll), so nothing in this slice needs the
 * OS-level heap to support real reuse.
 */
#include "dolphin.h"
#include "mp6_shim_log.h"
#include "host.h" /* mp6_host_arena_reserve */
#include "mp6_alloc_size.h"
#include "mp6_heap_scale.h" /* the reservation is a function of the heap scale */
#include "mp6_diag_probe.h" /* pull-side arena snapshot -- see the block at the end */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Sizing: platform/os/malloc_direct.c's HeapSizeTbl requests ~98MB across
 * its 4 fixed HuMem heaps alone (HEAP_MODEL is 88MB because this port's
 * native HSF deserializer -- unlike the original's in-place loader --
 * allocates a fresh copy of every array/struct instead of reusing the
 * file buffer, so it needs room for every model a real play session loads
 * at once); 256MB covers that plus the arena's other tenants (the game's
 * own workloads, the OS-level bump allocator's remaining HEAP_SPACE heap)
 * with a comfortable margin, while staying trivially "low" for the
 * u32<->pointer round-trips this file's header describes.
 *
 * That 256MB is now the SCALE-1 case of one formula rather than a literal,
 * because the Enhancements "Expanded heaps" toggle multiplies exactly the
 * four fixed HuMem capacities and the reservation has to grow with them --
 * scaling the table alone would only produce `HuMem> Failed OSAlloc` at
 * boot. mp6_heap_arena_bytes() (shim/include/mp6_heap_scale.h) is
 * `98MB * scale + 158MB of constant headroom`, which returns exactly
 * 0x10000000 at scale 1, so with the enhancement off the reservation, the
 * [BOOT] banner below, and every savestate's recorded arena size are
 * unchanged. The scale is latched process-wide (platform/os/heap_scale.c),
 * so the value HuMemInitAll later sizes its table with is structurally the
 * same one this reservation was made for. */

static u8 *g_arenaBase;
static u8 *g_arenaEnd;
static u8 *g_arenaLo;
static u8 *g_arenaHi;

#define MP6_MAX_HEAPS 16
typedef struct { u8 *cur; u8 *end; int valid; } BumpHeap;
static BumpHeap g_heaps[MP6_MAX_HEAPS];
static int g_heapCount;

volatile OSHeapHandle __OSCurrHeap = -1;

void *mp6_arena_base(void) { return g_arenaBase; }

/* The size actually reserved, not a compile-time constant -- savestate
 * capture records this as the arena region's extent and a restore compares
 * it against the loading process's own (platform/os/savestate.c). It reads
 * through the host-owned latch rather than a static of this TU on purpose:
 * this file's statics ARE captured and restored, so a local copy would let a
 * state captured at one scale overwrite the loading process's true extent
 * with the capturing process's. See heap_scale.c's own header. */
u32 mp6_arena_size(void) { return mp6_heap_arena_bytes(); }

void mp6_arena_init(void)
{
    /* The candidate-base loop (0x80000000 first -- game/memory.c's
     * BLOCK_CHECK_BROKEN high-bit check) plus the verified OS-picked
     * fallback live in mp6_host_arena_reserve (the host backends). This TU
     * keeps the FATAL policy and independently verifies the ABI invariant. */
    u32 arenaSize = mp6_arena_size();
    /* No preferred base: the candidate ladder's own first entry (0x80000000)
     * is what this reservation wants, and it is the FIRST reservation the
     * process makes, so first-fit is deterministic here. The coroutine pool
     * -- which comes second and would otherwise be displaced by this one's
     * size -- is the caller that names a base. */
    void *got = mp6_host_arena_reserve(0, arenaSize);

    if (!got || !mp6_host_range_below_4gb(got, arenaSize)) {
        fprintf(stderr, "[FATAL] mp6_arena_init: could not reserve an entirely low-4GB "
                        "game arena of %u bytes\n",
                (unsigned)arenaSize);
        exit(1);
    }

    g_arenaBase = (u8 *)got;
    g_arenaEnd = g_arenaBase + arenaSize;
    g_arenaLo = g_arenaBase;
    g_arenaHi = g_arenaEnd;

    printf("[BOOT] arena: base=%p size=%uMB (%s 4GB)\n",
           (void *)g_arenaBase, arenaSize / (1024 * 1024),
           mp6_host_range_below_4gb(g_arenaBase, arenaSize) ? "below" : "ABOVE");
    fflush(stdout);
}

/* ---- OSGetArenaLo/Hi/SetArenaLo/Hi -------------------------------------
 * Plain mutable bounds the game walks itself (e.g. game/init.c's InitMem
 * rounds arenaLo up past the two framebuffers it carves out by hand before
 * ever calling OSCreateHeap). */
void *OSGetArenaLo(void) { MP6_LOG_ONCE("OS", "OSGetArenaLo"); return g_arenaLo; }
void *OSGetArenaHi(void) { MP6_LOG_ONCE("OS", "OSGetArenaHi"); return g_arenaHi; }
void OSSetArenaLo(void *newLo)
{
    MP6_LOG_ONCE("OS", "OSSetArenaLo");
    g_arenaLo = (u8 *)newLo;
}
void OSSetArenaHi(void *newHi)
{
    MP6_LOG_ONCE("OS", "OSSetArenaHi");
    g_arenaHi = (u8 *)newHi;
}

/* [HEAPTRACE]: every OS-level heap call is logged unconditionally (not
 * just the first per symbol, unlike MP6_LOG_ONCE elsewhere in this file),
 * with the heap index and the actual pointer arithmetic involved.
 * OS-level heap calls are inherently rare (a handful total, all during
 * boot, never in the per-frame loop), so unconditional logging costs
 * nothing and gives direct evidence of heap layout on every run. */
void *OSInitAlloc(void *arenaStart, void *arenaEnd, int maxHeaps)
{
    printf("[HEAPTRACE] OSInitAlloc(start=%p, end=%p, maxHeaps=%d) -> %p (no-op passthrough)\n",
           arenaStart, arenaEnd, maxHeaps, arenaStart);
    fflush(stdout);
    (void)arenaEnd;
    (void)maxHeaps;
    /* Real SDK reserves a small heap-descriptor table at the start of the
     * given range and returns the adjusted start; our heap bookkeeping is a
     * static C array instead, so nothing needs reserving. */
    return arenaStart;
}

OSHeapHandle OSCreateHeap(void *start, void *end)
{
    if (g_heapCount >= MP6_MAX_HEAPS) {
        printf("[HEAPTRACE] OSCreateHeap(start=%p, end=%p) -> FAILED (MP6_MAX_HEAPS exceeded)\n", start, end);
        fflush(stdout);
        return -1;
    }
    {
        int h = g_heapCount++;
        g_heaps[h].cur = (u8 *)start;
        g_heaps[h].end = (u8 *)end;
        g_heaps[h].valid = 1;
        if (__OSCurrHeap == (OSHeapHandle)-1) __OSCurrHeap = h;
        printf("[HEAPTRACE] OSCreateHeap(start=%p, end=%p) -> heap=%d (size=%llu bytes) g_heapCount now %d\n",
               start, end, h, (unsigned long long)((u8 *)end - (u8 *)start), g_heapCount);
        fflush(stdout);
        return h;
    }
}

void OSDestroyHeap(OSHeapHandle heap)
{
    printf("[HEAPTRACE] OSDestroyHeap(heap=%d)\n", heap);
    fflush(stdout);
    if (heap >= 0 && heap < MP6_MAX_HEAPS) g_heaps[heap].valid = 0;
}

void OSAddToHeap(OSHeapHandle heap, void *start, void *end)
{
    printf("[HEAPTRACE] OSAddToHeap(heap=%d, start=%p, end=%p)\n", heap, start, end);
    fflush(stdout);
    /* Bump-allocator simplification: treat the added range as extending
     * `end` if it's contiguous, else ignore -- not exercised in this slice. */
    (void)start;
    if (heap >= 0 && heap < MP6_MAX_HEAPS && g_heaps[heap].valid) {
        if ((u8 *)end > g_heaps[heap].end) g_heaps[heap].end = (u8 *)end;
    }
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap)
{
    OSHeapHandle prev = __OSCurrHeap;
    __OSCurrHeap = heap;
    printf("[HEAPTRACE] OSSetCurrentHeap(heap=%d) -> prev=%d\n", heap, prev);
    fflush(stdout);
    return prev;
}

void *OSAllocFromHeap(OSHeapHandle heap, u32 size)
{
    if (heap < 0 || heap >= MP6_MAX_HEAPS || !g_heaps[heap].valid) {
        printf("[HEAPTRACE] OSAllocFromHeap(heap=%d, size=%u) -> NULL (invalid/inactive heap)\n", heap, size);
        fflush(stdout);
        return NULL;
    }
    {
        u32 rounded;
        BumpHeap *h = &g_heaps[heap];
        u8 *curBefore = h->cur;
        size_t remaining;
        if (!mp6_os_heap_checked_request(size, &rounded)) {
            printf("[HEAPTRACE] OSAllocFromHeap(heap=%d, size=%u) -> NULL "
                   "(zero/rounding overflow)\n", heap, size);
            fflush(stdout);
            return NULL;
        }
        if (h->cur == NULL || h->end == NULL || h->cur > h->end) {
            printf("[HEAPTRACE] OSAllocFromHeap(heap=%d, size=%u/rounded=%u) -> NULL "
                   "(invalid heap bounds: cur=%p end=%p)\n",
                   heap, size, rounded, (void *)h->cur, (void *)h->end);
            fflush(stdout);
            return NULL;
        }
        remaining = (size_t)(h->end - h->cur);
        if ((size_t)rounded > remaining) {
            printf("[HEAPTRACE] OSAllocFromHeap(heap=%d, size=%u/rounded=%u) -> NULL (exhausted: cur=%p end=%p)\n",
                   heap, size, rounded, (void *)h->cur, (void *)h->end);
            fflush(stdout);
            return NULL;
        }
        {
            void *p = h->cur;
            h->cur += rounded;
            printf("[HEAPTRACE] OSAllocFromHeap(heap=%d, size=%u/rounded=%u) -> %p (heap.cur %p -> %p, end=%p)\n",
                   heap, size, rounded, p, (void *)curBefore, (void *)h->cur, (void *)h->end);
            fflush(stdout);
            return p;
        }
    }
}

void OSFreeToHeap(OSHeapHandle heap, void *ptr)
{
    printf("[HEAPTRACE] OSFreeToHeap(heap=%d, ptr=%p) -- bump allocator, no reuse\n", heap, ptr);
    fflush(stdout);
    /* Bump allocator: no reuse. See file header comment. */
    (void)heap;
    (void)ptr;
}

void *OSAllocFixed(void **rstart, void **rend)
{
    MP6_LOG_ONCE("OS", "OSAllocFixed");
    (void)rstart;
    (void)rend;
    /* Only reachable via game/init.c's LoadMemInfo dev-hardware path
     * (OS_CONSOLE_DEVHW1), which our OSGetConsoleType shim never reports. */
    return NULL;
}

long OSCheckHeap(OSHeapHandle heap)
{
    MP6_LOG_ONCE("OS", "OSCheckHeap");
    if (heap < 0 || heap >= MP6_MAX_HEAPS || !g_heaps[heap].valid) return 0;
    return (long)(g_heaps[heap].end - g_heaps[heap].cur);
}

void OSDumpHeap(OSHeapHandle heap)
{
    MP6_LOG_ONCE("OS", "OSDumpHeap");
    if (heap >= 0 && heap < MP6_MAX_HEAPS && g_heaps[heap].valid) {
        printf("[BOOT] OSDumpHeap(%d): %ld bytes free\n", heap, OSCheckHeap(heap));
    }
}

/* Pull-side arena snapshot (shim/include/mp6_diag_probe.h). The [HEAPTRACE]
 * lines above report every heap EVENT; this reports the resulting STATE, which
 * is the thing a live memory panel needs and which no existing surface can
 * answer between two events. Read-only over the same table OSCheckHeap uses. */
int mp6_diag_arena_count(void)
{
    return g_heapCount;
}

int mp6_diag_arena_current(void)
{
    return (int)__OSCurrHeap;
}

int mp6_diag_arena(int index, Mp6DiagArenaHeap *out)
{
    if (out == NULL || index < 0 || index >= MP6_MAX_HEAPS) return 0;
    out->valid = g_heaps[index].valid;
    out->cur = g_heaps[index].cur;
    out->end = g_heaps[index].end;
    out->remaining = (g_heaps[index].valid && g_heaps[index].end > g_heaps[index].cur)
                         ? (unsigned long long)(g_heaps[index].end - g_heaps[index].cur)
                         : 0ull;
    return 1;
}
