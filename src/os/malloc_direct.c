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
 * (88MB, 8x the original) is what src/hsf/hsf_load_native.c's native
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
 *
 * Those four widened numbers are the port's BASELINE -- what it ships with
 * the Enhancements "Expanded heaps" toggle off -- and they now live as named
 * constants in include/mp6_heap_scale.h so the enhancement can multiply
 * them without restating them. Everything else in this file reads capacities
 * out of HeapSizeTbl, and HuMemInitAll rewrites that one table from the
 * baseline at the active scale, so the whole consumer set below (the
 * pointer-to-heap classifier, the block-bounds probe, HuMemHeapSizeGet, the
 * DC flush extent, the census/diag panels) sees ACTIVE capacities with no
 * per-consumer change. At scale 1 the rewrite is the identity and the table
 * is byte-for-byte what it was before the toggle existed.
 */
#include "game/memory.h"
#include "game/init.h"
#include "dolphin/os.h"
#include "mp6_boot.h" /* mp6_tick_count -- mp6_trace_heap_model's tick gate below */
#include "mp6_savestate.h"
#include "mp6_alloc_size.h"
#include "mp6_anim_native.h"
#include "mp6_gxarray_registry.h"
#include "mp6_heap_scale.h" /* Enhancements: expanded HuMem capacities */
#include "mp6_diag_probe.h" /* pull-side heap snapshot -- see the block at the end */
#include "mp6_console.h"    /* the MP6_ALLOC_CENSUS_START_TICK runtime lever */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(MP6_HEAP_TABLE_LEN == HEAP_MAX,
               "mp6_heap_scale.h's table length must equal the decomp's HEAP_MAX");
_Static_assert(MP6_HEAP_FIXED_COUNT < HEAP_MAX,
               "HEAP_SPACE must stay outside the configured/scaled range");

/* The live table. Written once by HuMemInitAll from the baseline constants
 * (never multiplied IN PLACE -- a second call would compound the scale), and
 * a second time for HEAP_SPACE, whose size is a measured remainder rather
 * than a configured capacity. Ordinary restorable .data, deliberately: the
 * capacities a savestate was captured under are part of the world it
 * describes, and restoring them alongside the heaps they measure is what
 * keeps a state self-consistent when it is loaded into a process configured
 * at a different scale. */
static u32 HeapSizeTbl[HEAP_MAX] = {
    MP6_HEAP_RETAIL_HEAP, MP6_HEAP_RETAIL_SOUND,
    MP6_HEAP_RETAIL_MODEL, MP6_HEAP_RETAIL_DVD, 0
};
static void *HeapTbl[HEAP_MAX];

/* MP6_HEAP_SCALE_PROBE=1 -- the one measurement that distinguishes a scaled
 * table from a scaled-looking one: ask the ALLOCATOR for a block that does
 * not fit the baseline capacity and see whether it is served. Requests
 * 1.5x the BASELINE HEAP_MODEL capacity, which is unserviceable at scale 1
 * (the whole heap is smaller than the request) and comfortably inside
 * HEAP_MODEL at scale 4, then frees it again. Prints exactly one line and
 * changes nothing else; absent the env var this is a complete no-op,
 * matching every other instrument in this file.
 *
 * It runs against the real HuMemMemoryAlloc through the real HuMemDirectMalloc
 * wrapper -- not against HeapSizeTbl -- so a table that was scaled without the
 * arena growing to match (the failure mode this whole change exists to avoid)
 * reports GRANTED=no at scale 4 rather than passing on the strength of a
 * number nobody allocated against. tools/heap_scale_gate.py is the consumer. */
static void mp6_heap_scale_probe(unsigned int scale)
{
    const char *env = getenv("MP6_HEAP_SCALE_PROBE");
    s32 request;
    void *block;

    if (env == NULL || env[0] == '\0' || env[0] == '0') return;
    request = (s32)(MP6_HEAP_RETAIL_MODEL + MP6_HEAP_RETAIL_MODEL / 2u);
    block = HuMemDirectMalloc(HEAP_MODEL, request);
    printf("[HEAPSCALE-PROBE] scale=x%u heap=HEAP_MODEL capacity=0x%08x request=0x%08x granted=%s\n",
           scale, HeapSizeTbl[HEAP_MODEL], (unsigned)request, block != NULL ? "yes" : "no");
    fflush(stdout);
    if (block != NULL) HuMemDirectFree(block);
}

