/* MP6 native port -- HuMemDirect* family, ported natively.
 *
 * Replaces src/game/malloc.c (skipped from the decomp unit set): every
 * one of its 7 functions uses an MWCC-only, UNGUARDED (no #ifdef
 * __MWERKS__) `asm { mflr retaddr }` block to capture its own caller's
 * return address, purely as an opaque tag forwarded into
 * HuMemMemoryAlloc/Free/etc. (game/memory.c, unmodified decomp code --
 * NOT skipped) for allocation-tracking/debug-dump purposes.
 * `__builtin_return_address(0)` is the portable, semantically equivalent
 * replacement clang (and gcc) provide for exactly this: "the return
 * address of the current function's caller", i.e. the same value `mflr`
 * reads on PPC at the same point in the callee's prologue.
 *
 * HeapTbl/HeapSizeTbl are copied verbatim from the original file (plain
 * data, no MWCC-isms) so every other game file that reaches into this
 * subsystem via game/memory.h keeps working unmodified.
 *
 * Heap sizing: HEAP_HEAP/HEAP_MODEL's budgets are widened well beyond
 * their original values. The four fixed sizes totaled ~19.9MB on real
 * hardware, sized against the GameCube's own 24MB of physical RAM -- a
 * constraint this native port doesn't share. HEAP_MODEL specifically
 * (88MB, 8x the original) is what platform/hsf/hsf_load_native.c's native
 * HSF deserializer allocates every loaded model's data into: unlike the
 * original's in-place loader (which reused the SAME already-loaded file
 * buffer for nearly everything, fixing up pointers within it), this
 * port's deserializer builds a FRESH, natively-laid-out copy of every
 * array/struct (a real, necessary cost of fixing the
 * pointer-width/endianness mismatch documented in that file's header) --
 * a real increase in bytes-per-model, not a leak. HEAP_HEAP gets a
 * smaller, proportionate bump for the same reason (this port's own
 * per-model bookkeeping/side-table allocations also land there).
 * HEAP_SOUND/HEAP_DVD keep their original sizes -- the audio bridge
 * allocates modestly and DVD reads complete synchronously into
 * caller-owned buffers, neither under the same pressure.
 */
#include "game/memory.h"
#include "game/init.h"
#include "dolphin/os.h"
#include "mp6_boot.h" /* mp6_tick_count -- mp6_trace_heap_model's tick gate below */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static u32 HeapSizeTbl[HEAP_MAX] = { 0x220000*2, 0xC0000, 0xB00000*8, 0x500000, 0 };
static void *HeapTbl[HEAP_MAX];

void HuMemInitAll(void)
{
    s32 i;
    void *ptr;
    u32 free_size;
    for (i = 0; i < 4; i++) {
        ptr = OSAlloc(HeapSizeTbl[i]);
        if (ptr == NULL) {
            OSReport("HuMem> Failed OSAlloc Size:%d(left:%x)\n", HeapSizeTbl[i], OSCheckHeap(currentHeapHandle));
            return;
        }
        HeapTbl[i] = HuMemInit(ptr, HeapSizeTbl[i]);
    }
    free_size = OSCheckHeap(currentHeapHandle);
    OSReport("HuMem> left memory space %dKB(%d)\n", free_size / 1024, free_size);
    /* Boot diagnostic: confirms HEAP_HEAP's freshly-initialized block
     * really does start at HeapSizeTbl[HEAP_HEAP] -- if a later
     * allocation failure shows a smaller tracked total, something shrank
     * it after init; it was never mis-sized. */
    printf("[DIAG] HeapTbl[HEAP_HEAP]=%p initial block->size=0x%08x (HeapSizeTbl says 0x%08x)\n",
           HeapTbl[HEAP_HEAP], HeapTbl[HEAP_HEAP] ? *(u32 *)HeapTbl[HEAP_HEAP] : 0u, HeapSizeTbl[HEAP_HEAP]);
    fflush(stdout);
    ptr = OSAlloc(free_size);
    if (ptr == NULL) {
        OSReport("HuMem> Failed OSAlloc left space\n");
        return;
    }
    HeapTbl[4] = HuMemInit(ptr, free_size);
    HeapSizeTbl[4] = free_size;
}

