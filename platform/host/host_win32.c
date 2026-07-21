/* MP6 native port -- the win32 host backend (see host.h for the seam
 * contract each function implements).
 *
 * Compiled into BOTH build modes (tools/build.py PLATFORM_SOURCES_COMMON);
 * links against kernel32/dbghelp/psapi only.
 *
 * This file is the ONLY place under platform/** that may include
 * windows.h, with one documented exception: platform/gx/aurora_bridge.c's
 * console-repositioning nicety -- win32-only UI policy with no portable
 * meaning, kept in the one aurora-flavor TU that owns the window.
 */
#include "host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp6_boot.h" /* mp6_symbolize_addr's own declaration (crash section below) */

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md). Placing this AFTER this TU's own
 * includes is load-bearing, not stylistic: it is a #pragma clang section that
 * redirects every file-scope definition FOLLOWING it. As a -include (before all
 * headers) it also captured the decomp headers' C TENTATIVE definitions --
 * dolphin/os.h:27 `u32 __OSBusClock;` and friends -- turning those common
 * symbols into strong per-TU definitions and breaking the link with duplicate
 * symbol errors. Here, headers keep their normal linkage and only this file's
 * own statics move. tools/build.py asserts this line exists in every TU listed
 * in HOST_STATE_SECTION_SOURCES. */
#include "mp6_host_section.h"


#pragma comment(lib, "dbghelp.lib")

/* =======================================================================
 * Lifecycle: mp6_host_init -- OS timer-resolution push
 * Sleep() has ~1-2ms wakeup slop at default resolution;
 * timeBeginPeriod(1) tightens it for the tick throttle's sleep band.
 * timeBeginPeriod/timeEndPeriod live in winmm.dll, which neither build
 * mode's link set includes -- resolved at runtime via
 * LoadLibraryA/GetProcAddress instead of an import-table entry, degrading
 * gracefully (callers still work, just with coarser sleeps) if winmm.dll
 * is ever missing. The library is deliberately never FreeLibrary'd: the
 * raised timer resolution is process-lifetime by design. The atexit
 * teardown covers every deliberate shutdown path (they all go through
 * exit()), and an abnormal termination reverts timer resolution
 * automatically anyway (it is process-scoped).
 * ======================================================================= */

typedef UINT (WINAPI *MP6TimePeriodFn)(UINT); /* timeBeginPeriod/timeEndPeriod shape (MMRESULT == UINT) */
static MP6TimePeriodFn g_timeEndPeriodFn = NULL;
static bool            g_timePeriodRaised = false;

static void mp6_host_timer_res_atexit(void)
{
    if (g_timePeriodRaised && g_timeEndPeriodFn) {
        g_timeEndPeriodFn(1);
        g_timePeriodRaised = false;
    }
}

int mp6_host_init(void)
{
    static int s_done = 0;
    if (s_done) {
        return g_timePeriodRaised ? 1 : 0; /* idempotent -- first call's result */
    }
    s_done = 1;
    {
        HMODULE winmm = LoadLibraryA("winmm.dll");
        if (winmm) {
            MP6TimePeriodFn beginFn = (MP6TimePeriodFn)(void *)GetProcAddress(winmm, "timeBeginPeriod");
            g_timeEndPeriodFn = (MP6TimePeriodFn)(void *)GetProcAddress(winmm, "timeEndPeriod");
            if (beginFn && beginFn(1) == 0 /* TIMERR_NOERROR */) {
                g_timePeriodRaised = true;
                atexit(mp6_host_timer_res_atexit);
            }
        }
    }
    return g_timePeriodRaised ? 1 : 0;
}

/* =======================================================================
 * Time: mp6_host_monotonic_ns / mp6_host_sleep_ns / mp6_host_wallclock
 * QPC/Sleep/GetLocalTime -- the primitives behind aurora_bridge.c's tick
 * throttle and shims_manual.c's OSGetTime timebase. The QPC->ns scaling
 * is exact integer math (seconds + remainder split), so the result is
 * monotone and free of double-rounding artifacts; consumers scale from ns
 * with a fixed 1e9 timebase instead of the machine's QPC frequency.
 * ======================================================================= */

