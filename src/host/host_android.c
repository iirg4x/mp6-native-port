/* MP6 native port -- the Android (bionic/posix) host backend (see
 * src/host/host.h for the seam contract; win32 backend:
 * host_win32.c).
 *
 * Replacement mechanisms: clock_gettime(CLOCK_MONOTONIC), nanosleep,
 * localtime_r, an mmap candidate-base loop mirroring the win32
 * VirtualAlloc loop byte-for-byte in policy (same bases, same order, same
 * [WARN] fallback text), dl_iterate_phdr for the image-below-4GB
 * invariant, pthread mutex/thread, and /proc/self/statm for RSS.
 *
 * The COROUTINE backend is deliberately NOT here: Android always uses the
 * shared arena-backed minicoro backend (src/host/coro_arena.c --
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
 * Paths: the launcher (src/android/mp6launcher.c) publishes the
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
#include "mp6_path.h"
#include "host_file_posix.h"
#include "host_precise_sleep.h"

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

/* Carved out of savestates (include/mp6_host_section.h): this file's statics
 * are the Android host's own (paths, sampler, crash state, the layout table). */
#include "mp6_host_section.h"

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

static __thread Mp6PreciseSleep t_preciseSleep;

static void mp6_android_wait_until(int64_t targetNs)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(targetNs / 1000000000ll);
    ts.tv_nsec = (long)(targetNs % 1000000000ll);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

