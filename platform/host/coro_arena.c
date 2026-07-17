/* MP6 native port -- the arena-backed coroutine backend (the default on
 * every platform).
 *
 * WHY THIS EXISTS: the port's HuPrc process scheduler
 * (platform/os/process_native.c) runs every game process on its own
 * stackful coroutine. A Win32 FIBER's stack is self-allocated by the OS
 * at an address this 64-bit build does NOT control -- possibly >=4 GB. A
 * process's locals live on that stack, and the game routinely takes the
 * address of a local and stores it in a u32 field (e.g. message-insert of
 * a stack-formatted string; the same u32<->pointer round-trip the whole
 * low-arena model rests on). A stack >=4 GB silently truncates that
 * pointer. This backend removes the risk by construction: every coroutine
 * stack comes from a fixed low-4-GB reservation (the same candidate-base
 * VirtualAlloc/mmap the game arena uses -- mp6_host_arena_reserve),
 * asserted <4 GB at reservation.
 *
 * HOW: minicoro (single-header, MIT; vendored verbatim as
 * platform/host/minicoro.h, v0.2.0) with MCO_USE_ASM -- its assembly
 * context switch takes a CALLER-PROVIDED stack (fibers cannot;
 * CreateFiber always self-allocates), and correctly saves/restores the
 * win32 TIB stack fields (StackBase/StackLimit/DeallocationStack) on
 * every switch, so SEH and the crash handler's StackWalk64
 * (host_win32.c) see correct bounds even for a fault taken on a
 * coroutine stack. minicoro's allocator hook is what lets each
 * coroutine's mco_coro struct + stack land in our pool in one block.
 *
 * The SCHEDULER IS UNCHANGED by backend choice: process_native.c's
 * dispatch loop, yield status, slot-reuse table, and HUPROCESS
 * observables are identical -- this file only provides the three
 * mp6_coro_* context primitives (host.h). minicoro's mco_resume/mco_yield
 * "return to whoever resumed me" chain reproduces the fiber backend's
 * saved-dispatcher nesting discipline exactly; the whole scheduler runs
 * on the game's one main thread.
 *
 * Compiled into BOTH build modes (tools/build.py PLATFORM_SOURCES_COMMON).
 * This whole file is #ifndef MP6_CORO_FIBERS: a -DMP6_CORO_FIBERS build
 * compiles it to nothing and host_win32.c's fiber backend provides
 * mp6_coro_* instead (exactly one backend defines the symbols).
 * Deliberately free of windows.h: minicoro's ASM+custom-allocator path
 * pulls in no OS header, and the one OS call (the pool reservation) goes
 * through the host seam -- so this same file serves the Android backend,
 * with mp6_host_arena_reserve there backed by mmap over the same
 * candidate bases.
 */
#ifndef MP6_CORO_FIBERS

#include "host.h"

/* minicoro backend selection (must precede the include):
 *   MCO_USE_ASM  -- the caller-provided-stack context switch. On Windows
 *                   x86_64 minicoro DEFAULTS to fibers (which self-allocate
 *                   an OS stack -- exactly what we are replacing); forcing
 *                   ASM is what makes the stack come from our pool.
 * The default allocator/logging stay as-is; we override the allocator per
 * coroutine via mco_desc (below) so nothing here ever calls malloc for a
 * stack, and correct usage produces no MCO_LOG output. */
#define MCO_USE_ASM
#define MINICORO_IMPL
#include "minicoro.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Keep in lockstep with process_native.c's own limits (verified equal):
 * the scheduler caps concurrent processes at MP6_MAX_PROC_FIBERS=128 and
 * sizes each fiber stack at MP6_FIBER_STACK_SIZE=1 MB. Matching both here
 * means this backend introduces NO new cap and NO smaller stack than the
 * fiber backend had -- zero behavior change (an overflow here would have
 * been an overflow there too). */
#define MP6_CORO_MAX_SLOTS  128
#define MP6_CORO_STACK_SIZE (1u * 1024u * 1024u)
/* One pool slot holds minicoro's whole per-coroutine block (mco_coro +
 * context + 1 KB storage + the stack + alignment). 64 KB of header room
 * over the 1 MB stack is ~40x minicoro's actual ~1.5 KB overhead -- the
 * exact fit is asserted per create() below, so this generous constant can
 * never silently under-size a slot. */
#define MP6_CORO_SLOT_SIZE  (MP6_CORO_STACK_SIZE + 0x10000u)
#define MP6_CORO_POOL_SIZE  ((size_t)MP6_CORO_MAX_SLOTS * (size_t)MP6_CORO_SLOT_SIZE)