uint64_t mp6_host_monotonic_ns(void)
{
    static LARGE_INTEGER s_freq;
    static int s_freqInit;
    LARGE_INTEGER now;
    if (!s_freqInit) {
        QueryPerformanceFrequency(&s_freq); /* cannot fail on XP+ */
        s_freqInit = 1;
    }
    QueryPerformanceCounter(&now);
    {
        uint64_t c = (uint64_t)now.QuadPart;
        uint64_t f = (uint64_t)s_freq.QuadPart;
        /* (c%f) < f <= a few GHz, so (c%f)*1e9 fits u64 comfortably. */
        return (c / f) * 1000000000ull + (c % f) * 1000000000ull / f;
    }
}

void mp6_host_sleep_ns(uint64_t ns)
{
    /* Thin primitive by contract (host.h): sub-ms (including 0) becomes
     * Sleep(0) -- "yield to any ready thread". */
    Sleep((DWORD)(ns / 1000000ull));
}

void mp6_host_wallclock(Mp6DateTime *out)
{
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    out->year = lt.wYear;
    out->mon  = lt.wMonth;
    out->day  = lt.wDay;
    out->hour = lt.wHour;
    out->min  = lt.wMinute;
    out->sec  = lt.wSecond;
    out->msec = lt.wMilliseconds;
}

/* =======================================================================
 * Memory: mp6_host_arena_reserve / mp6_host_image_below_4gb
 * Two callers reserve low memory through mp6_host_arena_reserve: the game
 * arena (platform/os/arena.c, which keeps the FATAL/exit policy and the
 * [BOOT] banner) and the coroutine stack pool
 * (platform/host/coro_arena.c). They are separate reservations; either
 * can fall back to an OS-picked address independently, which is why the
 * [WARN] below names this function rather than either caller.
 * ======================================================================= */

void *mp6_host_arena_reserve(size_t size)
{
    /* 0x8xxxxxxx first, matching real GameCube MEM1's 0x80000000 base:
     * game/memory.c's allocator sanity-checks every block pointer with
     * BLOCK_CHECK_BROKEN(block) = `((u32)block->next & 0x80000000) == 0`
     * -- "high bit set" is real hardware's cheap stand-in for "looks like
     * a valid pointer" (ALL of MEM1 has it set). An arena below
     * 0x80000000 (0x4-0x7xxxxxxx, tried further down as a fallback) makes
     * EVERY valid allocation fail that check, spamming "Error: memory
     * chain broken!" (harmless on its own -- just an OSReport -- but code
     * paths that branch on it can misbehave for real). */
    static const uintptr_t kCandidateBases[] = {
        0x80000000u, 0x90000000u, 0xA0000000u, 0xB0000000u,
        0x40000000u, 0x50000000u, 0x60000000u, 0x70000000u, 0x20000000u, 0x30000000u,
    };
    size_t i;
    void *got = NULL;

    for (i = 0; i < sizeof(kCandidateBases) / sizeof(kCandidateBases[0]); i++) {
        void *want = (void *)kCandidateBases[i];
        got = VirtualAlloc(want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (got) break;
    }
    if (!got) {
        got = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!got) {
            return NULL; /* caller (arena.c) keeps its own FATAL+exit policy */
        }
        fprintf(stderr,
                "[WARN] mp6_host_arena_reserve: no candidate low(<4GB) base free; OS picked %p -- "
                "u32 pointer round-trips may break if this is >=4GB\n",
                got);
    }
    return got;
}

int mp6_host_image_below_4gb(void)
{
    /* Same GetModuleHandle/GetModuleInformation pair the crash section
     * below uses for the module range. */
    HMODULE hMod = GetModuleHandle(NULL);
    MODULEINFO modInfo;
    memset(&modInfo, 0, sizeof(modInfo));
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &modInfo, sizeof(modInfo))) {
        return 0; /* cannot prove the invariant */
    }
    return ((uintptr_t)hMod + modInfo.SizeOfImage) <= 0xFFFFFFFFu;
}

