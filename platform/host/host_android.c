/* MP6 native port -- the Android (bionic/posix) host backend (see
 * platform/host/host.h for the seam contract; win32 backend:
 * host_win32.c).
 *
 * Replacement mechanisms: clock_gettime(CLOCK_MONOTONIC), nanosleep,
 * localtime_r, an mmap candidate-base loop mirroring the win32
 * VirtualAlloc loop byte-for-byte in policy (same bases, same order, same
 * [WARN] fallback text), dl_iterate_phdr for the image-below-4GB
 * invariant, pthread mutex/thread, and /proc/self/statm for RSS.
 *
 * The COROUTINE backend is deliberately NOT here: Android always uses the
 * shared arena-backed minicoro backend (platform/host/coro_arena.c --
 * MCO_USE_ASM covers aarch64), the same file and mechanism Windows ships
 * as its default. The win32 FIBER fallback does not exist on this
 * platform, so a -DMP6_CORO_FIBERS android build is a link error by
 * construction (no mp6_coro_* provider) rather than a silent wrong
 * backend.
 *
 * Crash reporting: a sigaction(SIGSEGV/SIGBUS/SIGILL/SIGFPE) handler on
 * its own sigaltstack that prints the [MP6-CRASH] header (fault pc +
 * si_addr), dladdr-resolves the pc (every extern function in
 * libmp6game.so is a dynamic symbol, so this genuinely names game
 * functions), best-effort calls bionic's backtrace()/
 * backtrace_symbols_fd() when the device's libc has them (API 33+;
 * resolved at RUNTIME via dlsym so the artifact can still target older
 * API levels), then re-raises with the default disposition so debuggerd
 * still writes its full tombstone.
 *
 * Paths: the launcher (platform/android/mp6launcher.c) publishes the
 * on-device base directory as the MP6_HOST_BASE env var before dlopen'ing
 * the game .so -- mp6_host_disc_root/save_dir/pref_dir all resolve under
 * it. The disc root contract matches host_win32.c's exactly: return 0
 * only with the "<root>/sys/fst.bin" openability probe passing, so
 * dvd_files.c's fallback path stays the same honest degradation.
 *
 * Compiled with COMMON_FLAGS like host_win32.c (dolphin_compat.h is
 * force-included; nothing here depends on it) in the android target row
 * only (tools/build.py --target aarch64-android). No SDL, no aurora, no
 * windows.h -- exactly the headless TU set.
 */
#include "host.h"

#include "mp6_boot.h" /* mp6_symbolize_addr's declaration (extra exported
                       * backend symbol, same as host_win32.c) */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000 /* kernel >= 4.17; the plain-hint
                                      * fallback below covers anything older */
#endif

/* =======================================================================
 * Lifecycle: mp6_host_init
 * The win32 backend's job here is the winmm timer-resolution push for the
 * tick throttle; Linux/bionic nanosleep needs no resolution push. host.h's
 * contract for this backend: "a no-op returning nonzero". Idempotent
 * trivially.
 * ======================================================================= */
int mp6_host_init(void)
{
    return 1;
}

/* =======================================================================
 * Time
 * ======================================================================= */

uint64_t mp6_host_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void mp6_host_sleep_ns(uint64_t ns)
{
    /* Same thin-primitive contract as the win32 Sleep(ns/1e6): a 0
     * request keeps the "yield to any ready thread" semantics. */
    if (ns == 0) {
        sched_yield();
        return;
    }
    {
        struct timespec ts;
        ts.tv_sec = (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        nanosleep(&ts, NULL); /* coarse by contract -- callers needing
                               * precision spin on mp6_host_monotonic_ns */
    }
}

void mp6_host_wallclock(Mp6DateTime *out)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm); /* local, not UTC -- the GC RTC counts
                                   * the user's wall clock (host.h) */
    out->year = tm.tm_year + 1900;
    out->mon  = tm.tm_mon + 1;
    out->day  = tm.tm_mday;
    out->hour = tm.tm_hour;
    out->min  = tm.tm_min;
    out->sec  = tm.tm_sec;
    out->msec = (int)(ts.tv_nsec / 1000000);
}

/* =======================================================================
 * Memory: mp6_host_arena_reserve / mp6_host_image_below_4gb
 * ======================================================================= */