void *HuMemInit(void *ptr, s32 size)
{
    return HuMemHeapInit(ptr, size);
}

void HuMemDCFlushAll(void)
{
    HuMemDCFlush(2);
    HuMemDCFlush(0);
}

void HuMemDCFlush(HEAPID heap)
{
    DCFlushRangeNoSync(HeapTbl[heap], HeapSizeTbl[heap]);
}

/* Zero-size allocations must be clamped to 1 byte: a latent,
 * 64-bit-pointer-widening-only bug in the decomp's OWN, unmodified
 * game/memory.c is exposed (not introduced) by this port.
 *
 * MEMORY_BLOCK (game/memory.c) is 40 bytes natively (8-byte prev/next) vs
 * 28 bytes on the original 32-bit target (4-byte prev/next), but
 * DATA_GET_BLOCK/BLOCK_GET_DATA/MEM_ALLOC_SIZE all hardcode a 32-byte
 * "header size" regardless -- so natively, MEMORY_BLOCK's `file` field
 * lands at offset 32-35, EXACTLY where BLOCK_GET_DATA says the caller's
 * data (or, when a split happens, the next block's OWN header) begins;
 * on the original 32-bit layout `file` sits at offset 24-27, safely
 * inside the assumed header with 4 real padding bytes to spare, so this
 * never manifested there. For an ordinary (non-zero) allocation this only
 * corrupts the block's own `file` tag (harmless here: nothing in this
 * slice re-reads a block's file id after the fact). But
 * HuMemMemoryAlloc2/HuMemTailMemoryAlloc2 both end with an unconditional
 * `block->file = 0;` AFTER already having split off a new leftover free
 * block when one exists -- and MEM_ALLOC_SIZE(0) == 32 (the minimum
 * possible block size, header-only), so a literal ZERO-BYTE allocation
 * request places that new leftover block's header AT block+32, i.e.
 * EXACTLY where block->file lives. The trailing `block->file = 0` then
 * clobbers the leftover block's `size` field (also at its own offset 0)
 * back to 0, silently erasing however many megabytes were left in the
 * heap from its own ring accounting from that point on.
 *
 * Zero-size requests are routine, not an edge case: dll_bridge.c's
 * synthetic module headers have bssSize=0, and unmodified game code hits
 * the identical request too (e.g. game/objmain.c's omAddObjEx:
 * `HuMemDirectMallocNum(HEAP_HEAP, sizeof(HU3D_MODELID)*mdlcnt, ...)` is
 * zero for any object created with mdlcnt==0, which is common) -- so the
 * fix belongs at this single shared entry point rather than any one
 * caller. Clamping the incoming size to a minimum of 1 byte makes
 * MEM_ALLOC_SIZE() round up to 64 (never the pathological 32), restoring
 * a real gap between a block's `file` field and its neighbor -- exactly
 * the margin the original 32-bit struct had for free. Costs at most one
 * extra 32-byte "slot" per zero-size request; irrelevant against a
 * multi-megabyte heap. */
static s32 mp6_no_zero_alloc(s32 size)
{
    return (size > 0) ? size : 1;
}

/* Lightweight, permanent HEAP_HEAP trace (bounded call count so a long
 * play session doesn't spam stdout): one line per direct allocation
 * showing the live ring total (used + largest-free, valid since boot-time
 * HEAP_HEAP allocation is append-only/non-fragmenting) alongside the
 * static configured size, so a shrinking total is visible at a glance
 * without needing a full block-by-block HuMemHeapDump every call. */
static int g_heapHeapTraceCalls = 0;
#define MP6_HEAPHEAP_TRACE_BUDGET 64
static void mp6_trace_heap_heap(const char *who, s32 rawSize, u32 retaddr, void *result)
{
    if (g_heapHeapTraceCalls >= MP6_HEAPHEAP_TRACE_BUDGET) return;
    g_heapHeapTraceCalls++;
    {
        s32 used = HuMemUsedMemorySizeGet(HeapTbl[HEAP_HEAP]);
        s32 maxFree = HuMemMaxMemorySizeGet(HeapTbl[HEAP_HEAP]);
        printf("[HEAPTRACE] #%d %s(HEAP_HEAP, size=%d) retaddr=0x%08x -> %p "
               "(ring: used=0x%x + maxFree=0x%x = 0x%x, configured total=0x%x)\n",
               g_heapHeapTraceCalls, who, rawSize, retaddr, result,
               used, maxFree, used + maxFree, HeapSizeTbl[HEAP_HEAP]);
        fflush(stdout);
    }
}