/* Savestate (docs/SAVESTATE.md): enumerate this image's own WRITABLE
 * sections -- the game's ordinary globals/BSS, which live in the binary's
 * fixed, non-ASLR data segments rather than inside the game arena, and so
 * have to be captured alongside it.
 *
 * Walks the in-memory PE headers rather than re-reading the file on disk:
 * `GetModuleHandle(NULL)` already returns the mapped IMAGE_DOS_HEADER, and
 * every struct/macro used below comes from the windows.h this file already
 * includes (it is the only TU in the tree permitted to -- see this file's
 * own header comment), so no new library or dependency is introduced.
 *
 * Two details that are easy to get wrong, stated explicitly:
 *  - the extent is `Misc.VirtualSize`, NOT `SizeOfRawData`: an uninitialized
 *    (.bss-style) tail exists only in memory and has VirtualSize >
 *    SizeOfRawData, so using the raw size would silently truncate exactly
 *    the zero-initialized globals a savestate most needs;
 *  - DISCARDABLE sections are skipped -- they may already have been unmapped
 *    by the loader, so touching them is a fault, not merely wasteful.
 *
 * The host-owned carve-out section (MP6_HOST_STATE_SECTION, see
 * shim/include/mp6_savestate.h) is deliberately NOT filtered here: this
 * function reports what the image contains, and the savestate module itself
 * decides what to exclude by name, keeping that policy in one place.
 *
 * Contract matches mp6_host_disc_root/mp6_host_save_dir: returns the number
 * of entries written (0 on query failure -- callers fail closed). */
int mp6_host_image_writable_sections(Mp6HostImageSection *out, int maxOut)
{
    HMODULE hMod;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int i, n = 0;

    if (out == NULL || maxOut <= 0) {
        return 0;
    }
    hMod = GetModuleHandle(NULL);
    if (hMod == NULL) {
        return 0;
    }
    dos = (IMAGE_DOS_HEADER *)hMod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    nt = (IMAGE_NT_HEADERS *)((BYTE *)hMod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < (int)nt->FileHeader.NumberOfSections && n < maxOut; i++) {
        DWORD ch = sec[i].Characteristics;
        if (!(ch & IMAGE_SCN_MEM_WRITE) || (ch & IMAGE_SCN_MEM_DISCARDABLE)) {
            continue;
        }
        if (sec[i].Misc.VirtualSize == 0) {
            continue;
        }
        memset(out[n].name, 0, sizeof(out[n].name));
        memcpy(out[n].name, sec[i].Name, 8); /* PE names are 8 bytes, NOT NUL-terminated when exactly 8 */
        out[n].addr = (void *)((BYTE *)hMod + sec[i].VirtualAddress);
        out[n].size = (size_t)sec[i].Misc.VirtualSize;
        n++;
    }
    return n;
}

/* =======================================================================
 * Paths & filesystem
 * ======================================================================= */

/* Resolve the disc tree relative to the RUNNING EXE'S OWN location, not
 * the process's cwd: a plain double-click (any cwd) and a move/rename of
 * the enclosing workspace tree are both real ways for the build-time -D
 * fallback paths to stop matching reality. This workspace's layout
 * (port/mp6-native/build/mp6native.exe with external_refs/repos/
 * marioparty6/orig/GP6E01/ a sibling of port/) is a FIXED relative offset
 * from the exe's own directory, so walking up from GetModuleFileNameA
 * (never cwd) and back down that fixed offset is robust to both. Returns
 * 0 only with the fst.bin probe passing, so the caller keeps its
 * build-time -D fallback otherwise. */
int mp6_host_disc_root(char *buf, size_t n)
{
    char exeDir[MAX_PATH];
    char candidateRoot[1024];
    char candidateFst[1024];
    DWORD len;
    char *lastSlash;
    char *p;
    FILE *probe;
    size_t rootLen;

    len = GetModuleFileNameA(NULL, exeDir, (DWORD)sizeof(exeDir));
    if (len == 0 || len >= sizeof(exeDir)) {
        return -1; /* GetModuleFileNameA failed/truncated -- caller keeps its fallback */
    }
    /* exeDir is ".../port/mp6-native/build/mp6native.exe" -- strip the
     * filename to get the exe's own directory. Handle both separators:
     * GetModuleFileNameA always returns '\\', but staying defensive here
     * costs nothing. */
    lastSlash = strrchr(exeDir, '\\');
    p = strrchr(exeDir, '/');
    if (p && (!lastSlash || p > lastSlash)) lastSlash = p;
    if (!lastSlash) return -1;
    *lastSlash = '\0';

    /* build/ -> mp6-native/ -> port/ -> workspace root, then back down to
     * external_refs/repos/marioparty6/orig/GP6E01 -- this workspace's
     * fixed layout, not a guess. Forward slashes work fine mixed with the
     * backslash-separated exeDir prefix on Windows. */
    snprintf(candidateRoot, sizeof(candidateRoot),
             "%s/../../../external_refs/repos/marioparty6/orig/GP6E01", exeDir);
    snprintf(candidateFst, sizeof(candidateFst), "%s/sys/fst.bin", candidateRoot);

    probe = fopen(candidateFst, "rb");
    if (!probe) {
        return -1; /* not this repo's layout -- caller keeps its fallback */
    }
    fclose(probe);

    rootLen = strlen(candidateRoot);
    if (rootLen + 1 > n) return -1;
    memcpy(buf, candidateRoot, rootLen + 1);
    return 0;
}

