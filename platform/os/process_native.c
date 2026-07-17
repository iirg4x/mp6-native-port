/* MP6 native port -- HuPrc process scheduler, native reimplementation.
 * Replaces src/game/process.c (skipped from the decomp unit set, along
 * with kerent.c/jmp.c/malloc.c).
 *
 * Why process.c is replaced rather than compiled as-is: its HuPrcCall
 * implements the scheduling loop around ONE MORE jmp_buf besides each
 * process's own -- a single `processjmpbuf` representing "hand control
 * back to the dispatch loop" -- via `ret = gcsetjmp(&processjmpbuf)` at
 * the top of a `while(1)`, repeatedly re-armed by every
 * HuPrcSleep/HuPrcChildWatch/process-termination in the whole game. On
 * real hardware, gcsetjmp/gclongjmp are raw PPC asm (manual sp/lr
 * juggling) with no notion of "the calling function must still be on the
 * stack" -- they just poke registers. Host C's setjmp/longjmp does NOT
 * have that property: by the time any process yields, HuPrcCall's own
 * dispatch call has ALREADY pushed new stack frames reusing the exact
 * memory gcsetjmp's (also-already-returned) call chain occupied, so the
 * "return again, later, with a new value" trick is undefined behavior
 * (C11 7.13.2.1p3) -- and on this toolchain (zig cc / mingw-w64 x86-64)
 * it doesn't even fail loudly: it silently corrupts control flow (plain
 * setjmp/longjmp misjumps into unrelated code; __builtin_setjmp/longjmp
 * silently no-ops; a manual stack-snapshot-and-restore around the pair
 * still hangs). Stackful coroutines handle PER-PROCESS jump bufs
 * perfectly (a process is only ever "resumed exactly where it yielded")
 * but cannot express processjmpbuf's fundamentally different "same
 * statement, many logical re-entries, must see the RIGHT return value"
 * contract.
 *
 * The design: don't make host setjmp/longjmp (or anything imitating it)
 * stand in for processjmpbuf at all. Reimplement HuPrcCall's own loop
 * natively -- verified line-by-line against src/game/process.c, which
 * remains the single source of truth for OBSERVABLE BEHAVIOR (canary
 * check byte value, priority-sorted insertion order, exec-state
 * transitions, sleep countdown, pause bits, ...) -- so the one place that
 * genuinely needs "dispatch a process and wait for it to yield" becomes a
 * PLAIN, ORDINARY (blocking, from the caller's point of view) C function
 * call (DispatchProcessAndWait), returning the yielded status as a real
 * return value. No jmp_buf stands in for "the scheduler's own resume
 * point" anymore, so there's nothing for the stack-reuse problem to bite.
 * Each PROCESS gets its own stackful coroutine (created lazily on first
 * dispatch) -- a coroutine yielding is always "resumed exactly where it
 * last switched away", which is precisely what
 * HuPrcSleep/HuPrcChildWatch/HuPrcEnd need.
 *
 * game/process.h's HUPROCESS struct layout is completely UNCHANGED --
 * other decomp files (game/objmain.c, src/REL/*) read/write
 * ->property/->destructor/->child/->parent/->stat/->prio directly, so
 * those fields keep meaning exactly what they always did. Only .jump's
 * INTERNAL role changes (the native coroutine bootstrap reads .lr once,
 * on first dispatch; .sp is unused -- each coroutine has its own stack).
 */
#include "dolphin.h"
#include "game/process.h"
#include "game/memory.h"
#include "host.h" /* mp6_coro_* context backend + mp6_host_sleep_ns; this
                   * file owns the ENTIRE scheduler (dispatch loop, yield
                   * status, slot-reuse table, HUPROCESS observables) --
                   * the backends only switch contexts. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FAKE_RETADDR 0xA5A5A5A5
#define DEFAULT_STACK_SIZE 4096
#define MP6_FIBER_STACK_SIZE (1u * 1024u * 1024u)
#define MP6_MAX_PROC_FIBERS 128

static HUPROCESS *processtop;
static HUPROCESS *processcur;
static u16 processcnt;
u32 procfunc; /* matches the original's external linkage; nothing else in
               * the slice reads it (verified via grep), kept for parity. */