void HuMemInitAll(void)
{
    s32 i;
    void *ptr;
    u32 free_size;
    unsigned int scale = mp6_heap_scale_active();
    mp6_gxarray_reset();
    /* Enhancements "Expanded heaps": the four CONFIGURED capacities, re-derived
     * from the baseline at the active scale. Derived rather than multiplied so
     * this is idempotent and so scale 1 provably reproduces the exact baseline
     * table; the arena that has to hold the result was reserved from the same
     * latched scale (src/os/arena.c). HEAP_SPACE is untouched here -- it
     * is whatever the OS heap has left, and the loop below records that. */
    for (i = 0; i < MP6_HEAP_FIXED_COUNT; i++) {
        HeapSizeTbl[i] = mp6_heap_scaled_capacity((int)i, scale);
    }
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
    mp6_heap_scale_probe(scale);
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
static int mp6_checked_direct_alloc_size(const char *api, s32 requested, s32 *roundedOut)
{
    if (mp6_humem_checked_request(requested, roundedOut)) return 1;
    fprintf(stderr, "[ALLOC] %s rejected invalid/overflowing size %d\n", api, (int)requested);
    return 0;
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

void mp6_malloc_savestate_capture_host_config(Mp6SsAllocDiagHostConfig *out)
{
    if (out == NULL) return;
    out->censusStartTick = g_allocCensusStartTick;
    out->censusParsed = g_allocCensusParsed;
    out->censusCallsLogged = g_allocCensusCallsLogged;
}

void mp6_malloc_savestate_apply_host_config(const Mp6SsAllocDiagHostConfig *in)
{
    if (in == NULL) return;
    if ((in->censusParsed != 0 && in->censusParsed != 1) ||
        in->censusCallsLogged < 0 ||
        in->censusCallsLogged > MP6_ALLOC_CENSUS_CALL_BUDGET) {
        g_allocCensusStartTick = -1;
        g_allocCensusParsed = 0;
        g_allocCensusCallsLogged = 0;
        return;
    }
    g_allocCensusStartTick = in->censusStartTick;
    g_allocCensusParsed = in->censusParsed;
    g_allocCensusCallsLogged = in->censusCallsLogged;
}

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
    long startTick;
    if (!g_allocCensusParsed) mp6_alloc_census_parse_env();
    /* env latches the initial arming tick; the dev console may override it live
     * (`set alloccensus 0` arms from now, -1 disarms). The override deliberately
     * does NOT write g_allocCensusStartTick: that static is savestate-marshalled
     * with its own validation (mp6_malloc_savestate_apply_host_config above),
     * and a console value written into it would be captured into a state file
     * and re-applied on every later restore. The console's value belongs to the
     * live debug session, so it stays in the console's own table. */
    startTick = (long)mp6_console_cvar_get(MP6_CVAR_ALLOC_CENSUS,
                                           (int)g_allocCensusStartTick);
    return startTick >= 0 && mp6_tick_count >= startTick;
}

/* Called once per tick from mp6_tick_advance() (src/null/shims_manual.c,
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
    uintptr_t p;
    if (ptr == NULL) return -1;
    p = (uintptr_t)ptr;
    for (i = 0; i < HEAP_MAX; i++) {
        uintptr_t start = (uintptr_t)HeapTbl[i];
        uintptr_t end;
        if (start == 0 || HeapSizeTbl[i] == 0 ||
            (uintptr_t)HeapSizeTbl[i] > UINTPTR_MAX - start) {
            continue;
        }
        end = start + (uintptr_t)HeapSizeTbl[i];
        if (p >= start && p < end) {
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
 * that file, and compat/decomp/src/game/memory.c.patch's MP6_MEMBLOCK_HDR
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
    mp6_symbolize_addr((void *)(uintptr_t)retaddr, sym, sizeof(sym));
    if (heap < 0 || (uintptr_t)ptr < (uintptr_t)HeapTbl[heap] + sizeof(*blk)) {
        printf("[ALLOC-CENSUS-CALL] #%d tick=%ld %s(heap=?(outside all heaps) ptr=%p "
               "num=0x%08x) caller=%s  *** INVALID POINTER ***\n",
               g_allocCensusCallsLogged, mp6_tick_count, who, ptr, num, sym);
        fflush(stdout);
        return;
    }
    blk = (MP6ShadowMemBlock *)((char *)ptr - sizeof(MP6ShadowMemBlock));
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
int mp6_heap_block_info(const void *ptr, int32_t *heapOut,
                        uint32_t *sizeOut, uint32_t *tagOut)
{
    const MP6ShadowMemBlock *blk;
    uintptr_t start, end, p, blockAddr;
    int heap;
    if (heapOut != NULL) *heapOut = -1;
    if (sizeOut != NULL) *sizeOut = 0;
    if (tagOut != NULL) *tagOut = 0;
    if (ptr == NULL) return 0;
    heap = mp6_heap_id_for_ptr(ptr);
    if (heap < 0) return 0;
    start = (uintptr_t)HeapTbl[heap];
    end = start + (uintptr_t)HeapSizeTbl[heap];
    p = (uintptr_t)ptr;
    if (p < start + sizeof(MP6ShadowMemBlock)) return 0;
    blockAddr = p - sizeof(MP6ShadowMemBlock);
    blk = (const MP6ShadowMemBlock *)((const char *)ptr - sizeof(MP6ShadowMemBlock));
    if (blk->magic != 165 || blk->flag != 1) return 0;
    if (blk->size <= (int32_t)sizeof(MP6ShadowMemBlock)) return 0;
    if ((uint32_t)blk->size > end - blockAddr) return 0;
    if (heapOut != NULL) *heapOut = heap;
    if (sizeOut != NULL) {
        *sizeOut = (uint32_t)blk->size - (uint32_t)sizeof(MP6ShadowMemBlock);
    }
    if (tagOut != NULL) *tagOut = blk->num;
    return 1;
}

uint32_t mp6_heap_block_data_size(const void *ptr)
{
    uint32_t size = 0;
    (void)mp6_heap_block_info(ptr, NULL, &size, NULL);
    return size;
}

uint32_t mp6_heap_pointer_tag(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    if (!mp6_heap_block_info(ptr, NULL, NULL, NULL) || value > UINT32_MAX) {
        fprintf(stderr, "[FATAL] allocation pointer cannot be represented as model tag: %p\n",
                ptr);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    return (uint32_t)value;
}

void *HuMemDirectMalloc(HEAPID heap, s32 size)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    s32 rawSize = size;
    if (!mp6_checked_direct_alloc_size("HuMemDirectMalloc", size, &size)) return NULL;
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
    if (!mp6_checked_direct_alloc_size("HuMemDirectMallocNum", size, &size)) return NULL;
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
    if (!mp6_checked_direct_alloc_size("HuMemDirectTailMalloc", size, &size)) return NULL;
    return HuMemTailMemoryAlloc(HeapTbl[heap], size, retaddr);
}

void *HuMemDirectTailMallocNum(HEAPID heap, s32 size, u32 num)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    if (!mp6_checked_direct_alloc_size("HuMemDirectTailMallocNum", size, &size)) return NULL;
    return HuMemTailMemoryAllocNum(HeapTbl[heap], size, num, retaddr);
}

void *HuMemDirectRealloc(HEAPID heap, void *ptr, s32 size)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    if (!mp6_checked_direct_alloc_size("HuMemDirectRealloc", size, &size)) return NULL;
    void *result = HuMemMemoryRealloc(HeapTbl[heap], ptr, size, retaddr);
    /* Even an in-place resize invalidates the old array extent. A failed
     * realloc leaves the original allocation and registration untouched. */
    if (result != NULL) mp6_gxarray_before_direct_free(ptr);
    return result;
}

