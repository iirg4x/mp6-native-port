/* MP6 native port -- the platform/host/ seam.
 *
 * Every OS-facing runtime mechanic the port needs, behind one small
 * interface, so a new platform is one new backend file instead of a sweep
 * over platform/**. Backends: host_win32.c (links into BOTH build modes,
 * headless and windowed) and host_android.c. The seam core is
 * deliberately SDL-free: the --headless build links no SDL at all
 * (tools/build.py's platform-unit split).
 *
 * What is deliberately NOT here: everything SDL3 already abstracts (audio
 * out, window/aspect, keyboard/gamepad), stdout (every gate is
 * stdout-scrape based; it works on all targets), and the win32
 * console-repositioning nicety (aurora_bridge.c; win32-only by nature, so
 * that one aurora-flavor TU keeps its own windows.h include).
 *
 * Types: this header is included from BOTH compile flavors (decomp-flag
 * TUs with the decomp's dolphin types, and aurora-flag TUs with aurora's),
 * so it depends only on <stdint.h>/<stddef.h>, never on either dolphin.h.
 */
#ifndef MP6_HOST_H
#define MP6_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Opaque/plain types
 * --------------------------------------------------------------------- */

/* A stackful coroutine context (one per HuPrc process). Opaque; created/
 * destroyed only via mp6_coro_create/mp6_coro_destroy below. */
typedef struct Mp6Coro Mp6Coro;

/* A plain (non-recursive) mutex with caller-provided static storage, so a
 * backend needs no allocation for it. Sized to hold any backend's native
 * object (win32 CRITICAL_SECTION is 40 bytes on x64; host_win32.c
 * static-asserts the fit). Zero-initialized storage is NOT a valid mutex
 * -- call mp6_host_mutex_init exactly once first. */
typedef struct Mp6Mutex {
    void *opaque[8];
} Mp6Mutex;

/* Local wall-clock ("RTC") calendar time. Fields mirror what the one
 * consumer (shims_manual.c's OSGetTime/OSGetTick RTC seed) needs: local
 * time, no zone info, msec 0-999, mon 1-12, day 1-31, year AD
 * (e.g. 2026). */
typedef struct Mp6DateTime {
    int year;
    int mon;
    int day;
    int hour;
    int min;
    int sec;
    int msec;
} Mp6DateTime;

/* ---------------------------------------------------------------------
 * Lifecycle (2)
 * --------------------------------------------------------------------- */

/* One-time host-side timing setup: raises the OS timer resolution for the
 * life of the process (win32: winmm timeBeginPeriod(1), resolved at
 * runtime via LoadLibraryA) and registers its own atexit() teardown
 * (timeEndPeriod). Idempotent -- later calls return the first call's
 * result.
 *
 * Returns nonzero if the resolution push took effect, 0 if unavailable
 * (winmm missing / timeBeginPeriod refused) -- the tick throttle prints
 * that status in its "[MP6-TICK] tick throttle active" boot line.
 *
 * Call-site policy is the caller's, not this function's: the tick
 * throttle calls it only when the throttle actually engages (MP6_TICK_HZ=0
 * must keep loading no winmm and pushing no timer resolution), so nothing
 * else should call this "for completeness". Android backend: a no-op
 * returning nonzero. */
int mp6_host_init(void);

/* Installs the host's crash reporting: win32 = a dbghelp
 * SetUnhandledExceptionFilter handler with a symbolized stack walk. Call
 * once, as early as possible in main() -- both main_native.c mains do.
 * Android: honest no-op (debuggerd tombstones already exist); the seam
 * call exists so an upgrade there is additive. */
void mp6_host_crash_install(void);

/* ---------------------------------------------------------------------
 * Time (3)
 * --------------------------------------------------------------------- */

/* Monotonic time in nanoseconds since an arbitrary process-stable epoch.
 * win32: QueryPerformanceCounter scaled to ns with exact integer math
 * (sec + remainder split; no double rounding, monotone). android:
 * clock_gettime(CLOCK_MONOTONIC). Consumers: the tick throttle's
 * absolute-deadline math and OSGetTime/OSGetTick's RTC-seeded timebase --
 * both keep their own deadline/seed math and treat this as the raw
 * counter it is (1 ns units, timebase constant 1000000000 per second). */