/* HEAP_MODEL steady-state tracer: budget-capped (like mp6_trace_heap_heap
 * above) AND tick-gated (does NOT start counting until well after the
 * initial legitimate model-load burst is over) so it isolates
 * STEADY-STATE repeat allocators instead of re-confirming the one-time
 * startup allocations already accounted for by the "[HSF] loaded" trace.
 * Cheap, permanent: any heap-growth investigation gets the same tool. */
static int g_heapModelTraceCalls = 0;
#define MP6_HEAPMODEL_TRACE_BUDGET 4000
#define MP6_HEAPMODEL_TRACE_START_TICK 250000
static void mp6_trace_heap_model(const char *who, s32 rawSize, u32 retaddr, u32 num)
{
    if (mp6_tick_count < MP6_HEAPMODEL_TRACE_START_TICK) return;
    if (g_heapModelTraceCalls >= MP6_HEAPMODEL_TRACE_BUDGET) return;
    g_heapModelTraceCalls++;
    printf("[HEAPMODEL-TRACE] #%d tick=%ld %s(HEAP_MODEL, size=%d, num=0x%08x) retaddr=0x%08x\n",
           g_heapModelTraceCalls, mp6_tick_count, who, rawSize, num, retaddr);
    fflush(stdout);
}

/* ---------------------------------------------------------------------
 * All-heap allocation census (opt-in leak diagnostics). The two tracers
 * above are fixed-scope: mp6_trace_heap_heap has a lifetime budget of 64
 * calls (exhausted during boot), and mp6_trace_heap_model doesn't start
 * counting until a hardcoded late tick. This is the general tool: an
 * ALL-FIVE-HEAP census, gated by an env var
 * (MP6_ALLOC_CENSUS_START_TICK, a tick number) instead of a hardcoded
 * threshold, so it aims at whatever tick range a given scenario's
 * transition actually falls in. Absent the env var, this is a complete
 * no-op (matching every other instrument in this file).
 *
 * Two halves:
 *   1. mp6_alloc_census_tick_check() -- a per-heap USED-bytes/block-count
 *      summary, printed once every MP6_ALLOC_CENSUS_SUMMARY_INTERVAL
 *      ticks, called from mp6_tick_advance() (shims_manual.c, shared by
 *      BOTH build modes -- same choke point the RSS watchdog rides).
 *      Cheap enough (5 heaps' worth of two integers) to keep running for
 *      an entire long soak with no budget cap at all -- this is what
 *      answers "does any heap keep growing" directly.
 *   2. A per-call trace on every HuMemDirect{Malloc,MallocNum,Free,FreeNum}
 *      entry point, all 5 heaps (not just HEAP_HEAP/HEAP_MODEL), budget-
 *      capped (MP6_ALLOC_CENSUS_CALL_BUDGET) since this one really can
 *      flood a long soak otherwise. Every line is symbolized
 *      (mp6_symbolize_addr) so a call site is directly nameable from the
 *      log with no separate map-file lookup. HuMemDirectFree additionally
 *      peeks the block's own magic byte BEFORE calling HuMemMemoryFree (a
 *      read-only shadow of game/memory.c's own private MEMORY_BLOCK --
 *      see MP6ShadowMemBlock below) so every occurrence of the
 *      "HuMem>memory free error" rejection (not just the first) is
 *      directly flagged in THIS log, next to the caller that triggered
 *      it, rather than only ever seeing decomp's own generic OSReport
 *      line with no calling context. */
#define MP6_ALLOC_CENSUS_SUMMARY_INTERVAL 60
#define MP6_ALLOC_CENSUS_CALL_BUDGET 8000