int mp6_host_save_dir(char *buf, size_t n)
{
    /* The cwd-relative "saves" -- see host.h for why this is deliberately
     * not "<exe>/saves" (existing save layouts are cwd-relative). */
    if (n < sizeof("saves")) return -1;
    memcpy(buf, "saves", sizeof("saves"));
    return 0;
}

int mp6_host_pref_dir(char *buf, size_t n)
{
    /* The cwd -- where every current diagnostic writer's relative fopen
     * already lands (see host.h: currently uncalled). */
    if (n < sizeof(".")) return -1;
    memcpy(buf, ".", sizeof("."));
    return 0;
}

int mp6_host_mkdir(const char *path)
{
    /* "Already exists" is success per the header contract (callers
     * ignore the result and rely on exactly that). */
    if (CreateDirectoryA(path, NULL)) return 0;
    return (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
}

/* =======================================================================
 * Coroutines -- the win32 FIBER backend (compile-time fallback).
 *
 * The default backend is arena-backed minicoro
 * (platform/host/coro_arena.c): fibers self-allocate an OS stack this
 * 64-bit build cannot place below 4 GB, so game code running on a fiber
 * stack would break the u32<->pointer round-trip for stack addresses.
 * This fiber backend is retained as a compile-time fallback / A-B lever:
 * built only under -DMP6_CORO_FIBERS, in which case coro_arena.c compiles
 * to nothing and this provides mp6_coro_* instead. Exactly one backend is
 * ever linked. process_native.c's scheduler (dispatch loop, yield status,
 * slot-reuse table, HUPROCESS observables) is identical for both backends
 * -- this is only the context-switch mechanism.
 * ======================================================================= */
#ifdef MP6_CORO_FIBERS

struct Mp6Coro {
    void *fiber;         /* Windows fiber handle */
    void (*fn)(void *);  /* entry, run on first switch-in */
    void *arg;
};

static int   g_threadIsFiber;
static void *g_dispatcherFiber; /* who to switch back to on mp6_coro_switch(NULL) */

static void WINAPI mp6_coro_fiber_entry(void *param)
{
    Mp6Coro *c = (Mp6Coro *)param;
    c->fn(c->arg);
    /* Never reached: every coro entry (process_native.c's
     * ProcessTrampoline) FATALs before returning. A fiber entry that
     * returned would end the whole THREAD (win32 rule), so there is
     * nothing graceful to do here anyway. */
}

Mp6Coro *mp6_coro_create(void (*fn)(void *), void *arg, void *stack, size_t stack_size)
{
    Mp6Coro *c;
    (void)stack; /* fiber backend: stacks are OS-managed (host.h contract);
                  * caller-provided storage is for the arena backend. */
    c = (Mp6Coro *)calloc(1, sizeof(*c));
    if (!c) {
        fprintf(stderr, "[FATAL] host_win32: out of memory allocating an Mp6Coro\n");
        exit(1);
    }
    c->fn = fn;
    c->arg = arg;
    c->fiber = CreateFiber(stack_size, mp6_coro_fiber_entry, c);
    if (!c->fiber) {
        /* The "process_native" prefix is deliberate: process creation is
         * the failing operation at the only call site. */
        fprintf(stderr, "[FATAL] process_native: CreateFiber failed (GetLastError=%lu)\n", GetLastError());
        exit(1);
    }
    return c;
}

void mp6_coro_switch(Mp6Coro *to)
{
    if (to) {
        /* Dispatcher side: remember the previous dispatcher pointer
         * (dispatch nests strictly; see process_native.c), lazily convert
         * this thread to a fiber once, publish ourselves as "the
         * dispatcher", run the target until it yields, restore. */
        void *savedDispatcher = g_dispatcherFiber;
        if (!g_threadIsFiber) {
            ConvertThreadToFiber(NULL);
            g_threadIsFiber = 1;
        }
        g_dispatcherFiber = GetCurrentFiber();
        SwitchToFiber(to->fiber);
        g_dispatcherFiber = savedDispatcher;
    } else {
        /* Process side: plain fiber switch straight back to whichever
         * context is CURRENTLY the dispatcher. Resumes exactly where it
         * left off (full stack state intact) the next time this coroutine
         * is switched to again. */
        SwitchToFiber(g_dispatcherFiber);
    }
}

void mp6_coro_destroy(Mp6Coro *c)
{
    if (!c) return;
    /* DeleteFiber on the fiber you're currently running on is invalid per
     * Win32 -- both call sites run on the dispatcher context and only ever
     * destroy a dormant coro (see process_native.c). */
    if (c->fiber) DeleteFiber(c->fiber);
    free(c);
}

/* Savestate accessors, fiber-backend edition: report "no capturable pool."
 * Fiber stacks are OS-placed (that is exactly why this backend is not the
 * default -- they are not guaranteed below 4 GB), so their addresses are
 * not reproducible across runs and a cross-session savestate cannot be
 * honest about them. Reporting an empty pool makes mp6_savestate_capture()
 * refuse up front with a clear message, rather than writing a state file
 * that would restore onto different stack addresses. Defined (not omitted)
 * because host.h is a shared seam -- see the same rule in host_android.c. */
void  *mp6_coro_pool_base(void) { return NULL; }
size_t mp6_coro_pool_size(void) { return 0; }
size_t mp6_coro_slot_size(void) { return 0; }
int    mp6_coro_slot_count(void) { return 0; }
int    mp6_coro_slot_in_use(int slot) { (void)slot; return 0; }
void  *mp6_coro_slot_addr(int slot) { (void)slot; return NULL; }

#endif /* MP6_CORO_FIBERS -- else platform/host/coro_arena.c provides mp6_coro_* */

/* =======================================================================
 * Mutex / threads
 * Mutexes: msm_bridge.c's CRITICAL_SECTION pair (g_mixerLock/g_grpLock)
 * behind caller-provided storage. Threads: the opt-in leak-stress
 * CreateThread sites, same detached process-lifetime shape.
 * ======================================================================= */

/* Mp6Mutex must be able to hold a CRITICAL_SECTION (40 bytes on x64). */
typedef char mp6_host_mutex_size_check[
    (sizeof(CRITICAL_SECTION) <= sizeof(((Mp6Mutex *)0)->opaque)) ? 1 : -1];

void mp6_host_mutex_init(Mp6Mutex *m)
{
    InitializeCriticalSection((CRITICAL_SECTION *)m->opaque);
}

void mp6_host_mutex_lock(Mp6Mutex *m)
{
    EnterCriticalSection((CRITICAL_SECTION *)m->opaque);
}

void mp6_host_mutex_unlock(Mp6Mutex *m)
{
    LeaveCriticalSection((CRITICAL_SECTION *)m->opaque);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} Mp6HostThreadThunk;

static DWORD WINAPI mp6_host_thread_entry(LPVOID param)
{
    Mp6HostThreadThunk t = *(Mp6HostThreadThunk *)param;
    free(param);
    t.fn(t.arg);
    return 0;
}

int mp6_host_thread_start(void (*fn)(void *), void *arg)
{
    Mp6HostThreadThunk *t = (Mp6HostThreadThunk *)malloc(sizeof(*t));
    HANDLE h;
    if (!t) {
        return (int)ERROR_NOT_ENOUGH_MEMORY;
    }
    t->fn = fn;
    t->arg = arg;
    h = CreateThread(NULL, 0, mp6_host_thread_entry, t, 0, NULL);
    if (!h) {
        DWORD err = GetLastError();
        free(t);
        return err ? (int)err : -1; /* callers print this as GetLastError */
    }
    /* Deliberately NOT CloseHandle(h): these are opt-in test threads
     * whose loops never return, and leakgate.py's handle-count diagnostic
     * observes this accounting -- a stable count either way (host.h
     * contract). */
    return 0;
}

/* =======================================================================
 * Diagnostics: mp6_host_rss_bytes
 * ======================================================================= */

size_t mp6_host_rss_bytes(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return 0; /* callers fail open */
    }
    return (size_t)pmc.WorkingSetSize;
}