/* One non-clobbering fixed-base attempt: MAP_FIXED_NOREPLACE where the
 * kernel knows it, a verified plain-hint attempt where it doesn't
 * (pre-4.17 kernels ignore unknown mmap flags, so a "success" at the
 * WRONG address must be undone and treated as unavailable, never used).
 * PROT_READ|PROT_WRITE up front -- the win32 backend commits rw in the
 * same call (MEM_RESERVE|MEM_COMMIT); MAP_NORESERVE keeps the arena and
 * coro pool as lazy commit charge, so RSS stays touched-pages-only
 * exactly like VirtualAlloc's demand-zero pages. */
static void *mp6_try_fixed_low(uintptr_t base, size_t size)
{
    void *want = (void *)base;
    void *got = mmap(want, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == want) {
        return got;
    }
    if (got == MAP_FAILED) {
        if (errno == EEXIST) {
            return NULL; /* base occupied */
        }
        /* EINVAL/EOPNOTSUPP etc: kernel may not know the flag -- fall
         * through to the verified plain-hint attempt. */
    } else {
        /* Landed at the wrong address: old kernel treated the unknown flag
         * as a plain hint it then declined. Undo; base is unavailable. */
        munmap(got, size);
        return NULL;
    }
    got = mmap(want, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (got == want) {
        return got;
    }
    if (got != MAP_FAILED) {
        munmap(got, size);
    }
    return NULL;
}

void *mp6_host_arena_reserve(size_t size)
{
    /* Same candidate list, same order, same rationale as host_win32.c
     * (0x8xxxxxxx first: game/memory.c's BLOCK_CHECK_BROKEN wants the
     * high bit -- the full comment travels with the win32 copy). */
    static const uintptr_t kCandidateBases[] = {
        0x80000000u, 0x90000000u, 0xA0000000u, 0xB0000000u,
        0x40000000u, 0x50000000u, 0x60000000u, 0x70000000u, 0x20000000u, 0x30000000u,
    };
    size_t i;
    void *got = NULL;

    for (i = 0; i < sizeof(kCandidateBases) / sizeof(kCandidateBases[0]); i++) {
        got = mp6_try_fixed_low(kCandidateBases[i], size);
        if (got) break;
    }
    if (!got) {
        got = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (got == MAP_FAILED) {
            return NULL; /* caller (arena.c / coro_arena.c) keeps its own FATAL policy */
        }
        /* Same fallback WARN as host_win32.c, byte-for-byte. */
        fprintf(stderr,
                "[WARN] mp6_host_arena_reserve: no candidate low(<4GB) base free; OS picked %p -- "
                "u32 pointer round-trips may break if this is >=4GB\n",
                got);
    }
    return got;
}

/* The image here is libmp6game.so -- placed low by the launcher via
 * android_dlopen_ext(ANDROID_DLEXT_RESERVED_ADDRESS). Walk the loaded
 * modules, find the one THIS function's own address lives in, and prove
 * all of its PT_LOAD segments end below 4GB. 0 on any failure to
 * identify the module -- "cannot prove the invariant", fail closed, same
 * contract as the win32 backend. */
struct Mp6ImageQuery {
    uintptr_t probe_addr;
    int found;
    int below4g;
};

static int mp6_image_phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    struct Mp6ImageQuery *q = (struct Mp6ImageQuery *)data;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    int contains = 0;
    int i;
    (void)size;

    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t seg_lo, seg_hi;
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        seg_lo = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
        seg_hi = seg_lo + (uintptr_t)ph->p_memsz;
        if (seg_lo < lo) lo = seg_lo;
        if (seg_hi > hi) hi = seg_hi;
        if (q->probe_addr >= seg_lo && q->probe_addr < seg_hi) {
            contains = 1;
        }
    }
    if (contains) {
        q->found = 1;
        q->below4g = (hi != 0 && hi <= 0x100000000ull);
        return 1; /* stop iterating */
    }
    return 0;
}

int mp6_host_image_below_4gb(void)
{
    struct Mp6ImageQuery q;
    q.probe_addr = (uintptr_t)&mp6_host_image_below_4gb;
    q.found = 0;
    q.below4g = 0;
    dl_iterate_phdr(mp6_image_phdr_cb, &q);
    return q.found && q.below4g;
}

/* =======================================================================
 * Paths & filesystem
 * The launcher publishes the on-device base dir as MP6_HOST_BASE;
 * everything resolves under it. The /data/local/tmp/mp6 default matches
 * the adb smoke-test layout so the .so is usable even without the env
 * var.
 * ======================================================================= */

static const char *mp6_android_base(void)
{
    static char base[512];
    static int init;
    if (!init) {
        const char *env = getenv("MP6_HOST_BASE");
        snprintf(base, sizeof(base), "%s", (env && env[0]) ? env : "/data/local/tmp/mp6");
        init = 1;
    }
    return base;
}