/* ------------------------------------------------------------------
 * Per-process coroutine bootstrap.
 * ------------------------------------------------------------------ */
typedef struct ProcFiber_s {
    HUPROCESS *process; /* identity key */
    Mp6Coro *coro;       /* host coroutine context; NULL until first dispatch */
} ProcFiber;

static ProcFiber g_procFibers[MP6_MAX_PROC_FIBERS];
static int g_procFiberCount;
static int g_yieldStatus;       /* status the current process yielded with */
/* (The dispatcher-context pointer -- including DispatchProcessAndWait's
 * save/restore nesting discipline -- lives inside mp6_coro_switch,
 * platform/host/.) */

/* A terminated process's slot is marked free by setting .process = NULL
 * (see DispatchProcessAndWait's own comment for why) -- search for one of
 * those to reuse BEFORE growing the table, both to avoid slowly
 * exhausting MP6_MAX_PROC_FIBERS over a long play session (short-lived
 * one-shot processes, e.g. REL/bootDll/opening.c's EventCreate/EventExec,
 * are created and destroyed constantly) and so a genuinely fresh
 * mp6_coro_create() happens for the reused slot (.coro is reset to NULL
 * too), never a stale one. */
static ProcFiber *find_or_create_proc_fiber(HUPROCESS *process)
{
    int i;
    int freeSlot = -1;
    for (i = 0; i < g_procFiberCount; i++) {
        if (g_procFibers[i].process == process) return &g_procFibers[i];
        if (g_procFibers[i].process == NULL && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot >= 0) {
        g_procFibers[freeSlot].process = process;
        g_procFibers[freeSlot].coro = NULL;
        return &g_procFibers[freeSlot];
    }
    if (g_procFiberCount >= MP6_MAX_PROC_FIBERS) {
        fprintf(stderr, "[FATAL] process_native: too many concurrent HuPrc fibers (max %d)\n", MP6_MAX_PROC_FIBERS);
        exit(1);
    }
    {
        ProcFiber *pf = &g_procFibers[g_procFiberCount++];
        pf->process = process;
        pf->coro = NULL;
        return pf;
    }
}

/* Lookup-only sibling of find_or_create_proc_fiber above -- used by
 * ForceTerminateKilledProcess below, which must NOT create a fresh
 * coroutine slot just to immediately discard it for a process that may
 * never have been dispatched at all yet. */
static ProcFiber *find_proc_fiber_if_exists(HUPROCESS *process)
{
    int i;
    for (i = 0; i < g_procFiberCount; i++) {
        if (g_procFibers[i].process == process) {
            return &g_procFibers[i];
        }
    }
    return NULL;
}

static void ProcessTrampoline(void *param)
{
    HUPROCESS *process = (HUPROCESS *)param;
    void (*entry)(void) = (void (*)(void))(uintptr_t)(uint32_t)process->jump.lr;

    if (!entry) {
        fprintf(stderr,
                "[FATAL] process_native: HuPrc process dispatched with a NULL entry "
                "function (process=%p) -- exiting cleanly instead of segfaulting.\n",
                (void *)process);
        fflush(stderr);
        exit(1);
    }

    entry();

    fprintf(stderr, "[FATAL] process_native: a HuPrc process function returned instead of "
                    "calling HuPrcEnd() -- unsupported\n");
    exit(1);
}

/* Dispatches `process`; BLOCKS (an ordinary, synchronous call from
 * HuPrcCall's point of view) until it yields or terminates, and returns
 * the status it yielded with (1 = normal yield, 2 = terminated). This is
 * the direct replacement for the broken gclongjmp(&process->jump, 1) ->
 * (eventually) gclongjmp(&processjmpbuf, N) round trip. */
static int DispatchProcessAndWait(HUPROCESS *process)
{
    ProcFiber *pf = find_or_create_proc_fiber(process);

    if (!pf->coro) {
        /* Creation failure is FATAL inside the backend -- it prints and
         * exits(1) itself (host.h's mp6_coro_create contract), so this
         * never returns NULL. stack=NULL: the backend supplies the stack
         * (arena pool by default; OS-managed in the fiber fallback). */
        pf->coro = mp6_coro_create(ProcessTrampoline, process, NULL, MP6_FIBER_STACK_SIZE);
    }
    mp6_coro_switch(pf->coro);
    /* Resumes here once `process` calls YieldToDispatcher (below), which
     * runs on `process`'s own context and switches straight back to
     * whichever context is CURRENTLY the dispatcher -- always this
     * exact call, since dispatch nests strictly (a process's own
     * HuPrcChildWatch dispatches ITS children by HuPrcCall being called
     * recursively... actually HuPrcCall itself is never reentered by
     * game code; each process yields back to the ONE scheduler loop).
     * The dispatcher-pointer save/restore for this switch lives inside
     * mp6_coro_switch itself (platform/host/), not here. */

    if (g_yieldStatus == 2) {
        /* Process TERMINATED (gcTerminateProcess -> YieldToDispatcher(2))
         * -- its coroutine is now permanently parked at that exact yield
         * point and must never be resumed again. Tear it down now (safe
         * here: we're back on the DISPATCHER's context, not the dying
         * one) and free its table slot (.process = NULL, matched by
         * find_or_create_proc_fiber's own free-slot search above).
         *
         * The slot MUST be freed eagerly, not lazily: HuPrcCall's own
         * case-2 handling (one caller up) does
         * HuMemDirectFree(processcur->heap) -- which frees the SAME heap
         * block the HUPROCESS struct itself lives in (see HuPrcCreate) --
         * immediately freeing that exact address for reuse by the NEXT
         * HuMemDirectMalloc of a similar size. One-shot "run this
         * callback as its own HuPrc process" helpers (e.g.
         * REL/bootDll/opening.c's EventCreate/EventExec) create and
         * destroy many same-sized processes in quick succession, making
         * this address reuse routine, not a rare edge case. With a stale
         * table entry, a NEW process allocated at the OLD process's
         * just-freed address would match that entry and resume the
         * ALREADY-DEAD coroutine instead of getting a fresh one -- which
         * resumes mid-way through the OLD process's own teardown call
         * chain, never calls the new process's real entry function at
         * all, and eventually falls back into ProcessTrampoline's
         * `entry();` return path, tripping its "returned instead of
         * calling HuPrcEnd()" FATAL. */
        mp6_coro_destroy(pf->coro);
        pf->coro = NULL;
        pf->process = NULL;
    }

    return g_yieldStatus;
}

/* Called BY a process's own code (via HuPrcSleep/HuPrcChildWatch/
 * gcTerminateProcess below) to yield back to whoever dispatched it. Plain
 * fiber switch -- no jmp_buf, so none of the stack-reuse problems the
 * file header comment describes can apply here. */
static void YieldToDispatcher(int status)
{
    g_yieldStatus = status;
    mp6_coro_switch(NULL); /* NULL = back to the dispatcher (host.h) */
    /* Resumes here, exactly where it left off (full stack state intact --
     * that's what the coro backend guarantees), the next time THIS
     * process is dispatched again. */
}

/* ------------------------------------------------------------------
 * Faithful reimplementation of process.c's public API (game/process.h).
 * Behavior verified line-by-line against the decomp (read-only reference,
 * unchanged src/game/process.c between the pinned cd3642f and current
 * 8c93902 -- checked via `git diff`). Every function keeps the EXACT same
 * observable behavior except HuPrcCall/HuPrcSleep/HuPrcChildWatch/
 * gcTerminateProcess's internal yield mechanism (see above).
 * ------------------------------------------------------------------ */

void HuPrcInit(void)
{
    processcnt = 0;
    processtop = NULL;
}

static void LinkProcess(HUPROCESS **root, HUPROCESS *process)
{
    HUPROCESS *src_process = *root;

    if (src_process && (src_process->prio >= process->prio)) {
        while (src_process->next && src_process->next->prio >= process->prio) {
            src_process = src_process->next;
        }
        process->next = src_process->next;
        process->prev = src_process;
        src_process->next = process;
        if (process->next) {
            process->next->prev = process;
        }
    } else {
        process->next = (*root);
        process->prev = NULL;
        *root = process;
        if (src_process) {
            src_process->prev = process;
        }
    }
}

static void UnlinkProcess(HUPROCESS **root, HUPROCESS *process)
{
    if (process->next) {
        process->next->prev = process->prev;
    }
    if (process->prev) {
        process->prev->next = process->next;
    } else {
        *root = process->next;
    }
}

HUPROCESS *HuPrcCreate(void (*func)(void), u16 prio, u32 stackSize, s32 heapSize)
{
    HUPROCESS *process;
    s32 allocSize;
    void *heap;
    if (stackSize == 0) {
        stackSize = DEFAULT_STACK_SIZE;
    }
    allocSize = HuMemMemoryAllocSizeGet(sizeof(HUPROCESS))
                    + HuMemMemoryAllocSizeGet(stackSize)
                    + HuMemMemoryAllocSizeGet(heapSize);
    if (!(heap = HuMemDirectMalloc(HEAP_HEAP, allocSize))) {
        OSReport("process> malloc error size %d\n", allocSize);
        return NULL;
    }
    printf("[HEAPTRACE] HuPrcCreate: nested HuMemHeapInit(heap=%p, allocSize=%d) "
           "[sizeof(HUPROCESS)=%zu stackSize=%u heapSize=%d]\n",
           heap, allocSize, sizeof(HUPROCESS), (unsigned)stackSize, (int)heapSize);
    fflush(stdout);
    HuMemHeapInit(heap, allocSize);
    process = HuMemMemoryAlloc(heap, sizeof(HUPROCESS), FAKE_RETADDR);
    process->heap = heap;
    process->exec = HUPRC_EXEC_NORMAL;
    process->stat = 0;
    process->prio = prio;
    process->sleep = 0;
    process->spBase = ((u32)(uintptr_t)HuMemMemoryAlloc(heap, stackSize, FAKE_RETADDR)) + stackSize - 8;
    /* The native coroutine bootstrap (ProcessTrampoline) reads ONLY .lr,
     * on this process's first dispatch; .sp is unused (each coroutine has
     * its own stack). No throwaway gcsetjmp needed: we just set .lr. */
    process->jump.lr = (u32)(uintptr_t)func;
    process->jump.sp = process->spBase;
    process->destructor = NULL;
    process->property = NULL;
    LinkProcess(&processtop, process);
    process->child = NULL;
    process->parent = NULL;
    processcnt++;
    printf("[PRC] HuPrcCreate: func=%p prio=%u -> process=%p\n", (void *)func, (unsigned)prio, (void *)process);
    fflush(stdout);
    return process;
}

void HuPrcChildLink(HUPROCESS *parent, HUPROCESS *child)
{
    HuPrcChildUnlink(child);
    if (parent->child) {
        parent->child->firstChild = child;
    }
    child->nextChild = parent->child;
    child->firstChild = NULL;
    parent->child = child;
    child->parent = parent;
}

void HuPrcChildUnlink(HUPROCESS *process)
{
    if (process->parent) {
        if (process->nextChild) {
            process->nextChild->firstChild = process->firstChild;
        }
        if (process->firstChild) {
            process->firstChild->nextChild = process->nextChild;
        } else {
            process->parent->child = process->nextChild;
        }
        process->parent = NULL;
    }
}

HUPROCESS *HuPrcChildCreate(void (*func)(void), u16 prio, u32 stackSize, s32 heapSize, HUPROCESS *parent)
{
    HUPROCESS *child = HuPrcCreate(func, prio, stackSize, heapSize);
    HuPrcChildLink(parent, child);
    printf("[PRC] HuPrcChildCreate: parent=%p -> child=%p\n", (void *)parent, (void *)child);
    fflush(stdout);
    return child;
}

void HuPrcChildWatch(void)
{
    HUPROCESS *curr = HuPrcCurrentGet();
    if (curr->child) {
        curr->exec = HUPRC_EXEC_CHILDWATCH;
        YieldToDispatcher(1);
    }
}

HUPROCESS *HuPrcCurrentGet(void)
{
    return processcur;
}

static s32 SetKillStatusProcess(HUPROCESS *process)
{
    if (process->exec != HUPRC_EXEC_KILLED) {
        HuPrcWakeup(process);
        process->exec = HUPRC_EXEC_KILLED;
        return 0;
    } else {
        return -1;
    }
}

s32 HuPrcKill(HUPROCESS *process)
{
    if (process == NULL) {
        process = HuPrcCurrentGet();
    }
    HuPrcChildKill(process);
    HuPrcChildUnlink(process);
    return SetKillStatusProcess(process);
}

void HuPrcChildKill(HUPROCESS *process)
{
    HUPROCESS *child = process->child;
    while (child) {
        if (child->child) {
            HuPrcChildKill(child);
        }
        SetKillStatusProcess(child);
        child = child->nextChild;
    }
    process->child = NULL;
}

static void gcTerminateProcess(HUPROCESS *process)
{
    if (process->destructor) {
        process->destructor();
    }
    UnlinkProcess(&processtop, process);
    processcnt--;
    YieldToDispatcher(2);
}

void HuPrcEnd(void)
{
    HUPROCESS *process = HuPrcCurrentGet();
    HuPrcChildKill(process);
    HuPrcChildUnlink(process);
    gcTerminateProcess(process);
}

void HuPrcSleep(s32 time)
{
    HUPROCESS *process = HuPrcCurrentGet();
    if (time != 0 && process->exec != HUPRC_EXEC_KILLED) {
        process->exec = HUPRC_EXEC_SLEEP;
        process->sleep = time;
    }
    YieldToDispatcher(1);
}

void HuPrcVSleep(void)
{
    HuPrcSleep(0);
}

void HuPrcWakeup(HUPROCESS *process)
{
    process->sleep = 0;
}

void HuPrcDestructorSet2(HUPROCESS *process, void (*func)(void))
{
    process->destructor = func;
}

void HuPrcDestructorSet(void (*func)(void))
{
    HUPROCESS *process = HuPrcCurrentGet();
    process->destructor = func;
}

/* Forces a KILLED-but-not-yet-terminated process straight to termination
 * WITHOUT ever resuming its own stale suspended coroutine state.
 *
 * Real hardware's raw PowerPC sp/lr-poking scheduler (this file's header
 * comment) can unconditionally redirect a killed process's NEXT dispatch
 * straight into HuPrcEnd() by simply overwriting the saved link register
 * -- gclongjmp there always honors whatever `.jump.lr` currently holds,
 * on EVERY resume, not just the first. Stackful coroutines cannot do
 * this: a resume always continues a suspended coroutine exactly where it
 * last yielded, full stop -- there is no way to redirect it elsewhere
 * short of never switching back into it. The original shape (KILLED sets
 * process->jump.lr to HuPrcEnd, then falls through into the SAME dispatch
 * call HUPRC_EXEC_NORMAL uses) is still correct for a process's OWN
 * first-ever dispatch, which legitimately reads .jump.lr as its entry
 * point in ProcessTrampoline -- but silently does nothing useful for a
 * KILLED process that was already dispatched at least once before: its
 * coroutine resumes exactly where IT last called HuPrcVSleep/HuPrcSleep,
 * completely unaware it was ever killed, and keeps executing its own
 * remaining scripted statements for one more slice -- which can reference
 * resources the KILLER already tore down in the very same tick (e.g. a
 * parent that calls HuPrcKill(eventProcess) and then immediately frees
 * the windows that process's script still touches, with no yield in
 * between -- the killed process's next resume then indexes a freed
 * HUSPR_GROUP). This function closes that gap by doing exactly what a
 * real, already-executing `HuPrcEnd()` call would do
 * (HuPrcChildKill/HuPrcChildUnlink/unlink-from-processtop/processcnt--)
 * directly on the DISPATCHER's own context, then discards the killed
 * process's coroutine (if one was ever created) instead of ever switching
 * back into its stale suspended state -- functionally identical to real
 * hardware's immediate redirect, achieved by skipping the resume entirely
 * rather than retargeting it. */
static void ForceTerminateKilledProcess(HUPROCESS *process)
{
    ProcFiber *pf;
    if (process->destructor) {
        process->destructor();
    }
    HuPrcChildKill(process);
    HuPrcChildUnlink(process);
    UnlinkProcess(&processtop, process);
    processcnt--;
    pf = find_proc_fiber_if_exists(process);
    if (pf && pf->coro) {
        mp6_coro_destroy(pf->coro);
        pf->coro = NULL;
        pf->process = NULL;
    }
}

void HuPrcCall(s32 tick)
{
    HUPROCESS *process;
    s32 ret;
    processcur = processtop;
    ret = 0; /* was `gcsetjmp(&processjmpbuf)` -- always 0 on the one, direct
              * call HuPrcCall itself ever makes; every LATER update to
              * `ret` below is a plain assignment or DispatchProcessAndWait's
              * real return value -- see file header comment. */
    while (1) {
        switch (ret) {
            case 2:
                HuMemDirectFree(processcur->heap);
                /* fallthrough */
            case 1:
                if (((u8 *)(processcur->heap))[4] != 165) {
                    fprintf(stderr, "stack overlap error.(process pointer %p)\n", (void *)processcur);
                    fflush(stderr);
                    for (;;) mp6_host_sleep_ns(1000ull * 1000000ull); /* portable stall, not Win32 Sleep() */
                } else {
                    processcur = processcur->next;
                }
                break;
        }
        process = processcur;
        if (!process) {
            return;
        }
        procfunc = process->jump.lr;
        if ((process->stat & (HU_PRC_STAT_PAUSE | HU_PRC_STAT_UPAUSE)) && process->exec != HUPRC_EXEC_KILLED) {
            ret = 1;
            continue;
        }
        switch (process->exec) {
            case HUPRC_EXEC_SLEEP:
                if (process->sleep > 0) {
                    process->sleep -= tick;
                    if (process->sleep <= 0) {
                        process->sleep = 0;
                        process->exec = HUPRC_EXEC_NORMAL;
                    }
                }
                ret = 1;
                break;

            case HUPRC_EXEC_CHILDWATCH:
                if (process->child) {
                    ret = 1;
                } else {
                    process->exec = HUPRC_EXEC_NORMAL;
                    ret = 0;
                }
                break;

            case HUPRC_EXEC_KILLED:
                /* See ForceTerminateKilledProcess's comment above -- a
                 * process already dispatched at least once before being
                 * killed must NOT be resumed again (its stale coroutine
                 * would just keep running its own remaining script,
                 * unaware it was killed); one that's never been dispatched
                 * yet still needs its first-ever entry redirected to
                 * HuPrcEnd, which ForceTerminateKilledProcess also
                 * achieves directly (no coroutine exists yet to discard in
                 * that case -- harmless). */
                ForceTerminateKilledProcess(process);
                ret = 2;
                break;

            case HUPRC_EXEC_NORMAL:
                ret = DispatchProcessAndWait(process);
                break;
        }
    }
}

void *HuPrcMemAlloc(s32 size)
{
    HUPROCESS *process = HuPrcCurrentGet();
    return HuMemMemoryAlloc(process->heap, size, FAKE_RETADDR);
}

void HuPrcMemFree(void *ptr)
{
    HuMemMemoryFree(ptr, FAKE_RETADDR);
}

void HuPrcSetStat(HUPROCESS *process, u16 value)
{
    process->stat |= value;
}

void HuPrcResetStat(HUPROCESS *process, u16 value)
{
    process->stat &= ~value;
}

void HuPrcAllPause(s32 flag)
{
    HUPROCESS *process = processtop;
    if (flag) {
        while (process != NULL) {
            if (!(process->stat & HU_PRC_STAT_PAUSE_ON)) {
                HuPrcSetStat(process, HU_PRC_STAT_PAUSE);
            }
            process = process->next;
        }
    } else {
        while (process != NULL) {
            if (process->stat & HU_PRC_STAT_PAUSE) {
                HuPrcResetStat(process, HU_PRC_STAT_PAUSE);
            }
            process = process->next;
        }
    }
}

void HuPrcAllUPause(s32 flag)
{
    HUPROCESS *process = processtop;
    if (flag) {
        while (process != NULL) {
            if (!(process->stat & HU_PRC_STAT_UPAUSE_ON)) {
                HuPrcSetStat(process, HU_PRC_STAT_UPAUSE);
            }
            process = process->next;
        }
    } else {
        while (process != NULL) {
            if (process->stat & HU_PRC_STAT_UPAUSE) {
                HuPrcResetStat(process, HU_PRC_STAT_UPAUSE);
            }
            process = process->next;
        }
    }
}
