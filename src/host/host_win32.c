/* MP6 native port -- the win32 host backend (see host.h for the seam
 * contract each function implements).
 *
 * Compiled into BOTH build modes (tools/build.py PLATFORM_SOURCES_COMMON);
 * links against kernel32/dbghelp/psapi only.
 *
 * This file is the ONLY place under src/** that may include
 * windows.h, with one documented exception: src/gx/aurora_bridge.c's
 * console-repositioning nicety -- win32-only UI policy with no portable
 * meaning, kept in the one aurora-flavor TU that owns the window.
 */
#include "host.h"
#include "mp6_path.h"
#include "mp6_utf8_file.h"
#include "mp6_heap_scale.h"
#include "mp6_coro_layout.h"
#include "host_precise_sleep.h"

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

/* SAVESTATE CARVE-OUT. Placing this AFTER this TU's own
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

/* GUI release logging. Keep this independent of the game arena, renderer and
 * configuration so early startup failures and crash-handler stderr are saved.
 * Inherited pipes/files belong to the caller and must never be redirected.
 * Debug/headless entry points do not call this function. */
static int mp6_host_output_redirected(DWORD channel)
{
    HANDLE handle = GetStdHandle(channel);
    DWORD type;
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) return 0;
    type = GetFileType(handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

void mp6_host_release_logging_init(void)
{
    int keepOut = mp6_host_output_redirected(STD_OUTPUT_HANDLE);
    int keepErr = mp6_host_output_redirected(STD_ERROR_HANDLE);
    wchar_t directory[MP6_WINDOWS_PATH_WCHARS];
    wchar_t latest[MP6_WINDOWS_PATH_WCHARS];
    wchar_t previous[MP6_WINDOWS_PATH_WCHARS];
    DWORD error;
    if (keepOut && keepErr) return;
    if (mp6_utf8_to_wide_full_path("logs", directory, MP6_WINDOWS_PATH_WCHARS) != 0 ||
        mp6_utf8_to_wide_full_path("logs/mp6.log", latest, MP6_WINDOWS_PATH_WCHARS) != 0 ||
        mp6_utf8_to_wide_full_path("logs/mp6.previous.log", previous, MP6_WINDOWS_PATH_WCHARS) != 0) return;
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    if (!MoveFileExW(latest, previous, MOVEFILE_REPLACE_EXISTING)) {
        error = GetLastError();
        /* A concurrent game may still own this log. Do not truncate it. */
        if (error != ERROR_FILE_NOT_FOUND) return;
    }
    /* Append handles keep stdout and stderr from overwriting each other's
     * writes. freopen initializes the CRT FILE objects even when the GUI
     * subsystem started with invalid standard descriptors. */
    if (!keepOut) _wfreopen(latest, L"a", stdout);
    if (!keepErr) _wfreopen(latest, L"a", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
}

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

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* The tick throttle's thread owns this timer; a high-resolution waitable
 * timer (Windows 10 1803+) wakes within about half a millisecond without
 * timeBeginPeriod. NULL until first use, INVALID_HANDLE_VALUE when the
 * system has none. */
static _Thread_local HANDLE t_preciseTimer;
static _Thread_local Mp6PreciseSleep t_preciseSleep;

static void mp6_win32_wait_until(int64_t targetNs)
{
    LARGE_INTEGER due;
    int64_t wait = targetNs - (int64_t)mp6_host_monotonic_ns();
    if (wait <= 0) return;
    due.QuadPart = -(wait / 100); /* relative, in 100 ns units */
    if (SetWaitableTimerEx(t_preciseTimer, &due, 0, NULL, NULL, NULL, 0)) {
        WaitForSingleObject(t_preciseTimer, INFINITE);
    }
}

int mp6_host_sleep_until_ns(uint64_t deadlineNs)
{
    if (!mp6_precise_sleep_enabled(1)) return 0;
    if (t_preciseTimer == NULL) {
        t_preciseTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
        if (t_preciseTimer == NULL) t_preciseTimer = INVALID_HANDLE_VALUE;
    }
    if (t_preciseTimer == INVALID_HANDLE_VALUE) return 0;
    mp6_precise_sleep(&t_preciseSleep, (int64_t)deadlineNs, mp6_win32_wait_until);
    return 1;
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
 * arena (src/os/arena.c, which keeps the FATAL/exit policy and the
 * [BOOT] banner) and the coroutine stack pool
 * (src/host/coro_arena.c). They are separate reservations; either
 * has its address space reserved before graphics starts, with commitment
 * deferred until Play. Every successful reservation is verified as a whole.
 *
 * `preferredBase` (0 = none) is tried ahead of the candidate list -- see
 * host.h for why the coroutine pool names one: first-fit made ITS base a
 * function of the arena's SIZE, which is now a setting.
 * ======================================================================= */

static void *g_preparedGameArena;
static void *g_preparedCoroPool;

static void *mp6_find_low_region(uintptr_t preferredBase, size_t size, DWORD allocationType)
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
    if (size == 0 || (uint64_t)size > 0xFFFF0000ull) return NULL;

    if (preferredBase != 0) {
        got = VirtualAlloc((void *)preferredBase, size, allocationType, PAGE_READWRITE);
        if (got && !mp6_host_range_below_4gb(got, size)) {
            VirtualFree(got, 0, MEM_RELEASE);
            got = NULL;
        }
    }
    for (i = 0; !got && i < sizeof(kCandidateBases) / sizeof(kCandidateBases[0]); i++) {
        void *want = (void *)kCandidateBases[i];
        got = VirtualAlloc(want, size, allocationType, PAGE_READWRITE);
        if (got && mp6_host_range_below_4gb(got, size)) break;
        if (got) {
            VirtualFree(got, 0, MEM_RELEASE);
            got = NULL;
        }
    }
    /* A sparse list of fixed addresses can miss large free holes between
     * driver allocations. Walk actual free regions, high-bit addresses first.
     * VirtualAlloc is non-clobbering if another thread wins the region. */
    if (!got) {
        const uint64_t starts[] = {0x80000000ull, 0x10000ull};
        const uint64_t ends[] = {0x100000000ull, 0x80000000ull};
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const uint64_t granularity = info.dwAllocationGranularity;
        for (i = 0; !got && i < 2; ++i) {
            uint64_t cursor = starts[i];
            while (cursor < ends[i]) {
                MEMORY_BASIC_INFORMATION region;
                if (!VirtualQuery((void *)(uintptr_t)cursor, &region, sizeof(region))) break;
                uint64_t end = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize;
                if (end <= cursor) break;
                if (end > ends[i]) end = ends[i];
                uint64_t start = (cursor + granularity - 1) & ~(granularity - 1);
                if (region.State == MEM_FREE && start < end && size <= end - start) {
                    got = VirtualAlloc((void *)(uintptr_t)start, size, allocationType, PAGE_READWRITE);
                    if (got) break;
                }
                cursor = end;
            }
        }
    }
    if (!got) {
        got = VirtualAlloc(NULL, size, allocationType, PAGE_READWRITE);
        if (!got) {
            return NULL; /* caller (arena.c) keeps its own FATAL+exit policy */
        }
        if (!mp6_host_range_below_4gb(got, size)) {
            fprintf(stderr,
                    "[ERROR] mp6_host_arena_reserve: OS-picked range %p (+%zu bytes) is not "
                    "entirely below 4GB; refusing it\n",
                    got, size);
            VirtualFree(got, 0, MEM_RELEASE);
            return NULL;
        }
    }
    return got;
}

int mp6_host_memory_prepare(void)
{
    if (g_preparedGameArena) return 1;
#ifndef MP6_CORO_FIBERS
    /* Claim the fixed stack range first so a relocated large arena cannot
     * consume it. Neither reservation charges the game's RAM commitment. */
    g_preparedCoroPool = mp6_find_low_region(MP6_CORO_POOL_BASE, MP6_CORO_POOL_SIZE, MEM_RESERVE);
    if (!g_preparedCoroPool) return 0;
#endif
    g_preparedGameArena = mp6_find_low_region(0, MP6_ARENA_EXPANDED_BYTES, MEM_RESERVE);
    if (!g_preparedGameArena) {
        if (g_preparedCoroPool) VirtualFree(g_preparedCoroPool, 0, MEM_RELEASE);
        g_preparedCoroPool = NULL;
        return 0;
    }
    printf("[BOOT] low address space reserved before graphics: game=%p (max %u MB), coro=%p\n",
           g_preparedGameArena, MP6_ARENA_EXPANDED_BYTES / (1024u * 1024u), g_preparedCoroPool);
    return 1;
}

void *mp6_host_arena_reserve(uintptr_t preferredBase, size_t size)
{
    void *prepared = NULL;
    size_t capacity = 0;
    if (preferredBase == 0 && g_preparedGameArena) {
        prepared = g_preparedGameArena;
        capacity = MP6_ARENA_EXPANDED_BYTES;
    } else if (preferredBase == MP6_CORO_POOL_BASE && g_preparedCoroPool) {
        prepared = g_preparedCoroPool;
        capacity = MP6_CORO_POOL_SIZE;
    }
    if (prepared) {
        if (size == 0 || size > capacity || !mp6_host_range_below_4gb(prepared, size)) return NULL;
        return VirtualAlloc(prepared, size, MEM_COMMIT, PAGE_READWRITE);
    }
    return mp6_find_low_region(preferredBase, size, MEM_RESERVE | MEM_COMMIT);
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
    return mp6_host_range_below_4gb((const void *)hMod, modInfo.SizeOfImage);
}

/* Enumerates this image's own WRITABLE
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
 * include/mp6_savestate.h) is deliberately NOT filtered here: this
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
 * from the exe's own directory, so walking up from GetModuleFileNameW
 * (never cwd) and back down that fixed offset is robust to both. Returns
 * 0 only with the fst.bin probe passing, so the caller keeps its
 * build-time -D fallback otherwise. */
int mp6_host_disc_root(char *buf, size_t n)
{
    wchar_t exeWide[MP6_WINDOWS_PATH_WCHARS];
    char exeDir[1200];
    char candidateRoot[1200];
    char candidateFst[1200];
    DWORD len;
    char *lastSlash;
    char *p;
    FILE *probe;

    len = GetModuleFileNameW(NULL, exeWide,
                             (DWORD)(sizeof(exeWide) / sizeof(exeWide[0])));
    if (len == 0 || len >= sizeof(exeWide) / sizeof(exeWide[0]) ||
        mp6_wide_to_utf8_path(exeWide, exeDir, sizeof(exeDir)) != 0) {
        return -1; /* failed, non-UTF8, or beyond the explicit path cap */
    }
    /* exeDir is ".../port/mp6-native/build/mp6native.exe" -- strip the
     * filename to get the exe's own directory. Handle both separators:
     * GetModuleFileNameW normally returns '\\', but staying defensive here
     * costs nothing. */
    lastSlash = strrchr(exeDir, '\\');
    p = strrchr(exeDir, '/');
    if (p && (!lastSlash || p > lastSlash)) lastSlash = p;
    if (!lastSlash) return -1;
    *lastSlash = '\0';

    /* build/ -> mp6-native/ -> port/ -> workspace root, then back down to
     * external_refs/repos/<checkout>/orig/GP6E01 -- this workspace's fixed
     * layout, not a guess. Forward slashes work fine mixed with the
     * backslash-separated exeDir prefix on Windows.
     *
     * BOTH historical checkout names are tried, with the canonical
     * marioparty6 path used by setup/lib/common.py and tools/build.py first.
     * This keeps runtime asset lookup on the exact source tree whose commit
     * the build validated even if an older marioparty6-pin worktree is also
     * present. Naming only one path previously made this fallback disappear
     * when a workspace used the other layout.
     * Each candidate is verified independently by its own fst.bin probe,
     * so an empty leftover directory under either name is skipped rather
     * than accepted. */
    {
        static const char *const candidates[] = {
            "../../../external_refs/repos/marioparty6/orig/GP6E01",
            "../../../external_refs/repos/marioparty6-pin/orig/GP6E01",
        };
        size_t i;
        for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (mp6_path_join_checked(candidateRoot, sizeof(candidateRoot),
                                      exeDir, candidates[i]) != 0 ||
                mp6_path_join_checked(candidateFst, sizeof(candidateFst),
                                      candidateRoot, "sys/fst.bin") != 0) {
                continue;
            }
            probe = mp6_fopen_utf8(candidateFst, "rb");
            if (!probe) {
                continue; /* not there -- try the next known checkout name */
            }
            fclose(probe);
            return mp6_path_copy_checked(buf, n, candidateRoot);
        }
    }
    return -1; /* not this repo's layout -- caller keeps its fallback */
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
    wchar_t widePath[MP6_WINDOWS_PATH_WCHARS];
    DWORD attrs;
    if (path == NULL || path[0] == '\0' ||
        mp6_utf8_to_wide_full_path(path, widePath, MP6_WINDOWS_PATH_WCHARS) != 0) return -1;
    if (!CreateDirectoryW(widePath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    attrs = GetFileAttributesW(widePath);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return -1;
    }
    return 0;
}

uint64_t mp6_host_process_id(void)
{
    return (uint64_t)GetCurrentProcessId();
}


int mp6_host_sync_file_path(const char *path)
{
    wchar_t widePath[MP6_WINDOWS_PATH_WCHARS];
    HANDLE h;
    int ok;
    if (path == NULL ||
        mp6_utf8_to_wide_full_path(path, widePath, MP6_WINDOWS_PATH_WCHARS) != 0) return -1;
    h = CreateFileW(widePath, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = FlushFileBuffers(h) ? 0 : -1;
    if (!CloseHandle(h)) ok = -1;
    return ok;
}

int mp6_host_atomic_replace_file(const char *replacement, const char *destination)
{
    wchar_t wideReplacement[MP6_WINDOWS_PATH_WCHARS];
    wchar_t wideDestination[MP6_WINDOWS_PATH_WCHARS];
    DWORD attrs;
    if (replacement == NULL || destination == NULL ||
        mp6_utf8_to_wide_full_path(replacement, wideReplacement, MP6_WINDOWS_PATH_WCHARS) != 0 ||
        mp6_utf8_to_wide_full_path(destination, wideDestination, MP6_WINDOWS_PATH_WCHARS) != 0) return -1;
    attrs = GetFileAttributesW(wideDestination);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        /* ReplaceFile is the Windows transaction intended for this exact
         * operation: the old destination remains the destination unless the
         * replacement succeeds. It also preserves the destination's ACLs. */
        if (ReplaceFileW(wideDestination, wideReplacement, NULL, REPLACEFILE_WRITE_THROUGH,
                         NULL, NULL)) {
            return 0;
        }
        return -1;
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
        return -1;
    }
    /* First save into an empty slot. MOVEFILE_REPLACE_EXISTING closes the
     * existence race between the probe above and this call. */
    return MoveFileExW(wideReplacement, wideDestination,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

/* =======================================================================
 * Coroutines -- the win32 FIBER backend (compile-time fallback).
 *
 * The default backend is arena-backed minicoro
 * (src/host/coro_arena.c): fibers self-allocate an OS stack this
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
int    mp6_coro_slots_peak(void) { return 0; }
void  *mp6_coro_slot_addr(int slot) { (void)slot; return NULL; }
void   mp6_coro_savestate_precapture(void) {}
int    mp6_coro_savestate_rehydrate(void) { return 0; }

#endif /* MP6_CORO_FIBERS -- else src/host/coro_arena.c provides mp6_coro_* */

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

int mp6_host_sem_init(Mp6Sem *s)
{
    HANDLE h = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
    s->opaque[0] = h;
    return h != NULL;
}

void mp6_host_sem_post(Mp6Sem *s)
{
    ReleaseSemaphore((HANDLE)s->opaque[0], 1, NULL);
}

void mp6_host_sem_wait(Mp6Sem *s)
{
    WaitForSingleObject((HANDLE)s->opaque[0], INFINITE);
}

int mp6_host_cpu_count(void)
{
    DWORD_PTR process = 0, system = 0;
    int n = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        for (; process; process &= process - 1) n++;
    }
    return n > 0 ? n : 1;
}

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
 * Read-only content files: positional reads on a shared handle
 * ======================================================================= */

struct Mp6HostFile {
    HANDLE handle;
};

Mp6HostFile *mp6_host_file_open(const char *utf8Path)
{
    wchar_t widePath[MP6_WINDOWS_PATH_WCHARS];
    Mp6HostFile *f;
    HANDLE h;
    if (utf8Path == NULL ||
        mp6_utf8_to_wide_full_path(utf8Path, widePath, MP6_WINDOWS_PATH_WCHARS) != 0) {
        return NULL;
    }
    h = CreateFileW(widePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    f = (Mp6HostFile *)malloc(sizeof(*f));
    if (f == NULL) {
        CloseHandle(h);
        return NULL;
    }
    f->handle = h;
    return f;
}

int64_t mp6_host_file_size(Mp6HostFile *f)
{
    LARGE_INTEGER size;
    if (f == NULL || !GetFileSizeEx(f->handle, &size)) return -1;
    return (int64_t)size.QuadPart;
}

int64_t mp6_host_file_read_at(Mp6HostFile *f, void *dst, size_t len, uint64_t offset)
{
    size_t done = 0;
    if (f == NULL || (dst == NULL && len > 0)) return -1;
    while (done < len) {
        OVERLAPPED at;
        DWORD want = (len - done) > 0x40000000u ? 0x40000000u : (DWORD)(len - done);
        DWORD got = 0;
        uint64_t pos = offset + done;
        memset(&at, 0, sizeof(at));
        at.Offset = (DWORD)pos;
        at.OffsetHigh = (DWORD)(pos >> 32);
        if (!ReadFile(f->handle, (char *)dst + done, want, &got, &at)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return -1;
        }
        if (got == 0) break;
        done += got;
    }
    return (int64_t)done;
}

void mp6_host_file_close(Mp6HostFile *f)
{
    if (f == NULL) return;
    CloseHandle(f->handle);
    free(f);
}

int mp6_host_file_prefetch(const char *utf8Path, uint64_t offset, uint64_t len)
{
    (void)utf8Path;
    (void)offset;
    (void)len;
    return 0; /* a local disk: the OS cache already reads ahead */
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
        wchar_t exePath[MP6_WINDOWS_PATH_WCHARS];
        char exePathUtf8[1024] = "<unprintable>";
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

        {
            DWORD exeLen = GetModuleFileNameW(NULL, exePath,
                                              MP6_WINDOWS_PATH_WCHARS);
            if (exeLen > 0 && exeLen < MP6_WINDOWS_PATH_WCHARS) {
            DWORD64 loaded;
            (void)mp6_wide_to_utf8_path(exePath, exePathUtf8,
                                         sizeof(exePathUtf8));
            /* A real, already-open file handle (vs NULL) lets dbghelp
             * read the PE debug directory directly instead of re-deriving
             * it -- required together with SYMOPT_LOAD_ANYTHING above. */
            hExeFile = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
            loaded = SymLoadModuleExW(proc,
                                      hExeFile != INVALID_HANDLE_VALUE ? hExeFile : NULL,
                                      exePath, NULL, imgBase, modSize, NULL, 0);
            if (loaded == 0) {
                fprintf(stderr, "[MP6-CRASH] SymLoadModuleEx(%s, base=0x%llX) failed (err=%lu)\n",
                        exePathUtf8, (unsigned long long)imgBase, GetLastError());
                fflush(stderr);
            }
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

static void mp6_sample_profile_start(void);

/* MP6_CPU_EXCLUDE=8,9 keeps the game (and every thread it starts later:
 * Dawn, audio, decode-ahead workers) off those logical CPUs, as the build
 * tools already do (tools/host_cpus.py). For machines with a faulty core. */
static void mp6_cpu_exclude_apply(void)
{
    const char *e = getenv("MP6_CPU_EXCLUDE");
    DWORD_PTR process = 0, system = 0, remove = 0;
    if (!e || !e[0] || !GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return;
    while (*e) {
        char *end;
        unsigned long cpu = strtoul(e, &end, 10);
        if (end == e) {
            e++;
            continue;
        }
        if (cpu < sizeof(DWORD_PTR) * 8) remove |= (DWORD_PTR)1 << cpu;
        e = end;
    }
    if ((process & ~remove) != 0 && (process & remove) != 0)
        SetProcessAffinityMask(GetCurrentProcess(), process & ~remove);
}

/* MP6_CRASH_VEH=1: report faults from a vectored handler too. A fault on a
 * coroutine stack never reaches the unhandled-exception filter (the thread's
 * recorded stack bounds do not cover it, so dispatch gives up and the
 * process just dies); vectored handlers run before that check. Opt-in:
 * they also see first-chance exceptions that drivers raise and handle. */
static LONG WINAPI mp6_crash_vectored(EXCEPTION_POINTERS *ep)
{
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        mp6_crash_filter(ep);
        TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void mp6_host_crash_install(void)
{
    const char *veh = getenv("MP6_CRASH_VEH");
    SetUnhandledExceptionFilter(mp6_crash_filter);
    if (veh && veh[0] == '1') AddVectoredExceptionHandler(1, mp6_crash_vectored);
    mp6_cpu_exclude_apply();
    /* Both mains call this first thing on the game thread, which is the
     * thread the opt-in sampler below watches. */
    mp6_sample_profile_start();
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
        wchar_t exePath[MP6_WINDOWS_PATH_WCHARS];
        HMODULE hMod = GetModuleHandle(NULL);
        DWORD64 imgBase = (DWORD64)(uintptr_t)hMod;
        MODULEINFO modInfo;
        DWORD modSize = 0;

        memset(&modInfo, 0, sizeof(modInfo));
        if (GetModuleInformation(proc, hMod, &modInfo, sizeof(modInfo))) {
            modSize = modInfo.SizeOfImage;
        }
        {
            DWORD exeLen = GetModuleFileNameW(NULL, exePath,
                                              MP6_WINDOWS_PATH_WCHARS);
            if (exeLen > 0 && exeLen < MP6_WINDOWS_PATH_WCHARS) {
            /* A real, already-open file handle (vs NULL) -- see the
             * matching comment in mp6_crash_filter above. Deliberately
             * left open (never CloseHandle'd): this whole function body
             * runs at most once per process (guarded above), so it's a
             * single one-time handle for the process's whole life, not a
             * per-call leak. */
            HANDLE hExeFile = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ,
                                          NULL, OPEN_EXISTING, 0, NULL);
            SymLoadModuleExW(proc,
                             hExeFile != INVALID_HANDLE_VALUE ? hExeFile : NULL,
                             exePath, NULL, imgBase, modSize, NULL, 0);
            }
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

/* =======================================================================
 * Game-thread sampling profiler (MP6_SAMPLE_PROFILE=<firstTick>-<lastTick>)
 * Opt-in hitch finder for load and frame-time work. A helper thread
 * suspends the game thread about once a millisecond while mp6_tick_count
 * is inside the window, records its instruction pointer, and at exit
 * prints the hottest functions over the window, every tick that took more
 * than 25 samples (~25 ms), and a per-function breakdown of ticks over 60.
 * One sample is roughly one millisecond of game-thread time. Release
 * builds keep their PDB, so names resolve through mp6_symbolize_addr;
 * code without symbols is attributed to its module ([ucrtbase.dll]).
 * Unset, nothing is allocated and no thread is started.
 * ======================================================================= */

#define MP6_SAMPLE_MAX (1 << 20)
#define MP6_SAMPLE_HITCH_MS 25
#define MP6_SAMPLE_DETAIL_MS 60

#define MP6_SAMPLE_FRAMES 8

typedef struct {
    uint64_t pc[MP6_SAMPLE_FRAMES]; /* [0] = where it was, then its callers */
    long tick;
} Mp6Sample;

typedef struct {
    char name[160];
    long count;
} Mp6SampleFn;

static Mp6Sample *g_samples;
static volatile LONG g_sampleCount;
static HANDLE g_sampleThread;
static long g_sampleFirst, g_sampleLast;
static volatile int g_sampleStop;
/* MP6_SAMPLE_THREAD=render samples Aurora's render worker instead of the game
 * thread; its handle exists only once the renderer has started. */
static int g_sampleRender;

/* Aurora's (render_worker.cpp) when linked; the headless build has none. */
__attribute__((weak)) void *aurora_render_worker_thread_handle(void) { return NULL; }

static DWORD WINAPI mp6_sample_loop(LPVOID unused)
{
    (void)unused;
    while (!g_sampleStop) {
        CONTEXT ctx;
        long tick;
        Sleep(1);
        tick = mp6_tick_count;
        if (tick < g_sampleFirst || tick > g_sampleLast) continue;
        if (g_sampleCount >= MP6_SAMPLE_MAX) break;
        if (g_sampleThread == NULL) {
            HANDLE worker = (HANDLE)aurora_render_worker_thread_handle();
            if (worker == NULL ||
                !DuplicateHandle(GetCurrentProcess(), worker, GetCurrentProcess(), &g_sampleThread,
                                 THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                 FALSE, 0)) {
                g_sampleThread = NULL;
                continue;
            }
        }
        if (SuspendThread(g_sampleThread) == (DWORD)-1) break;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(g_sampleThread, &ctx)) {
            Mp6Sample *sample = &g_samples[g_sampleCount];
            int f;
            memset(sample, 0, sizeof(*sample));
            sample->tick = tick;
            /* Unwind a few frames so time inside the C runtime or the OS can
             * be charged to the game code that called it. */
            for (f = 0; f < MP6_SAMPLE_FRAMES && ctx.Rip; f++) {
                DWORD64 image = 0, frame = 0;
                PVOID handler = NULL;
                PRUNTIME_FUNCTION fn;
                sample->pc[f] = ctx.Rip;
                fn = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
                if (fn) {
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler, &frame, NULL);
                } else {
                    ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp; /* leaf function */
                    ctx.Rsp += 8;
                }
            }
            g_sampleCount++;
        }
        ResumeThread(g_sampleThread);
    }
    return 0;
}

static int mp6_sample_fn_cmp(const void *a, const void *b)
{
    long ca = ((const Mp6SampleFn *)a)->count;
    long cb = ((const Mp6SampleFn *)b)->count;
    return (ca < cb) - (ca > cb);
}

/* Function name without the +offset, or [module] when there is no symbol. */
static void mp6_sample_name(uint64_t pc, char *out, size_t n)
{
    char buf[700];
    char *plus;
    mp6_symbolize_addr((void *)(uintptr_t)pc, buf, sizeof(buf));
    plus = strchr(buf, '+');
    if (plus) *plus = 0;
    if (strncmp(buf, "<no symbol", 10) == 0) {
        HMODULE mod = NULL;
        char path[MAX_PATH] = "?";
        const char *base;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)pc, &mod)) {
            GetModuleFileNameA(mod, path, sizeof(path));
        }
        base = strrchr(path, '\\');
        snprintf(buf, sizeof(buf), "[%s]", base ? base + 1 : path);
    }
    snprintf(out, n, "%s", buf);
}

/* 1 when name is one of list's comma-separated function names. */
static int mp6_sample_listed(const char *list, const char *name)
{
    const char *p;
    size_t n = strlen(name);
    for (p = list; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        size_t len = strcspn(p, ",");
        if (len == n && strncmp(p, name, n) == 0) return 1;
    }
    return 0;
}

/* MP6_SAMPLE_CALLERS: shared helpers charged to their callers. */
static int mp6_sample_charge_callers(const char *name)
{
    static const char *list = (const char *)-1;
    if (list == (const char *)-1) list = getenv("MP6_SAMPLE_CALLERS");
    return mp6_sample_listed(list, name);
}

/* MP6_SAMPLE_LINES: functions whose samples are split by source line. */
static void mp6_sample_add_line(uint64_t pc, char *name, size_t n)
{
    static const char *list = (const char *)-1;
    char buf[700];
    const char *open, *colon;
    if (list == (const char *)-1) list = getenv("MP6_SAMPLE_LINES");
    if (!mp6_sample_listed(list, name)) return;
    mp6_symbolize_addr((void *)(uintptr_t)pc, buf, sizeof(buf));
    open = strrchr(buf, '(');
    colon = open ? strrchr(open, ':') : NULL;
    if (colon) {
        size_t used = strlen(name);
        snprintf(name + used, n - used, ":%s", colon + 1);
        used = strlen(name);
        if (used && name[used - 1] == ')') name[used - 1] = 0;
    }
}

static void mp6_sample_report_range(const char *label, long t0, long t1)
{
    static Mp6SampleFn fns[4096];
    int nf = 0, k;
    long total = 0;
    LONG i;
    for (i = 0; i < g_sampleCount; i++) {
        char name[160];
        int f;
        if (g_samples[i].tick < t0 || g_samples[i].tick > t1) continue;
        mp6_sample_name(g_samples[i].pc[0], name, sizeof(name));
        mp6_sample_add_line(g_samples[i].pc[0], name, sizeof(name));
        /* Shared helpers named in MP6_SAMPLE_CALLERS (e.g. memset,memcpyFast)
         * are charged to their two nearest callers instead. */
        if (mp6_sample_charge_callers(name)) {
            for (f = 1; f <= 2 && f < MP6_SAMPLE_FRAMES && g_samples[i].pc[f]; f++) {
                char caller[160];
                size_t used = strlen(name);
                mp6_sample_name(g_samples[i].pc[f], caller, sizeof(caller));
                snprintf(name + used, sizeof(name) - used, " <- %s", caller);
            }
        }
        /* A runtime or OS module: name the first caller that has a symbol. */
        for (f = 1; name[0] == '[' && f < MP6_SAMPLE_FRAMES && g_samples[i].pc[f]; f++) {
            char caller[160];
            mp6_sample_name(g_samples[i].pc[f], caller, sizeof(caller));
            if (caller[0] != '[') {
                size_t used = strlen(name);
                snprintf(name + used, sizeof(name) - used, " <- %s", caller);
                break;
            }
        }
        for (k = 0; k < nf; k++) {
            if (strcmp(fns[k].name, name) == 0) break;
        }
        if (k == nf) {
            if (nf == 4096) continue;
            snprintf(fns[nf].name, sizeof(fns[nf].name), "%s", name);
            fns[nf].count = 0;
            nf++;
        }
        fns[k].count++;
        total++;
    }
    qsort(fns, nf, sizeof(fns[0]), mp6_sample_fn_cmp);
    fprintf(stderr, "[MP6-SAMPLE] %s ticks %ld-%ld: %ld samples\n", label, t0, t1, total);
    for (k = 0; k < nf && k < 30; k++) {
        fprintf(stderr, "[MP6-SAMPLE]   %6ld %5.1f%%  %s\n", fns[k].count,
                100.0 * fns[k].count / (total ? total : 1), fns[k].name);
    }
}

static void mp6_sample_profile_report(void)
{
    long span, t;
    long *perTick;
    LONG i;
    g_sampleStop = 1;
    if (!g_samples) return;
    mp6_sample_report_range("all", g_sampleFirst, g_sampleLast);
    span = g_sampleLast - g_sampleFirst + 1;
    perTick = (long *)calloc((size_t)span, sizeof(long));
    if (perTick) {
        for (i = 0; i < g_sampleCount; i++) perTick[g_samples[i].tick - g_sampleFirst]++;
        for (t = 0; t < span; t++) {
            if (perTick[t] > MP6_SAMPLE_HITCH_MS) {
                fprintf(stderr, "[MP6-SAMPLE] hitch tick %ld ~%ld ms\n", g_sampleFirst + t, perTick[t]);
            }
        }
        for (t = 0; t < span; t++) {
            if (perTick[t] > MP6_SAMPLE_DETAIL_MS) {
                mp6_sample_report_range("hitch", g_sampleFirst + t, g_sampleFirst + t);
            }
        }
        free(perTick);
    }
    fflush(stderr);
}

static void mp6_sample_profile_start(void)
{
    const char *e = getenv("MP6_SAMPLE_PROFILE");
    if (!e || !e[0]) return;
    if (sscanf(e, "%ld-%ld", &g_sampleFirst, &g_sampleLast) != 2 || g_sampleLast < g_sampleFirst) {
        fprintf(stderr, "[MP6-SAMPLE] MP6_SAMPLE_PROFILE must be <firstTick>-<lastTick>\n");
        return;
    }
    {
        const char *which = getenv("MP6_SAMPLE_THREAD");
        g_sampleRender = which != NULL && strcmp(which, "render") == 0;
    }
    g_samples = (Mp6Sample *)calloc(MP6_SAMPLE_MAX, sizeof(Mp6Sample));
    if (!g_samples ||
        (!g_sampleRender &&
         !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_sampleThread,
                          THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, 0)) ||
        !CreateThread(NULL, 0, mp6_sample_loop, NULL, 0, NULL)) {
        fprintf(stderr, "[MP6-SAMPLE] could not start the sampler\n");
        return;
    }
    atexit(mp6_sample_profile_report);
    fprintf(stderr, "[MP6-SAMPLE] sampling the %s thread for ticks %ld-%ld\n",
            g_sampleRender ? "render worker" : "game", g_sampleFirst, g_sampleLast);
}