uint64_t mp6_host_monotonic_ns(void);

/* Coarse OS sleep -- a thin primitive, NOT a precise waiter. win32:
 * Sleep(ns / 1000000), so sub-millisecond values (including 0) are
 * Sleep(0) = "yield to any ready thread" -- semantics the tick throttle's
 * 2..3ms band relies on. Callers needing precision keep their own spin on
 * mp6_host_monotonic_ns (the tick throttle does). android: nanosleep with
 * the same "coarse is fine" contract. */
void mp6_host_sleep_ns(uint64_t ns);

/* Local wall-clock calendar time, for the one RTC-seed consumer
 * (OSGetTime's "ticks since 2000-01-01 local" base). win32: GetLocalTime.
 * android: localtime_r. Local -- not UTC -- deliberately: the GC RTC
 * counts the user's wall-clock time (see shims_manual.c's RTC comment,
 * which travels with the call site). */
void mp6_host_wallclock(Mp6DateTime *out);

/* ---------------------------------------------------------------------
 * Memory (2)
 * --------------------------------------------------------------------- */

/* Reserve+commit the game arena, preferring the fixed low candidate bases
 * (0x80000000 first -- game/memory.c's BLOCK_CHECK high-bit check; full
 * candidate list and rationale in the backends). Falls back to an
 * OS-chosen address (with a stderr [WARN] about >=4GB round-trip risk) if
 * every candidate is taken. Returns NULL only if even that failed -- the
 * caller (arena.c) keeps its own [FATAL]+exit policy. win32:
 * VirtualAlloc(MEM_RESERVE|MEM_COMMIT). android: mmap over the same
 * candidate list. */
void *mp6_host_arena_reserve(size_t size);

/* Nonzero iff the running game image (code + statics) sits entirely below
 * 4 GB -- the "whole image low" invariant every u32<->pointer round-trip
 * relies on (code addresses live in jmp_buf.lr and OSModuleHeader
 * prolog/epilog). win32: GetModuleHandle(NULL) base + GetModuleInformation
 * size (the image is linked at 0x10000000 with ASLR off, so this holds by
 * construction). 0 also on query failure -- callers treat that as "cannot
 * prove the invariant" and fail closed. Consumed by main_native.c's boot
 * assert. */
int mp6_host_image_below_4gb(void);

/* ---------------------------------------------------------------------
 * Paths & filesystem (4)
 *
 * Path-result contract (all three _dir/_root functions): returns 0 and a
 * NUL-terminated path in buf on success; nonzero (buf contents undefined)
 * when the host cannot resolve one, in which case the caller keeps its
 * own fallback. Paths may be relative (see mp6_host_save_dir).
 * --------------------------------------------------------------------- */

/* The extracted disc tree's root ("<...>/GP6E01" -- the directory holding
 * sys/fst.bin and files/), VERIFIED to exist: this returns 0 only after
 * probing "<root>/sys/fst.bin" openable. win32: an exe-relative walk to
 * the sibling decomp checkout's extracted disc (this repo's fixed
 * layout); the caller (dvd_files.c) appends "/files" and "/sys/fst.bin"
 * and keeps its build-time -D fallback when this returns nonzero.
 * android: shell-provided app storage root (MP6_HOST_BASE). */
int mp6_host_disc_root(char *buf, size_t n);

/* Base directory for memory-card (GCI-folder) storage. win32: the
 * cwd-relative "saves" -- deliberately NOT "<exe>/saves": existing save
 * layouts are cwd-relative, and changing the resolution would orphan
 * them. android: "<app files dir>/saves". */
int mp6_host_save_dir(char *buf, size_t n);

/* Base directory for config/diagnostic output (framescope dumps, wav
 * dumps, ...). win32: "." -- the cwd, where every current diagnostic
 * writer's relative fopen already lands; those writers are portable C and
 * keep their own relative paths, so this is currently uncalled
 * (interface-completeness for platforms that need a real writable dir). */
int mp6_host_pref_dir(char *buf, size_t n);

/* Create a directory (parents NOT created; mirrors CreateDirectoryA /
 * mkdir(2)). Returns 0 if created or it already existed, nonzero on any
 * other failure. Callers (card_native.c) don't check the result; the
 * return exists for future callers. */
int mp6_host_mkdir(const char *path);