static long g_allocCensusStartTick = -1; /* -1 = disabled (env unset) */
static int g_allocCensusParsed = 0;
static int g_allocCensusCallsLogged = 0;
static const char *const g_heapNames[HEAP_MAX] = { "HEAP_HEAP", "HEAP_SOUND", "HEAP_MODEL", "HEAP_DVD", "HEAP_SPACE" };

static void mp6_alloc_census_parse_env(void)
{
    const char *env;
    g_allocCensusParsed = 1;
    env = getenv("MP6_ALLOC_CENSUS_START_TICK");
    if (env && env[0]) {
        g_allocCensusStartTick = atol(env);
        printf("[ALLOC-CENSUS] armed: start tick=%ld, summary every %d ticks, per-call budget %d\n",
               g_allocCensusStartTick, MP6_ALLOC_CENSUS_SUMMARY_INTERVAL, MP6_ALLOC_CENSUS_CALL_BUDGET);
        fflush(stdout);
    }
}

static int mp6_alloc_census_active(void)
{
    if (!g_allocCensusParsed) mp6_alloc_census_parse_env();
    return g_allocCensusStartTick >= 0 && mp6_tick_count >= g_allocCensusStartTick;
}

/* Called once per tick from mp6_tick_advance() (platform/null/shims_manual.c,
 * declared in mp6_boot.h) -- see this section's own header comment. */
void mp6_alloc_census_tick_check(void)
{
    int i;
    if (!mp6_alloc_census_active()) return;
    if ((mp6_tick_count % MP6_ALLOC_CENSUS_SUMMARY_INTERVAL) != 0) return;
    printf("[ALLOC-CENSUS] tick=%ld", mp6_tick_count);
    for (i = 0; i < HEAP_MAX; i++) {
        printf(" %s{used=%d blk=%d}", g_heapNames[i],
               (int)HuMemUsedMemorySizeGet(HeapTbl[i]), (int)HuMemUsedMemoryBlockGet(HeapTbl[i]));
    }
    printf("\n");
    fflush(stdout);
}

static int mp6_heap_id_for_ptr(const void *ptr)
{
    int i;
    for (i = 0; i < HEAP_MAX; i++) {
        if (ptr >= HeapTbl[i] && (const char *)ptr < (const char *)HeapTbl[i] + HeapSizeTbl[i]) {
            return i;
        }
    }
    return -1;
}

static void mp6_alloc_census_trace_call(const char *who, int heap, s32 rawSize, u32 num, u32 retaddr, void *result)
{
    char sym[256];
    if (!mp6_alloc_census_active()) return;
    if (g_allocCensusCallsLogged >= MP6_ALLOC_CENSUS_CALL_BUDGET) return;
    g_allocCensusCallsLogged++;
    mp6_symbolize_addr((void *)(uintptr_t)retaddr, sym, sizeof(sym));
    printf("[ALLOC-CENSUS-CALL] #%d tick=%ld %s(heap=%s size=%d num=0x%08x) -> %p  caller=%s\n",
           g_allocCensusCallsLogged, mp6_tick_count, who,
           (heap >= 0 && heap < HEAP_MAX) ? g_heapNames[heap] : "?", (int)rawSize, num, result, sym);
    fflush(stdout);
}

/* Read-only shadow of game/memory.c's own private MEMORY_BLOCK struct (see
 * that file, and patches/decomp/src/game/memory.c.patch's MP6_MEMBLOCK_HDR
 * comment for why prev/next are real 8-byte pointers on this native
 * build): identical field order/types, so this native compiler lays it out
 * byte-identically without memory.c needing to export the real, private
 * type at all. Never written, only read, and only ever through a pointer
 * HuMemDirectFree itself already received (never independently walked or
 * guessed at). */
typedef struct {
    int32_t size;
    uint8_t magic;
    uint8_t flag;
    void *prev;
    void *next;
    uint32_t num;
    uint32_t retAddr;
    uint32_t file;
} MP6ShadowMemBlock;