/* host.h's opaque Mp6Coro, defined here for the arena backend (the fiber
 * backend has its own definition -- only one is ever compiled). A fixed
 * static array indexed by pool slot: no malloc, and a stable identity
 * process_native.c can hold across dispatches. */
struct Mp6Coro {
    mco_coro *co;         /* minicoro coroutine (lives in this slot's pool block) */
    void    (*fn)(void *); /* entry passed by process_native.c (ProcessTrampoline) */
    void     *arg;
    int       slot;       /* pool slot index, -1 when free */
};

static uint8_t   *g_pool;                        /* base of the low-4GB coro pool */
static int        g_poolInit;
static Mp6Coro    g_wrappers[MP6_CORO_MAX_SLOTS];
static int16_t    g_freeList[MP6_CORO_MAX_SLOTS]; /* stack of free slot indices */
static int        g_freeTop;                      /* # free slots available */
static int        g_slotsInUse;                   /* live coroutines (diagnostic) */
static int        g_slotsHighWater;               /* max concurrent (diagnostic) */

static void mp6_coro_pool_report(void)
{
    /* Opt-in only (MP6_CORO_DEBUG): the default path emits ZERO output so
     * the headless boot log stays byte-stable for the log-diff gates. */
    if (getenv("MP6_CORO_DEBUG")) {
        printf("[CORO] arena backend peak concurrency: %d / %d slots (%d MB pool committed)\n",
               g_slotsHighWater, MP6_CORO_MAX_SLOTS, (int)(MP6_CORO_POOL_SIZE / (1024 * 1024)));
        fflush(stdout);
    }
}

static void mp6_coro_pool_init(void)
{
    int i;
    g_pool = (uint8_t *)mp6_host_arena_reserve(MP6_CORO_POOL_SIZE);
    if (!g_pool) {
        fprintf(stderr, "[FATAL] coro_arena: could not reserve a %zu-byte low-4GB coroutine stack pool\n",
                (size_t)MP6_CORO_POOL_SIZE);
        fflush(stderr);
        exit(1);
    }
    /* The whole point of this backend: prove the stacks are <4GB. If even
     * the OS-picked fallback inside mp6_host_arena_reserve landed the pool
     * >=4GB, fail loudly at boot rather than let a truncated stack-local
     * pointer corrupt state silently later. */
    if (((uintptr_t)g_pool + MP6_CORO_POOL_SIZE) > 0xFFFFFFFFu) {
        fprintf(stderr,
                "[FATAL] coro_arena: coroutine stack pool at %p (+%zu bytes) is NOT entirely below 4GB "
                "-- HuPrc process stacks must be <4GB (process-stack locals escape into u32 fields). "
                "No low candidate base was free.\n",
                (void *)g_pool, (size_t)MP6_CORO_POOL_SIZE);
        fflush(stderr);
        exit(1);
    }
    /* Fill the free-list so pop() hands out 0,1,2,... in order -- matches
     * the fiber table's slot-0-first growth, purely for reasoning parity;
     * slot choice is never observable. */
    for (i = 0; i < MP6_CORO_MAX_SLOTS; i++) {
        g_freeList[i] = (int16_t)(MP6_CORO_MAX_SLOTS - 1 - i);
    }
    g_freeTop = MP6_CORO_MAX_SLOTS;
    g_poolInit = 1;
    atexit(mp6_coro_pool_report);
    if (getenv("MP6_CORO_DEBUG")) {
        printf("[CORO] arena pool: base=%p size=%dMB slots=%d slot=%dKB stack=%dKB (below 4GB) "
               "[minicoro v0.2.0, MCO_USE_ASM]\n",
               (void *)g_pool, (int)(MP6_CORO_POOL_SIZE / (1024 * 1024)), MP6_CORO_MAX_SLOTS,
               (int)(MP6_CORO_SLOT_SIZE / 1024), (int)(MP6_CORO_STACK_SIZE / 1024));
        fflush(stdout);
    }
}

/* minicoro allocator hooks: alloc returns this coroutine's pre-reserved
 * pool slot (passed as allocator_data); dealloc is a no-op -- the slot is
 * returned to the free-list by mp6_coro_destroy, the one place that knows
 * the slot index. */
static void *mp6_coro_slot_alloc(size_t size, void *slot_mem)
{
    (void)size; /* fit asserted in mp6_coro_create before mco_create runs */
    return slot_mem;
}