int mp6_host_disc_root(char *buf, size_t n)
{
    /* Same contract as the win32 backend: return 0 only for a VERIFIED
     * disc root (the fst.bin openability probe), so the caller
     * (dvd_files.c) keeps its build-time -D fallback otherwise. Layout on
     * device: <base>/GP6E01/{sys/fst.bin,files/...}, mirroring the repo's
     * own orig/GP6E01 shape. */
    char candidateRoot[1024];
    char candidateFst[1024];
    FILE *probe;
    size_t rootLen;

    snprintf(candidateRoot, sizeof(candidateRoot), "%s/GP6E01", mp6_android_base());
    snprintf(candidateFst, sizeof(candidateFst), "%s/sys/fst.bin", candidateRoot);

    probe = fopen(candidateFst, "rb");
    if (!probe) {
        return -1; /* not pushed to this device -- caller keeps its fallback */
    }
    fclose(probe);

    rootLen = strlen(candidateRoot);
    if (rootLen + 1 > n) return -1;
    memcpy(buf, candidateRoot, rootLen + 1);
    return 0;
}

int mp6_host_save_dir(char *buf, size_t n)
{
    int len = snprintf(buf, n, "%s/saves", mp6_android_base());
    return (len > 0 && (size_t)len < n) ? 0 : -1;
}

int mp6_host_pref_dir(char *buf, size_t n)
{
    int len = snprintf(buf, n, "%s", mp6_android_base());
    return (len > 0 && (size_t)len < n) ? 0 : -1;
}

int mp6_host_mkdir(const char *path)
{
    if (mkdir(path, 0755) == 0) return 0;
    return (errno == EEXIST) ? 0 : -1;
}

/* =======================================================================
 * Coroutines -- NOT here by design: platform/host/coro_arena.c (the
 * shared arena-backed minicoro backend, MCO_USE_ASM aarch64) provides
 * mp6_coro_create/switch/destroy on this platform, exactly as it does on
 * Windows. See this file's header comment.
 * ======================================================================= */

/* =======================================================================
 * Mutex / threads -- pthread equivalents of the win32 CRITICAL_SECTION /
 * CreateThread sections (msm mixer lock + opt-in leak-stress threads).
 * ======================================================================= */

/* Mp6Mutex must be able to hold a pthread_mutex_t (40 bytes on bionic
 * LP64; Mp6Mutex.opaque is 64). Same compile-time proof style as the
 * win32 backend's CRITICAL_SECTION check. */
typedef char mp6_host_mutex_size_check[
    (sizeof(pthread_mutex_t) <= sizeof(((Mp6Mutex *)0)->opaque)) ? 1 : -1];

void mp6_host_mutex_init(Mp6Mutex *m)
{
    pthread_mutex_init((pthread_mutex_t *)m->opaque, NULL);
}

void mp6_host_mutex_lock(Mp6Mutex *m)
{
    pthread_mutex_lock((pthread_mutex_t *)m->opaque);
}

void mp6_host_mutex_unlock(Mp6Mutex *m)
{
    pthread_mutex_unlock((pthread_mutex_t *)m->opaque);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} Mp6HostThreadThunk;

static void *mp6_host_thread_entry(void *param)
{
    Mp6HostThreadThunk t = *(Mp6HostThreadThunk *)param;
    free(param);
    t.fn(t.arg);
    return NULL;
}

int mp6_host_thread_start(void (*fn)(void *), void *arg)
{
    Mp6HostThreadThunk *t = (Mp6HostThreadThunk *)malloc(sizeof(*t));
    pthread_t th;
    int err;
    if (!t) {
        return ENOMEM;
    }
    t->fn = fn;
    t->arg = arg;
    err = pthread_create(&th, NULL, mp6_host_thread_entry, t);
    if (err != 0) {
        free(t);
        return err; /* callers include this code in their failure print */
    }
    /* Detached lifetime, mirroring the win32 backend's never-closed HANDLE
     * (these opt-in stress threads run for the life of the process). */
    pthread_detach(th);
    return 0;
}

/* =======================================================================
 * Diagnostics: mp6_host_rss_bytes -- /proc/self/statm field 2 (resident
 * pages) * pagesize. 0 = "can't sample" (callers fail open). NOT
 * threshold-comparable to the win32 working-set numbers; leak gates
 * compare growth only.
 * ======================================================================= */