static void mp6_alloc_census_trace_free(const char *who, void *ptr, u32 num, u32 retaddr)
{
    char sym[256];
    int heap;
    MP6ShadowMemBlock *blk;

    if (!mp6_alloc_census_active()) return;
    if (!ptr) return;
    if (g_allocCensusCallsLogged >= MP6_ALLOC_CENSUS_CALL_BUDGET) return;
    g_allocCensusCallsLogged++;
    heap = mp6_heap_id_for_ptr(ptr);
    blk = (MP6ShadowMemBlock *)((char *)ptr - sizeof(MP6ShadowMemBlock));
    mp6_symbolize_addr((void *)(uintptr_t)retaddr, sym, sizeof(sym));
    printf("[ALLOC-CENSUS-CALL] #%d tick=%ld %s(heap=%s ptr=%p num=0x%08x magic=%u) caller=%s%s\n",
           g_allocCensusCallsLogged, mp6_tick_count, who,
           (heap >= 0) ? g_heapNames[heap] : "?(outside all heaps)", ptr, num, (unsigned)blk->magic, sym,
           (blk->magic != 165) ? "  *** INVALID MAGIC -- HuMemMemoryFree will REJECT this free ***" : "");
    fflush(stdout);
}

/* Bytes readable from `ptr` through the end of its OWN direct-malloc block,
 * or 0 when `ptr` is not verifiably a live block base (wrong magic, outside
 * every heap, or NULL). Read-only, and only via the same shadow-header
 * convention the alloc census above already relies on.
 *
 * Purpose: lets a consumer that receives a bare buffer pointer with no
 * length (the HSF loader's file buffers, allocated by game/data.c) bound
 * its reads to the allocation instead of trusting in-file offsets -- see
 * hsf_load_native.c's LoadBitmaps for the concrete case (2 real disc files
 * are EOF-truncated mid-bitmap).
 *
 * block->size is game/memory.c's own MEM_ALLOC_SIZE(request), i.e. it
 * INCLUDES the block header. We subtract sizeof(MP6ShadowMemBlock) -- the
 * native header footprint -- which is a conservative (never over-reporting)
 * bound regardless of the 32-vs-native-header-size wrinkle documented in
 * the MP6_MEMBLOCK_HDR comment: worst case we under-report by 8 bytes. */
uint32_t mp6_heap_block_data_size(const void *ptr)
{
    const MP6ShadowMemBlock *blk;
    if (ptr == NULL) return 0;
    if (mp6_heap_id_for_ptr(ptr) < 0) return 0;
    blk = (const MP6ShadowMemBlock *)((const char *)ptr - sizeof(MP6ShadowMemBlock));
    if (blk->magic != 165) return 0;
    if (blk->size <= (int32_t)sizeof(MP6ShadowMemBlock)) return 0;
    return (uint32_t)blk->size - (uint32_t)sizeof(MP6ShadowMemBlock);
}

void *HuMemDirectMalloc(HEAPID heap, s32 size)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    s32 rawSize = size;
    size = OSRoundUp32B(mp6_no_zero_alloc(size));
    {
        void *result = HuMemMemoryAlloc(HeapTbl[heap], size, retaddr);
        if (heap == HEAP_HEAP) mp6_trace_heap_heap("HuMemDirectMalloc", rawSize, retaddr, result);
        if (heap == HEAP_MODEL) mp6_trace_heap_model("HuMemDirectMalloc", rawSize, retaddr, 0);
        mp6_alloc_census_trace_call("HuMemDirectMalloc", heap, rawSize, (u32)-256, retaddr, result);
        return result;
    }
}

void *HuMemDirectMallocNum(HEAPID heap, s32 size, u32 num)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    s32 rawSize = size;
    size = OSRoundUp32B(mp6_no_zero_alloc(size));
    {
        void *result = HuMemMemoryAllocNum(HeapTbl[heap], size, num, retaddr);
        if (heap == HEAP_HEAP) mp6_trace_heap_heap("HuMemDirectMallocNum", rawSize, retaddr, result);
        if (heap == HEAP_MODEL) mp6_trace_heap_model("HuMemDirectMallocNum", rawSize, retaddr, num);
        mp6_alloc_census_trace_call("HuMemDirectMallocNum", heap, rawSize, num, retaddr, result);
        return result;
    }
}