int mp6_host_sleep_until_ns(uint64_t deadlineNs)
{
    /* Off by default: sleeping inside the tick lets the S22+'s governor drop
     * clocks (game logic went from 3.3 to 4.9 ms a tick), so the Android
     * throttle keeps the core busy for the last ~2 ms. MP6_PRECISE_SLEEP=1
     * turns it on for A/B. */
    if (!mp6_precise_sleep_enabled(0)) return 0;
    mp6_precise_sleep(&t_preciseSleep, (int64_t)deadlineNs, mp6_android_wait_until);
    return 1;
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

void *mp6_host_arena_reserve(uintptr_t preferredBase, size_t size)
{
    /* Same candidate list, same order, same rationale as host_win32.c
     * (0x8xxxxxxx first: game/memory.c's BLOCK_CHECK_BROKEN wants the
     * high bit -- the full comment travels with the win32 copy), and the
     * same preferredBase-before-the-list precedence (host.h explains why
     * the coroutine pool names one). */
    static const uintptr_t kCandidateBases[] = {
        0x80000000u, 0x90000000u, 0xA0000000u, 0xB0000000u,
        0x40000000u, 0x50000000u, 0x60000000u, 0x70000000u, 0x20000000u, 0x30000000u,
    };
    size_t i;
    void *got = NULL;

    if (preferredBase != 0) {
        got = mp6_try_fixed_low(preferredBase, size);
        if (got && !mp6_host_range_below_4gb(got, size)) {
            munmap(got, size);
            got = NULL;
        }
    }
    for (i = 0; !got && i < sizeof(kCandidateBases) / sizeof(kCandidateBases[0]); i++) {
        got = mp6_try_fixed_low(kCandidateBases[i], size);
        if (got && mp6_host_range_below_4gb(got, size)) break;
        if (got) {
            munmap(got, size);
            got = NULL;
        }
    }
    if (!got) {
        got = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (got == MAP_FAILED) {
            return NULL; /* caller (arena.c / coro_arena.c) keeps its own FATAL policy */
        }
        if (!mp6_host_range_below_4gb(got, size)) {
            fprintf(stderr,
                    "[ERROR] mp6_host_arena_reserve: OS-picked range %p (+%zu bytes) is not "
                    "entirely below 4GB; refusing it\n",
                    got, size);
            munmap(got, size);
            return NULL;
        }
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

/* Savestates: the game's own statics, as ranges of this image. The .so links
 * SDL, Aurora and libc++ statically, so its writable segments hold their
 * state beside the game's, and restoring theirs from another process would
 * corrupt the running one. tools/build.py writes the data and bss of the
 * port's own objects (minus the carve-out, include/mp6_host_section.h) from
 * the link map into this table, image-relative; the fill pattern reads as
 * "no table" (fail closed) if it was not patched. The image itself sits at
 * the fixed low address mp6shell.c reserves, which a restore re-checks. */
#define MP6_ANDROID_STATE_RANGES_MAX 32
#define MP6_FILL4 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u
#define MP6_FILL16 MP6_FILL4, MP6_FILL4, MP6_FILL4, MP6_FILL4
__attribute__((used)) static volatile uint32_t g_stateRanges[4 + 2 * MP6_ANDROID_STATE_RANGES_MAX] = {
    0x5353364Du, 0x454C4241u, /* "M6SSABLE": the patcher's search key */
    0xA5A5A5A5u, 0xA5A5A5A5u, MP6_FILL16, MP6_FILL16, MP6_FILL16, MP6_FILL16};

int mp6_host_image_writable_sections(Mp6HostImageSection *out, int maxOut)
{
    uint32_t count = g_stateRanges[2], i;
    Dl_info info;
    if (g_stateRanges[3] != 0x7AB1E000u || count == 0 || count > MP6_ANDROID_STATE_RANGES_MAX ||
        (int)count > maxOut || !dladdr((const void *)&mp6_host_image_writable_sections, &info) ||
        info.dli_fbase == NULL) {
        return 0; /* not patched: savestates unsupported (fail closed) */
    }
    for (i = 0; i < count; i++) {
        uint32_t start = g_stateRanges[4 + 2 * i], end = g_stateRanges[5 + 2 * i];
        if (end <= start) return 0;
        snprintf(out[i].name, sizeof(out[i].name), "data%u", (unsigned)i);
        out[i].addr = (uint8_t *)info.dli_fbase + start;
        out[i].size = end - start;
    }
    return (int)count;
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
    static char base[1200];
    static int init;
    static int valid;
    if (!init) {
        const char *env = getenv("MP6_HOST_BASE");
        valid = mp6_path_copy_checked(base, sizeof(base),
                                      (env && env[0]) ? env : "/data/local/tmp/mp6") == 0;
        if (!valid) {
            fprintf(stderr, "[HOST] MP6_HOST_BASE is too long -- Android host paths disabled\n");
        }
        init = 1;
    }
    return valid ? base : NULL;
}

int mp6_host_disc_root(char *buf, size_t n)
{
    /* Same contract as the win32 backend: return 0 only for a VERIFIED
     * disc root (the fst.bin openability probe), so the caller
     * (dvd_files.c) keeps its build-time -D fallback otherwise. Layout on
     * device: <base>/GP6E01/{sys/fst.bin,files/...}, mirroring the repo's
     * own orig/GP6E01 shape. */
    char candidateRoot[1200];
    char candidateFst[1200];
    const char *base = mp6_android_base();
    FILE *probe;

    if (base == NULL ||
        mp6_path_join_checked(candidateRoot, sizeof(candidateRoot), base, "GP6E01") != 0 ||
        mp6_path_join_checked(candidateFst, sizeof(candidateFst), candidateRoot,
                              "sys/fst.bin") != 0) {
        return -1;
    }

    probe = fopen(candidateFst, "rb");
    if (!probe) {
        return -1; /* not pushed to this device -- caller keeps its fallback */
    }
    fclose(probe);

    return mp6_path_copy_checked(buf, n, candidateRoot);
}

int mp6_host_save_dir(char *buf, size_t n)
{
    /* MP6_SAVE_DIR: a different memory-card folder for QA runs, so a device
     * test never touches the player's own saves. */
    const char *qa = getenv("MP6_SAVE_DIR");
    if (qa != NULL && qa[0] != '\0') return mp6_path_copy_checked(buf, n, qa);
    const char *base = mp6_android_base();
    return base ? mp6_path_join_checked(buf, n, base, "saves") : -1;
}

int mp6_host_pref_dir(char *buf, size_t n)
{
    const char *base = mp6_android_base();
    return base ? mp6_path_copy_checked(buf, n, base) : -1;
}

int mp6_host_mkdir(const char *path)
{
    struct stat st;
    if (path == NULL || path[0] == '\0') return -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return -1;
    return 0;
}

uint64_t mp6_host_process_id(void)
{
    return (uint64_t)(unsigned long)getpid();
}


int mp6_host_sync_file_path(const char *path)
{
    int fd, ok;
    if (path == NULL) return -1;
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    ok = fsync(fd) == 0 ? 0 : -1;
    if (close(fd) != 0) ok = -1;
    return ok;
}

int mp6_host_atomic_replace_file(const char *replacement, const char *destination)
{
    char parent[1200];
    char *slash;
    int dirfd, ok = 0;
    if (replacement == NULL || destination == NULL || destination[0] == '\0' ||
        mp6_path_copy_checked(parent, sizeof(parent), destination) != 0) return -1;
    slash = strrchr(parent, '/');
    if (slash == NULL) {
        if (mp6_path_copy_checked(parent, sizeof(parent), ".") != 0) return -1;
    } else if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    /* POSIX rename is atomic and replaces an existing non-directory target
     * without exposing a delete-then-create window. Both savestate names are
     * sibling paths, so EXDEV is not a supported/possible success case. */
    if (rename(replacement, destination) != 0) return -1;

    /* The file itself was fsync'd before rename. Persist the directory entry
     * too, so a reported-success publish survives power loss. If this step
     * fails the rename has already happened and cannot be rolled back safely;
     * return failure honestly while leaving the atomically-published file. */
    dirfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) return -1;
    if (fsync(dirfd) != 0) ok = -1;
    if (close(dirfd) != 0) ok = -1;
    return ok;
}

/* =======================================================================
 * Coroutines -- NOT here by design: src/host/coro_arena.c (the
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
    pthread_mutex_t lock;
    pthread_cond_t ready;
    unsigned count;
} Mp6HostSem;

typedef char mp6_host_sem_size_check[
    (sizeof(Mp6HostSem) <= sizeof(((Mp6Sem *)0)->opaque)) ? 1 : -1];

int mp6_host_sem_init(Mp6Sem *s)
{
    Mp6HostSem *h = (Mp6HostSem *)s->opaque;
    h->count = 0;
    return pthread_mutex_init(&h->lock, NULL) == 0 && pthread_cond_init(&h->ready, NULL) == 0;
}

void mp6_host_sem_post(Mp6Sem *s)
{
    Mp6HostSem *h = (Mp6HostSem *)s->opaque;
    pthread_mutex_lock(&h->lock);
    h->count++;
    pthread_cond_signal(&h->ready);
    pthread_mutex_unlock(&h->lock);
}

void mp6_host_sem_wait(Mp6Sem *s)
{
    Mp6HostSem *h = (Mp6HostSem *)s->opaque;
    pthread_mutex_lock(&h->lock);
    while (h->count == 0) pthread_cond_wait(&h->ready, &h->lock);
    h->count--;
    pthread_mutex_unlock(&h->lock);
}

int mp6_host_cpu_count(void)
{
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
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
 * Read-only content files (host_file_posix.h)
 * ======================================================================= */

struct Mp6HostFile {
    int fd;
};

Mp6HostFile *mp6_host_file_open(const char *utf8Path)
{
    Mp6HostFile *f;
    int fd = mp6_posix_file_open(utf8Path);
    if (fd < 0) return NULL;
    f = (Mp6HostFile *)malloc(sizeof(*f));
    if (f == NULL) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    return f;
}

int64_t mp6_host_file_size(Mp6HostFile *f)
{
    return f != NULL ? mp6_posix_file_size(f->fd) : -1;
}

int64_t mp6_host_file_read_at(Mp6HostFile *f, void *dst, size_t len, uint64_t offset)
{
    return f != NULL ? mp6_posix_file_read_at(f->fd, dst, len, offset) : -1;
}

void mp6_host_file_close(Mp6HostFile *f)
{
    if (f == NULL) return;
    close(f->fd);
    free(f);
}

int mp6_host_file_prefetch(const char *utf8Path, uint64_t offset, uint64_t len)
{
    (void)utf8Path;
    (void)offset;
    (void)len;
    return 0; /* app storage: the kernel page cache already reads ahead */
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

/* =======================================================================
 * Game-thread sampling profiler (MP6_SAMPLE_PROFILE=<firstTick>-<lastTick>),
 * the Android side of host_win32.c's. simpleperf cannot open perf events on
 * the S22+, so a 1 ms SIGPROF timer aimed at the game thread records the
 * interrupted pc and a frame-pointer walk of its callers. When the window
 * ends a helper thread prints, per library, the most frequent frames as
 * library-relative offsets; tools/android_sample_report.py turns those into
 * function names with the unstripped libmp6game.so. Unset, nothing starts.
 * ======================================================================= */
#include <dlfcn.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>

#define MP6_ASAMPLE_MAX (1 << 18)
#define MP6_ASAMPLE_FRAMES 8

typedef struct {
    uintptr_t pc[MP6_ASAMPLE_FRAMES];
    long tick;
} Mp6AndroidSample;

extern long mp6_tick_count;
static Mp6AndroidSample *g_asamples;
static volatile int g_asampleCount;
static long g_asampleFirst, g_asampleLast;
static timer_t g_asampleTimer;

static void mp6_asample_handler(int sig, siginfo_t *info, void *uctx)
{
    const ucontext_t *uc = (const ucontext_t *)uctx;
    long tick = mp6_tick_count;
    Mp6AndroidSample *sample;
    uintptr_t fp, sp;
    int f;
    (void)sig;
    (void)info;
    if (tick < g_asampleFirst || tick > g_asampleLast || g_asampleCount >= MP6_ASAMPLE_MAX) return;
    sample = &g_asamples[g_asampleCount];
    memset(sample, 0, sizeof(*sample));
    sample->tick = tick;
#if defined(__aarch64__)
    sample->pc[0] = (uintptr_t)uc->uc_mcontext.pc;
    sample->pc[1] = (uintptr_t)uc->uc_mcontext.regs[30]; /* lr: the caller of a leaf */
    fp = (uintptr_t)uc->uc_mcontext.regs[29];
    sp = (uintptr_t)uc->uc_mcontext.sp;
    /* Walk saved {fp, lr} pairs, only upward and within 1 MB of sp (game
     * code runs on coroutine stacks, so there is no fixed stack range). */
    for (f = 2; f < MP6_ASAMPLE_FRAMES && fp >= sp && fp < sp + (1u << 20) && !(fp & 15); f++) {
        uintptr_t next = ((const uintptr_t *)fp)[0];
        sample->pc[f] = ((const uintptr_t *)fp)[1];
        if (next <= fp) break;
        fp = next;
    }
#else
    (void)uc; (void)fp; (void)sp; (void)f;
#endif
    g_asampleCount++;
}

typedef struct {
    uintptr_t pc;
    int count;
} Mp6AsampleCount;

static int mp6_asample_cmp(const void *a, const void *b)
{
    int ca = ((const Mp6AsampleCount *)a)->count, cb = ((const Mp6AsampleCount *)b)->count;
    return (ca < cb) - (ca > cb);
}

/* Prints the 200 most frequent pcs at frame depth 0 ("self") and, for
 * time spent outside libmp6game.so, the first frame inside it ("game"). */
static void mp6_asample_print(const char *kind, int game)
{
    static Mp6AsampleCount counts[16384];
    Dl_info self;
    int n = 0, i, k;
    dladdr((void *)mp6_asample_print, &self);
    for (i = 0; i < g_asampleCount; i++) {
        uintptr_t pc = g_asamples[i].pc[0];
        if (game) {
            int f;
            Dl_info info;
            if (dladdr((void *)pc, &info) && info.dli_fbase == self.dli_fbase) continue;
            pc = 0;
            for (f = 1; f < MP6_ASAMPLE_FRAMES && g_asamples[i].pc[f]; f++) {
                if (dladdr((void *)g_asamples[i].pc[f], &info) && info.dli_fbase == self.dli_fbase) {
                    pc = g_asamples[i].pc[f] - 4; /* the call, not the return address */
                    break;
                }
            }
            if (!pc) continue;
        }
        for (k = 0; k < n && counts[k].pc != pc; k++) {}
        if (k == n) {
            if (n == (int)(sizeof(counts) / sizeof(counts[0]))) continue;
            counts[n].pc = pc;
            counts[n++].count = 0;
        }
        counts[k].count++;
    }
    qsort(counts, (size_t)n, sizeof(counts[0]), mp6_asample_cmp);
    for (k = 0; k < n && k < 200; k++) {
        Dl_info info;
        const char *lib = "?";
        uintptr_t ofs = counts[k].pc;
        if (dladdr((void *)counts[k].pc, &info) && info.dli_fname) {
            const char *slash = strrchr(info.dli_fname, '/');
            lib = slash ? slash + 1 : info.dli_fname;
            ofs = counts[k].pc - (uintptr_t)info.dli_fbase;
        }
        printf("[MP6-SAMPLE] %s %d %s 0x%lx\n", kind, counts[k].count, lib, (unsigned long)ofs);
    }
}

/* MP6_SAMPLE_THREAD=render samples Aurora's render worker instead: its
 * thread id exists only once the renderer has started. */
static int g_asampleRender;

/* Aurora's (render_worker.cpp) when linked; the headless build has none. */
__attribute__((weak)) int aurora_render_worker_tid(void) { return 0; }

static int mp6_asample_arm(pid_t tid)
{
    struct sigevent sev;
    struct itimerspec its;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev.sigev_notify_thread_id = tid;
    if (timer_create(CLOCK_MONOTONIC, &sev, &g_asampleTimer) != 0) {
        fprintf(stderr, "[MP6-SAMPLE] could not create the sampling timer\n");
        return 0;
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = its.it_interval.tv_nsec = 1000000;
    timer_settime(g_asampleTimer, 0, &its, NULL);
    return 1;
}

static void mp6_asample_reporter(void *unused)
{
    (void)unused;
    if (g_asampleRender) {
        int tid;
        while ((tid = aurora_render_worker_tid()) == 0 && mp6_tick_count <= g_asampleLast) usleep(10000);
        if (tid == 0 || !mp6_asample_arm((pid_t)tid)) return;
    }
    while (mp6_tick_count <= g_asampleLast) usleep(100000);
    timer_delete(g_asampleTimer);
    usleep(10000);
    printf("[MP6-SAMPLE] ticks %ld-%ld: %d samples (1 ms each)\n", g_asampleFirst, g_asampleLast,
           g_asampleCount);
    {
        /* Ticks that took more than 25 ms of game-thread time, as on Windows. */
        long span = g_asampleLast - g_asampleFirst + 1, t;
        int *perTick = (int *)calloc((size_t)span, sizeof(int));
        int i;
        if (perTick) {
            for (i = 0; i < g_asampleCount; i++) perTick[g_asamples[i].tick - g_asampleFirst]++;
            for (t = 0; t < span; t++) {
                if (perTick[t] > 25) printf("[MP6-SAMPLE] hitch tick %ld ~%d ms\n", g_asampleFirst + t, perTick[t]);
            }
            free(perTick);
        }
    }
    mp6_asample_print("self", 0);
    mp6_asample_print("game", 1);
    printf("[MP6-SAMPLE] end\n");
    fflush(stdout);
}

static void mp6_asample_start(void)
{
    const char *e = getenv("MP6_SAMPLE_PROFILE");
    const char *which = getenv("MP6_SAMPLE_THREAD");
    struct sigaction sa;
    if (!e || !e[0]) return;
    if (sscanf(e, "%ld-%ld", &g_asampleFirst, &g_asampleLast) != 2 || g_asampleLast < g_asampleFirst) {
        fprintf(stderr, "[MP6-SAMPLE] MP6_SAMPLE_PROFILE must be <firstTick>-<lastTick>\n");
        return;
    }
    g_asamples = (Mp6AndroidSample *)calloc(MP6_ASAMPLE_MAX, sizeof(Mp6AndroidSample));
    if (!g_asamples) return;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = mp6_asample_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    g_asampleRender = which != NULL && strcmp(which, "render") == 0;
    if (!g_asampleRender && !mp6_asample_arm((pid_t)syscall(SYS_gettid))) return; /* the game thread */
    mp6_host_thread_start(mp6_asample_reporter, NULL);
    printf("[MP6-SAMPLE] sampling the %s thread for ticks %ld-%ld\n",
           g_asampleRender ? "render worker" : "game", g_asampleFirst, g_asampleLast);
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
    /* Both mains call this first thing on the game thread. */
    mp6_asample_start();
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