size_t mp6_host_rss_bytes(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long long vm_pages = 0, rss_pages = 0;
    if (!f) {
        return 0;
    }
    if (fscanf(f, "%llu %llu", &vm_pages, &rss_pages) != 2) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return (size_t)rss_pages * (size_t)sysconf(_SC_PAGESIZE);
}

/* =======================================================================
 * Crash reporting -- see this file's header comment for the design.
 * ======================================================================= */

/* bionic gained backtrace(3)/backtrace_symbols_fd(3) in API 33; the
 * artifact targets android28, so resolve them at runtime -- used where
 * present, honestly skipped on anything older. */
typedef int (*Mp6BacktraceFn)(void **, int);
typedef void (*Mp6BacktraceSymbolsFdFn)(void *const *, int, int);

static void mp6_crash_print_addr(const char *label, const void *addr)
{
    Dl_info di;
    if (addr && dladdr(addr, &di) && di.dli_sname) {
        fprintf(stderr, "  %s %p: %s+0x%llx (%s)\n", label, addr, di.dli_sname,
                (unsigned long long)((uintptr_t)addr - (uintptr_t)di.dli_saddr),
                di.dli_fname ? di.dli_fname : "?");
    } else if (addr && dladdr(addr, &di) && di.dli_fname) {
        fprintf(stderr, "  %s %p: <no symbol> (%s+0x%llx)\n", label, addr, di.dli_fname,
                (unsigned long long)((uintptr_t)addr - (uintptr_t)di.dli_fbase));
    } else {
        fprintf(stderr, "  %s %p: <no symbol>\n", label, addr);
    }
    fflush(stderr);
}

static void mp6_crash_sigaction(int sig, siginfo_t *si, void *uctx)
{
    const void *pc = NULL;
#if defined(__aarch64__)
    if (uctx) {
        pc = (const void *)(uintptr_t)((ucontext_t *)uctx)->uc_mcontext.pc;
    }
#endif
    fprintf(stderr, "\n[MP6-CRASH] fatal signal %d (%s) fault addr %p\n",
            sig, strsignal(sig), si ? si->si_addr : NULL);
    fflush(stderr);
    mp6_crash_print_addr("fault pc @", pc);

    /* Best-effort libc backtrace (runtime-resolved; see typedef comment). */
    {
        Mp6BacktraceFn bt = (Mp6BacktraceFn)dlsym(RTLD_DEFAULT, "backtrace");
        Mp6BacktraceSymbolsFdFn btfd =
            (Mp6BacktraceSymbolsFdFn)dlsym(RTLD_DEFAULT, "backtrace_symbols_fd");
        if (bt && btfd) {
            void *frames[32];
            int n = bt(frames, 32);
            fprintf(stderr, "[MP6-CRASH] backtrace (%d frames):\n", n);
            fflush(stderr);
            btfd(frames, n, fileno(stderr));
        } else {
            fprintf(stderr, "[MP6-CRASH] (no in-process backtrace on this libc -- "
                            "see the debuggerd tombstone / logcat for the full stack)\n");
        }
        fflush(stderr);
    }

    /* Re-raise with the default disposition so the process still dies with
     * the real signal and debuggerd writes its full tombstone. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void mp6_host_crash_install(void)
{
    /* An overflowed coroutine/process stack is the likeliest crash shape
     * this handler will ever see -- run on a dedicated sigaltstack so the
     * report still prints from a dead stack. */
    static char altstack[64 * 1024];
    stack_t ss;
    struct sigaction sa;
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
    size_t i;

    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = mp6_crash_sigaction;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

/* ---------------------------------------------------------------------
 * mp6_symbolize_addr -- the extra exported backend symbol (not one of
 * the seam functions; declared in mp6_boot.h; consumers:
 * malloc_direct.c's opt-in alloc census, aurora_bridge.c's framescope
 * labels -- the latter never links on this target). dladdr genuinely
 * names game functions here since every extern function in libmp6game.so
 * is a dynamic symbol.
 * --------------------------------------------------------------------- */
void mp6_symbolize_addr(void *addr, char *outBuf, size_t outBufSz)
{
    Dl_info di;
    if (outBufSz == 0) {
        return;
    }
    outBuf[0] = '\0';
    if (addr && dladdr(addr, &di) && di.dli_sname) {
        snprintf(outBuf, outBufSz, "%s+0x%llx", di.dli_sname,
                 (unsigned long long)((uintptr_t)addr - (uintptr_t)di.dli_saddr));
    } else {
        snprintf(outBuf, outBufSz, "<no symbol for %p>", addr);
    }
}
