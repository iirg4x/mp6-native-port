/* MP6 native port -- loading trace; see include/mp6_load_trace.h. */
#include "mp6_load_trace.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__linux__)
#include <malloc.h>
#define MP6_HAVE_MALLINFO 1
#endif

#include "mp6_host_section.h" /* diagnostic state of the running process */

static const char *const g_loadKindNames[MP6_LOAD_KIND_COUNT] = {
    "dvd", "decode", "hsf", "segroup", "stream",
};

static int g_loadTraceOn = -1;
static uint64_t g_loadNs[MP6_LOAD_KIND_COUNT];
static uint64_t g_loadBytes[MP6_LOAD_KIND_COUNT];
static unsigned g_loadCalls[MP6_LOAD_KIND_COUNT];

static int mp6_load_trace_enabled(void)
{
    if (g_loadTraceOn < 0) {
        const char *e = getenv("MP6_LOAD_TRACE");
        g_loadTraceOn = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return g_loadTraceOn;
}

uint64_t mp6_load_trace_begin(void)
{
    return mp6_load_trace_enabled() ? mp6_host_monotonic_ns() : 0;
}

void mp6_load_trace_end(uint64_t start, Mp6LoadKind kind, uint64_t bytes)
{
    if (start == 0 || (unsigned)kind >= MP6_LOAD_KIND_COUNT) return;
    g_loadNs[kind] += mp6_host_monotonic_ns() - start;
    g_loadBytes[kind] += bytes;
    g_loadCalls[kind]++;
}

extern void *HuMemHeapPtrGet(int heap);
extern uint32_t HuMemHeapSizeGet(int heap);
extern int mp6_heap_check(void *heapHead, uint32_t heapBytes, char *why, int whyBytes);

/* MP6_HEAP_CHECK=1: walk the five HuMem heaps after every tick and report
 * the first broken block header (once per heap), naming the block before
 * it -- the allocation that overran. Slow; for chasing corruption. */
static void mp6_heap_check_tick(long tick)
{
    static int on = -1;
    static int reported[5];
    int h;
    if (on < 0) {
        const char *e = getenv("MP6_HEAP_CHECK");
        on = e && e[0] == '1';
    }
    if (!on) return;
    for (h = 0; h < 5; h++) {
        char why[256];
        void *head = HuMemHeapPtrGet(h);
        if (reported[h] || head == NULL) continue;
        if (!mp6_heap_check(head, HuMemHeapSizeGet(h), why, (int)sizeof(why))) {
            reported[h] = 1;
            printf("[MP6-HEAP] heap %d broken after tick %ld: %s\n", h, tick, why);
            fflush(stdout);
        }
    }
}

void mp6_load_trace_tick(long tick)
{
    uint64_t total = 0;
    int k;
    mp6_heap_check_tick(tick);
    if (tick > 0 && tick % 300 == 0) mp6_mem_trace("tick", tick);
    if (g_loadTraceOn != 1) return;
    for (k = 0; k < MP6_LOAD_KIND_COUNT; k++) total += g_loadNs[k];
    if (total >= (uint64_t)MP6_LOAD_TRACE_TICK_MS * 1000000u) {
        char line[512];
        int n = snprintf(line, sizeof(line), "[MP6-LOAD] tick=%ld total=%.1fms", tick, total / 1e6);
        for (k = 0; k < MP6_LOAD_KIND_COUNT && n > 0 && (size_t)n < sizeof(line); k++) {
            if (g_loadCalls[k] == 0) continue;
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%.1fms/%u/%lluKB",
                          g_loadKindNames[k], g_loadNs[k] / 1e6, g_loadCalls[k],
                          (unsigned long long)(g_loadBytes[k] / 1024u));
        }
        printf("%s\n", line);
        fflush(stdout);
        mp6_hsf_profile_flush();
    }
    memset(g_loadNs, 0, sizeof(g_loadNs));
    memset(g_loadBytes, 0, sizeof(g_loadBytes));
    memset(g_loadCalls, 0, sizeof(g_loadCalls));
}

/* MP6_MEM_TRACE=1: one "[MP6-MEM]" line at each overlay start and every
 * 300 ticks -- process memory (the wasm heap's size on the web, which only
 * grows; resident set elsewhere) and the C heap's live bytes where the
 * runtime reports them. Finds what a scene leaves resident. */
static int mp6_mem_trace_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MP6_MEM_TRACE");
        on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return on;
}

extern int32_t HuMemUsedMallocSizeGet(int heap);
extern uint32_t HuMemHeapSizeGet(int heap);

void mp6_mem_trace(const char *label, long tick)
{
    unsigned long long liveKB = 0;
    if (!mp6_mem_trace_enabled()) return;
#ifdef MP6_HAVE_MALLINFO
    {
        struct mallinfo mi = mallinfo();
        liveKB = (unsigned long long)(unsigned)mi.uordblks / 1024u;
    }
#endif
    printf("[MP6-MEM] %s tick=%ld process=%lluMB malloc-live=%lluMB", label, tick,
           (unsigned long long)(mp6_host_rss_bytes() >> 20), liveKB >> 10);
    {
        /* The game's five HuMem heaps (game/memory.h order: HEAP_HEAP,
         * SOUND, MODEL, DVD, SPACE): used and peak-used KB of capacity. */
        static const char *const names[5] = { "heap", "sound", "model", "dvd", "space" };
        static uint32_t peak[5];
        int h;
        for (h = 0; h < 5; h++) {
            uint32_t used = (uint32_t)HuMemUsedMallocSizeGet(h);
            if (used > peak[h]) peak[h] = used;
            printf(" %s=%u/%u/%uKB", names[h], used >> 10, peak[h] >> 10, HuMemHeapSizeGet(h) >> 10);
        }
    }
    printf("\n");
    fflush(stdout);
}