/* =======================================================================
 * Crash reporting -- an always-on handler with a symbolized backtrace.
 *
 * SetUnhandledExceptionFilter catches genuine Windows access
 * violations/etc. (never a caught panic -- those already print their own
 * location), resolves the faulting address AND a real stack walk via
 * dbghelp (SymFromAddr + StackWalk64), and prints them before the process
 * exits -- turning "segfault, no output" into an actionable
 * function+offset chain on the very first run. Relies on tools/build.py's
 * fixed, non-ASLR link (--image-base=0x10000000 + --no-dynamicbase) and
 * the PDB it emits next to the exe.
 * ======================================================================= */

static void mp6_print_symbol(HANDLE proc, DWORD64 addr, const char *label)
{
    char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line;
    DWORD lineDisp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 512;

    if (SymFromAddr(proc, addr, &disp, sym)) {
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
            fprintf(stderr, "  %s 0x%016llX: %s+0x%llX (%s:%lu)\n",
                    label, (unsigned long long)addr, sym->Name, (unsigned long long)disp,
                    line.FileName, line.LineNumber);
        } else {
            fprintf(stderr, "  %s 0x%016llX: %s+0x%llX\n",
                    label, (unsigned long long)addr, sym->Name, (unsigned long long)disp);
        }
    } else {
        fprintf(stderr, "  %s 0x%016llX: <no symbol>\n", label, (unsigned long long)addr);
    }
    fflush(stderr);
}