void HuMemDirectFree(void *ptr)
{
    u32 retaddr = (u32)(uintptr_t)__builtin_return_address(0);
    mp6_alloc_census_trace_free("HuMemDirectFree", ptr, (u32)-256, retaddr);
    mp6_anim_before_direct_free(ptr);
    mp6_gxarray_before_direct_free(ptr);
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
    /* Packed ANM parsing builds a native pointer graph whose lifetime tag
     * mirrors its raw backing allocation. Invalidate those side-table
     * identities while the block metadata is still live, at the one seam
     * shared by overlay and model-specific bulk reclamation. */
    mp6_anim_before_bulk_free(heap, num);
    mp6_gxarray_before_bulk_free(heap, num);
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

/* =======================================================================
 * Pull-side heap snapshot (include/mp6_diag_probe.h).
 *
 * Exactly the loop mp6_alloc_census_tick_check() above already runs, minus
 * the printf: used bytes and block count per heap, plus the capacity from
 * HeapSizeTbl and the largest free block from HuMemMaxMemorySizeGet (the same
 * call HuMemInitAll's own boot diagnostic uses). No new measurement, no arming
 * and no side effect -- reading this must never perturb the census the env
 * lever controls.
 *
 * HEAP_SPACE is deliberately reported as uninitialized rather than skipped:
 * HuMemInitAll only creates heaps 0..3 (HeapSizeTbl[4] is 0), and a panel that
 * silently dropped the fifth row would read as "there are four heaps".
 * ======================================================================= */
int mp6_diag_heap_count(void)
{
    return (HEAP_MAX < MP6_DIAG_HEAP_MAX) ? HEAP_MAX : MP6_DIAG_HEAP_MAX;
}

int mp6_diag_heap(int index, Mp6DiagHeap *out)
{
    if (out == NULL || index < 0 || index >= mp6_diag_heap_count()) return 0;
    out->name = g_heapNames[index];
    out->capacity = HeapSizeTbl[index];
    if (HeapTbl[index] == NULL) {
        out->initialized = 0;
        out->used = 0;
        out->blocks = 0;
        out->largestFree = 0;
        return 1;
    }
    out->initialized = 1;
    out->used = (int)HuMemUsedMemorySizeGet(HeapTbl[index]);
    out->blocks = (int)HuMemUsedMemoryBlockGet(HeapTbl[index]);
    out->largestFree = (int)HuMemMaxMemorySizeGet(HeapTbl[index]);
    return 1;
}