/* ---------------------------------------------------------------------
 * Coroutines (3) -- the HuPrc process scheduler's context backend
 *
 * The default backend is minicoro running on caller-provided stacks
 * carved from the low arena (see coro_arena.c); win32 fibers are retained
 * as a compile-time fallback (-DMP6_CORO_FIBERS) behind the same three
 * functions.
 * --------------------------------------------------------------------- */

/* Create a suspended coroutine that will run fn(arg) when first switched
 * to. `stack`/`stack_size`: caller-provided stack storage for backends
 * that support it; the win32 FIBER backend ignores `stack` (fibers always
 * get an OS-managed stack) and passes stack_size to CreateFiber as the
 * stack commit size. fn must never return (HuPrc processes end via
 * HuPrcEnd -> mp6_coro_switch(NULL) -> mp6_coro_destroy from the
 * dispatcher; process_native.c's trampoline FATALs first if game code
 * ever plain-returns).
 *
 * On creation failure the win32 backend prints a
 * "[FATAL] process_native: ..." line and exits(1), so on win32 this never
 * returns NULL. (A future backend may return NULL; callers today rely on
 * the fatal contract.) */
Mp6Coro *mp6_coro_create(void (*fn)(void *), void *arg, void *stack, size_t stack_size);

/* The one switch primitive, both directions:
 *   - mp6_coro_switch(c): dispatcher side. Saves the CURRENT context as
 *     "the dispatcher" (lazily converting the calling thread on first
 *     use), runs c until it switches back, then restores the previous
 *     dispatcher context pointer (nesting discipline for dispatch-within-
 *     dispatch, e.g. HuPrcDispatch from inside a process).
 *   - mp6_coro_switch(NULL): process side. Yields straight back to the
 *     dispatcher context that switched this coroutine in. Full stack
 *     state is preserved; the next switch-to resumes exactly here.
 * NOT thread-safe across OS threads by design -- the whole HuPrc
 * scheduler runs on the game's one main thread. */
void mp6_coro_switch(Mp6Coro *to);

/* Destroy a SUSPENDED coroutine (never the one currently running -- same
 * rule as win32 DeleteFiber; both call sites run on the dispatcher
 * context). Frees the context and, in the fiber backend, its OS-managed
 * stack. */
void mp6_coro_destroy(Mp6Coro *c);

/* ---------------------------------------------------------------------
 * Mutex / threads (4) -- msm mixer lock + opt-in leak-stress threads
 * --------------------------------------------------------------------- */

/* Initialize m's storage as a mutex. Call exactly once per mutex before
 * any lock/unlock (msm_bridge.c guards with its own init flag). There is
 * deliberately no destroy: both existing mutexes are process-lifetime.
 * win32: InitializeCriticalSection. android: pthread_mutex_init. */
void mp6_host_mutex_init(Mp6Mutex *m);
/* Blocking lock. Same-thread recursion is NOT part of the contract even
 * though win32 CRITICAL_SECTION happens to allow it -- no current caller
 * recurses (the msm mixer/group locks are leaf locks). */
void mp6_host_mutex_lock(Mp6Mutex *m);
void mp6_host_mutex_unlock(Mp6Mutex *m);

/* Start a detached-lifetime background thread running fn(arg). Used ONLY
 * by the opt-in MP6_AUDIO_LEAKTEST_* stress hooks; those threads run for
 * the life of the process (their loops never return). Returns 0 on
 * success, else a nonzero OS-specific error code (win32: GetLastError())
 * which callers include in their failure prints. The win32 backend
 * deliberately does NOT CloseHandle the created thread's handle: the
 * handle count is an observable diagnostic (leakgate.py samples it), and
 * these process-lifetime threads keep a stable count either way. */
int mp6_host_thread_start(void (*fn)(void *), void *arg);

/* ---------------------------------------------------------------------
 * Diagnostics (1)
 * --------------------------------------------------------------------- */

/* Current resident-set ("working set") size in bytes; 0 when the host
 * cannot sample it (callers fail open -- the RSS watchdog stays silent
 * rather than false-firing). win32: GetProcessMemoryInfo WorkingSetSize.
 * android: /proc/self/statm. Growth-gated only; never threshold-compared
 * across platforms. */
size_t mp6_host_rss_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_HOST_H */