static LONG WINAPI mp6_crash_filter(EXCEPTION_POINTERS *ep)
{
    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    CONTEXT *ctx = ep->ContextRecord;
    STACKFRAME64 frame;
    DWORD machineType;

    fprintf(stderr, "\n[MP6-CRASH] Unhandled exception 0x%08lX at address 0x%p\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "[MP6-CRASH] Access violation %s address 0x%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    fflush(stderr);

    /* SYMOPT_LOAD_ANYTHING is required: without it, dbghelp's PDB
     * age/GUID cross-check against the executable's debug directory makes
     * SymFromAddr print "<no symbol>" for every frame inside this port's
     * own module range, even with CodeView debug info compiled in and the
     * PDB present. This build pipeline recompiles+relinks the exe and its
     * PDB together as one atomic step (tools/build.py never ships one
     * without the other), so that validation is pure friction here, not a
     * safety net. It works only in combination with the real file handle
     * passed to SymLoadModuleEx below; both are needed. */
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_ANYTHING);
    /* fInvadeProcess=FALSE + an explicit SymLoadModuleEx below, rather
     * than TRUE's "auto-enumerate loaded modules": auto-invade resolves
     * system DLL frames (ntdll/kernel32) fine but NOT this process's own
     * main module. The explicit load uses the real runtime base
     * (GetModuleHandle(NULL), NOT assumed to still be the link-time
     * image base, in case a future change ever drops --no-dynamicbase). */
    if (!SymInitialize(proc, NULL, FALSE)) {
        fprintf(stderr, "[MP6-CRASH] SymInitialize failed (err=%lu) -- no symbols available\n", GetLastError());
        fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    {
        char exePath[MAX_PATH];
        HMODULE hMod = GetModuleHandle(NULL);
        DWORD64 imgBase = (DWORD64)(uintptr_t)hMod;
        MODULEINFO modInfo;
        DWORD modSize = 0;
        HANDLE hExeFile = INVALID_HANDLE_VALUE;

        memset(&modInfo, 0, sizeof(modInfo));
        if (GetModuleInformation(proc, hMod, &modInfo, sizeof(modInfo))) {
            modSize = modInfo.SizeOfImage;
        }
        fprintf(stderr, "[MP6-CRASH] main module base=0x%llX size=0x%lX\n",
                (unsigned long long)imgBase, (unsigned long)modSize);
        fflush(stderr);

        if (GetModuleFileNameA(NULL, exePath, sizeof(exePath)) > 0) {
            DWORD64 loaded;
            /* A real, already-open file handle (vs NULL) lets dbghelp
             * read the PE debug directory directly instead of re-deriving
             * it -- required together with SYMOPT_LOAD_ANYTHING above. */
            hExeFile = CreateFileA(exePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            loaded = SymLoadModuleEx(proc, hExeFile != INVALID_HANDLE_VALUE ? hExeFile : NULL,
                                      exePath, NULL, imgBase, modSize, NULL, 0);
            if (loaded == 0) {
                fprintf(stderr, "[MP6-CRASH] SymLoadModuleEx(%s, base=0x%llX) failed (err=%lu)\n",
                        exePath, (unsigned long long)imgBase, GetLastError());
                fflush(stderr);
            }
        }
    }

    mp6_print_symbol(proc, (DWORD64)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, "fault @");

    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#else
    machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx->Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Esp;
    frame.AddrStack.Mode = AddrModeFlat;
#endif

    fprintf(stderr, "[MP6-CRASH] stack walk:\n");
    fflush(stderr);
    {
        int i;
        for (i = 0; i < 32; i++) {
            if (!StackWalk64(machineType, proc, thread, &frame, ctx, NULL,
                              SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
                break;
            }
            if (frame.AddrPC.Offset == 0) break;
            mp6_print_symbol(proc, frame.AddrPC.Offset, "  frame");
        }
    }
    fflush(stderr);

    return EXCEPTION_EXECUTE_HANDLER; /* let the process terminate after printing */
}

void mp6_host_crash_install(void)
{
    SetUnhandledExceptionFilter(mp6_crash_filter);
}

/* ---------------------------------------------------------------------
 * mp6_symbolize_addr -- a reusable, non-crash-path symbol resolver (see
 * mp6_boot.h). Deliberately a SEPARATE init from mp6_crash_filter's own
 * inline SymInitialize/SymLoadModuleEx sequence above rather than a
 * shared refactor of it -- the filter is a tested, last-resort path and
 * a little duplication is cheaper than any risk of regressing it. Same
 * recipe either way (SYMOPT_LOAD_ANYTHING + a real file handle to
 * SymLoadModuleEx -- what makes in-process symbolization work at all).
 *
 * Not one of the host-seam functions -- an extra exported win32-backend
 * symbol (declared in mp6_boot.h); host_android.c stubs it with
 * "<no symbols>".
 * --------------------------------------------------------------------- */
static bool s_mp6SymReady = false;
static bool s_mp6SymAttempted = false;

static bool mp6_sym_ensure_ready(void)
{
    HANDLE proc;
    if (s_mp6SymAttempted) {
        return s_mp6SymReady;
    }
    s_mp6SymAttempted = true;
    proc = GetCurrentProcess();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_ANYTHING);
    if (!SymInitialize(proc, NULL, FALSE)) {
        return false;
    }
    {
        char exePath[MAX_PATH];
        HMODULE hMod = GetModuleHandle(NULL);
        DWORD64 imgBase = (DWORD64)(uintptr_t)hMod;
        MODULEINFO modInfo;
        DWORD modSize = 0;

        memset(&modInfo, 0, sizeof(modInfo));
        if (GetModuleInformation(proc, hMod, &modInfo, sizeof(modInfo))) {
            modSize = modInfo.SizeOfImage;
        }
        if (GetModuleFileNameA(NULL, exePath, sizeof(exePath)) > 0) {
            /* A real, already-open file handle (vs NULL) -- see the
             * matching comment in mp6_crash_filter above. Deliberately
             * left open (never CloseHandle'd): this whole function body
             * runs at most once per process (guarded above), so it's a
             * single one-time handle for the process's whole life, not a
             * per-call leak. */
            HANDLE hExeFile = CreateFileA(exePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            SymLoadModuleEx(proc, hExeFile != INVALID_HANDLE_VALUE ? hExeFile : NULL,
                             exePath, NULL, imgBase, modSize, NULL, 0);
        }
    }
    s_mp6SymReady = true;
    return true;
}

void mp6_symbolize_addr(void *addr, char *outBuf, size_t outBufSz)
{
    HANDLE proc = GetCurrentProcess();
    char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line;
    DWORD lineDisp = 0;

    if (outBufSz == 0) {
        return;
    }
    outBuf[0] = '\0';
    if (!mp6_sym_ensure_ready()) {
        snprintf(outBuf, outBufSz, "<no symbols: dbghelp init failed>");
        return;
    }

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 512;

    if (SymFromAddr(proc, (DWORD64)(uintptr_t)addr, &disp, sym)) {
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        if (SymGetLineFromAddr64(proc, (DWORD64)(uintptr_t)addr, &lineDisp, &line)) {
            snprintf(outBuf, outBufSz, "%s+0x%llx (%s:%lu)", sym->Name,
                     (unsigned long long)disp, line.FileName, line.LineNumber);
        } else {
            snprintf(outBuf, outBufSz, "%s+0x%llx", sym->Name, (unsigned long long)disp);
        }
    } else {
        snprintf(outBuf, outBufSz, "<no symbol for %p>", addr);
    }
}