static void mp6_coro_slot_dealloc(void *ptr, size_t size, void *slot_mem)
{
    (void)ptr;
    (void)size;
    (void)slot_mem;
}

/* minicoro entry (signature void(mco_coro*)) -> the seam's void(*)(void*).
 * Never returns: process_native.c's ProcessTrampoline FATALs if a game
 * process function ever plain-returns, and the normal path leaves via
 * mp6_coro_switch(NULL)=mco_yield (the coroutine stays SUSPENDED, later
 * torn down by mp6_coro_destroy from the dispatcher). */
static void mp6_coro_entry(mco_coro *co)
{
    Mp6Coro *w = (Mp6Coro *)mco_get_user_data(co);
    w->fn(w->arg);
}

Mp6Coro *mp6_coro_create(void (*fn)(void *), void *arg, void *stack, size_t stack_size)
{
    Mp6Coro *w;
    mco_desc desc;
    mco_result res;
    int slot;

    (void)stack; /* arena backend: the stack comes from the low-4GB pool,
                  * not caller storage (process_native.c passes NULL). */

    if (!g_poolInit) {
        mp6_coro_pool_init();
    }
    if (g_freeTop <= 0) {
        /* Backstop only: process_native.c's own MP6_MAX_PROC_FIBERS=128 cap
         * fires first (same number), so this preserves -- not tightens --
         * that existing concurrency limit. */
        fprintf(stderr, "[FATAL] coro_arena: too many concurrent HuPrc coroutines (max %d)\n",
                MP6_CORO_MAX_SLOTS);
        fflush(stderr);
        exit(1);
    }
    slot = g_freeList[--g_freeTop];

    desc = mco_desc_init(mp6_coro_entry, stack_size ? stack_size : MP6_CORO_STACK_SIZE);
    desc.alloc_cb = mp6_coro_slot_alloc;
    desc.dealloc_cb = mp6_coro_slot_dealloc;
    desc.allocator_data = g_pool + (size_t)slot * (size_t)MP6_CORO_SLOT_SIZE;

    w = &g_wrappers[slot];
    w->fn = fn;
    w->arg = arg;
    w->slot = slot;
    w->co = NULL;
    desc.user_data = w;

    if (desc.coro_size > MP6_CORO_SLOT_SIZE) {
        fprintf(stderr, "[FATAL] coro_arena: minicoro block %zu bytes exceeds slot %zu (stack_size=%zu) "
                        "-- widen MP6_CORO_SLOT_SIZE\n",
                (size_t)desc.coro_size, (size_t)MP6_CORO_SLOT_SIZE, (size_t)stack_size);
        fflush(stderr);
        exit(1);
    }

    res = mco_create(&w->co, &desc);
    if (res != MCO_SUCCESS || !w->co) {
        /* Mirrors the fiber backend's fatal-on-create contract (host.h): on
         * this platform mp6_coro_create never returns NULL. */
        fprintf(stderr, "[FATAL] coro_arena: mco_create failed (%s)\n", mco_result_description(res));
        fflush(stderr);
        exit(1);
    }

    g_slotsInUse++;
    if (g_slotsInUse > g_slotsHighWater) {
        g_slotsHighWater = g_slotsInUse;
    }
    return w;
}

void mp6_coro_switch(Mp6Coro *to)
{
    if (to) {
        /* Dispatcher side: run `to` until it yields back. minicoro records
         * the current running coroutine (NULL on the main/dispatcher thread)
         * as `to`'s resumer, so the matching mco_yield below returns here --
         * the automatic equivalent of the fiber backend's saved-dispatcher
         * nesting (process_native.c never nests dispatch, but this is
         * correct if it ever did). */
        mco_resume(to->co);
    } else {
        /* Process side (YieldToDispatcher): yield straight back to whoever
         * resumed this coroutine. Full stack state is preserved; the next
         * mp6_coro_switch(this) resumes exactly here. */
        mco_yield(mco_running());
    }
}

void mp6_coro_destroy(Mp6Coro *c)
{
    if (!c) {
        return;
    }
    /* Both call sites (process_native.c) destroy only a SUSPENDED
     * coroutine from the dispatcher context, never the running one -- same
     * rule the fiber backend's DeleteFiber had; mco_destroy enforces it. */
    if (c->co) {
        mco_destroy(c->co);
        c->co = NULL;
    }
    if (c->slot >= 0 && c->slot < MP6_CORO_MAX_SLOTS) {
        g_freeList[g_freeTop++] = (int16_t)c->slot;
        c->slot = -1;
        g_slotsInUse--;
    }
}

#endif /* !MP6_CORO_FIBERS */