void *HuMemDirectTailMalloc(HEAPID heap, s32 size)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    size = OSRoundUp32B(mp6_no_zero_alloc(size));
    return HuMemTailMemoryAlloc(HeapTbl[heap], size, retaddr);
}

void *HuMemDirectTailMallocNum(HEAPID heap, s32 size, u32 num)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    size = OSRoundUp32B(mp6_no_zero_alloc(size));
    return HuMemTailMemoryAllocNum(HeapTbl[heap], size, num, retaddr);
}

void *HuMemDirectRealloc(HEAPID heap, void *ptr, s32 size)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    return HuMemMemoryRealloc(HeapTbl[heap], ptr, mp6_no_zero_alloc(size), retaddr);
}

void HuMemDirectFree(void *ptr)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    mp6_alloc_census_trace_free("HuMemDirectFree", ptr, (u32)-256, retaddr);
    HuMemMemoryFree(ptr, retaddr);
}

/* A full walk of `heap`'s own circular MEMORY_BLOCK list
 * (MP6ShadowMemBlock's own comment explains why this is safe read-only
 * from platform code), listing every ACTIVE block whose `num` tag matches
 * `num` (matching exactly what game/memory.c's own HuMemMemoryFreeNum is
 * about to walk and free) -- called both immediately BEFORE and
 * immediately AFTER the real bulk-free below, so "what was here" and
 * "what's left" are both on record for the exact same tag/heap pair,
 * without needing a decomp-side patch at all (game/memory.c's own
 * MEMORY_BLOCK fields -- size/magic/flag/num/retAddr -- are read via the
 * same shadow layout mp6_alloc_census_trace_free already uses). Gated by
 * the same MP6_ALLOC_CENSUS_START_TICK env var as the rest of this
 * section; a no-op otherwise. */
static void mp6_heap_walk_tagged(HEAPID heap, u32 num, const char *context)
{
    MP6ShadowMemBlock *start;
    MP6ShadowMemBlock *block;
    int shown = 0;
    int totalActive = 0;
    s32 totalActiveBytes = 0;

    if (!mp6_alloc_census_active()) return;
    start = (MP6ShadowMemBlock *)HeapTbl[heap];
    block = start;
    do {
        if (block->flag == 1) {
            totalActive++;
            totalActiveBytes += block->size;
            if (block->num == num) {
                char sym[256];
                mp6_symbolize_addr((void *)(uintptr_t)block->retAddr, sym, sizeof(sym));
                printf("[ALLOC-CENSUS-WALK] %s heap=%s tick=%ld block=%p size=%d magic=%u num=0x%08x "
                       "origAllocator=%s\n",
                       context, g_heapNames[heap], mp6_tick_count, (void *)block, block->size,
                       (unsigned)block->magic, block->num, sym);
                shown++;
            }
        }
        block = (MP6ShadowMemBlock *)block->next;
    } while (block != start);
    printf("[ALLOC-CENSUS-WALK] %s heap=%s tick=%ld: %d block(s) tagged num=0x%08x shown above; "
           "heap totals: %d active block(s), %d active byte(s)\n",
           context, g_heapNames[heap], mp6_tick_count, shown, num, totalActive, totalActiveBytes);
    fflush(stdout);
}

void HuMemDirectFreeNum(HEAPID heap, u32 num)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    mp6_alloc_census_trace_call("HuMemDirectFreeNum(pre)", heap, 0, num, retaddr, NULL);
    mp6_heap_walk_tagged(heap, num, "BEFORE HuMemDirectFreeNum");
    HuMemMemoryFreeNum(HeapTbl[heap], num, retaddr);
    mp6_heap_walk_tagged(heap, num, "AFTER  HuMemDirectFreeNum");
}

s32 HuMemUsedMallocSizeGet(HEAPID heap)
{
    return HuMemUsedMemorySizeGet(HeapTbl[heap]);
}

s32 HuMemUsedMallocBlockGet(HEAPID heap)
{
    return HuMemUsedMemoryBlockGet(HeapTbl[heap]);
}

u32 HuMemHeapSizeGet(HEAPID heap)
{
    return HeapSizeTbl[heap];
}

void *HuMemHeapPtrGet(HEAPID heap)
{
    return HeapTbl[heap];
}
