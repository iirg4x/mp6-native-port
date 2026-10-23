/* MP6 native port -- the WebAssembly (Emscripten) host backend (see
 * src/host/host.h for the seam contract; siblings: host_win32.c,
 * host_android.c).
 *
 * wasm32 satisfies the low-4GB memory model by construction: every
 * pointer is 32 bits, so code, static data, the arena and the coroutine
 * pool are all "below 4GB" no matter where they land. Fixed bases are
 * meaningless in linear memory, so the arena and coroutine pool come from
 * the ordinary allocator. Fresh linear memory is zero-filled and these
 * reservations happen at boot before anything large is freed, matching
 * the demand-zero pages the other backends get from VirtualAlloc/mmap.
 *
 * Time: mp6_host_sleep_ns yields to the browser event loop through
 * emscripten_sleep (requires -sASYNCIFY, which the coroutine backend needs
 * anyway). That is what keeps the page responsive while GameMain's
 * infinite loop runs.
 *
 * Paths: everything resolves under MP6_HOST_BASE (default "/mp6"), the
 * mount point the web shell populates (NODERAWFS host paths under node).
 *
 * Threads: none. The build is single-threaded (no SharedArrayBuffer, so
 * it runs on pages that are not cross-origin isolated); the mutex calls
 * are emscripten's single-thread pthread stubs and thread_start reports
 * ENOSYS to its opt-in stress callers.
 */
#include "host.h"
#include "mp6_path.h"
#include "mp6_boot.h"
#include "mp6_utf8_file.h"
#include "mp6_web_content.h"
#include "host_file_posix.h"
#include "host_precise_sleep.h"
#include "mp6_coro_layout.h"
#include "mp6_heap_scale.h"

#include <emscripten.h>
#include <emscripten/heap.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Carved out of savestates (include/mp6_host_section.h): this file's statics
 * are the browser host's own (content caches, paths, the layout table). */
#include "mp6_host_section.h"

int mp6_host_init(void)
{
    return 1;
}

uint64_t mp6_host_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One turn of the browser's event loop: a message-channel task runs after
 * the tasks already queued, without a timer's minimum delay. */
EM_ASYNC_JS(void, mp6_web_yield, (void), {
    await new Promise((resolve) => {
        const channel = new MessageChannel();
        channel.port1.onmessage = () => resolve();
        channel.port2.postMessage(0);
    });
});

/* Wait for the next animation frame or `ms`, whichever comes first. */
EM_ASYNC_JS(void, mp6_web_wait, (double ms), {
    await new Promise((resolve) => {
        const timer = setTimeout(resolve, ms);
        /* ?uncapped (benchmark): wait out the time only, not the display. */
        if (!Module.mp6Uncapped) requestAnimationFrame(() => { clearTimeout(timer); resolve(); });
    });
});

void mp6_host_sleep_ns(uint64_t ns)
{
    /* Each wait unwinds and rewinds the whole game call stack (ASYNCIFY),
     * so tiny waits return at once and let the caller's clock check spin
     * briefly. Longer ones hand the thread back to the browser until the
     * next animation frame (or the requested time), so a 60 Hz game tick on
     * a faster display neither spins the main thread nor misses input and
     * audio callbacks. */
    if (ns >= 3000000ull) {
        mp6_web_wait((double)ns / 1000000.0);
    } else if (ns == 0) {
        /* "Yield to any ready thread": here the only other work is the
         * browser's own -- GPU completion callbacks a caller retrying on
         * queue pressure is waiting for, input, audio. Polling without
         * yielding would wait on the very events it starves. */
        mp6_web_yield();
    }
}

/* Hand the thread back to the browser for `ms` (a timer, not the next
 * animation frame: the caller has an exact deadline). */
EM_ASYNC_JS(void, mp6_web_timer_wait, (double ms), {
    await new Promise((resolve) => setTimeout(resolve, ms));
});


static void mp6_web_wait_until(int64_t targetNs)
{
    int64_t wait = targetNs - (int64_t)mp6_host_monotonic_ns();
    if (wait > 0) mp6_web_timer_wait((double)wait / 1000000.0);
}

#define MP6_WEB_TIMER_MIN_NS   5000000 /* a wait this long is not hit by the 4 ms timer clamp */
#define MP6_WEB_TIMER_EARLY_NS 1000000 /* typical browser timer lateness */

int mp6_host_sleep_until_ns(uint64_t deadlineNs)
{
    /* Native hosts aim a timer early and spin the last stretch; here that
     * spin polled the clock on the page's only thread for up to 2 ms of every
     * tick. Browsers clamp repeated timers to at least 4 ms, though, so a
     * timer alone cannot keep a 240 Hz Quick Party tick (4.2 ms). Timers cover
     * the part of the wait the clamp cannot distort (aiming 1 ms early), and
     * MessageChannel yields -- which hand the thread to the browser on every
     * turn rather than spin -- cover the rest. */
    if (!mp6_precise_sleep_enabled(1)) return 0;
    for (;;) {
        const int64_t remain = (int64_t)deadlineNs - (int64_t)mp6_host_monotonic_ns();
        if (remain <= 0) break;
        if (remain >= MP6_WEB_TIMER_MIN_NS) {
            mp6_web_wait_until((int64_t)deadlineNs - MP6_WEB_TIMER_EARLY_NS);
        } else {
            mp6_web_yield();
        }
    }
    return 1;
}

void mp6_host_wallclock(Mp6DateTime *out)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    out->year = tm.tm_year + 1900;
    out->mon  = tm.tm_mon + 1;
    out->day  = tm.tm_mday;
    out->hour = tm.tm_hour;
    out->min  = tm.tm_min;
    out->sec  = tm.tm_sec;
    out->msec = (int)(ts.tv_nsec / 1000000);
}

/* Savestates (dev builds). A state is raw memory, so a restoring run must
 * place the game arena and the coroutine stacks at the addresses the
 * capturing run used. Linear memory has no fixed bases, but its allocator is
 * deterministic until the first input-dependent allocation, and constructors
 * run before any: reserving both here, first, gives every run of a build the
 * same addresses whatever happens in the launcher before Play. The web
 * build always runs the x1 arena (mp6_heap_scale.h). */

void *mp6_host_arena_reserve(uintptr_t preferredBase, size_t size)
{
    (void)preferredBase; /* no fixed placement in linear memory */
    return aligned_alloc(65536, (size + 65535u) & ~(size_t)65535u);
}

void *mp6_host_web_coro_pool(void)
{
    return NULL;
}

void *mp6_host_web_coro_slot(int slot)
{
    static uint8_t *slots[MP6_CORO_MAX_SLOTS];
    if (slot < 0 || slot >= MP6_CORO_MAX_SLOTS) return NULL;
    if (!slots[slot]) {
        uint8_t *pool = (uint8_t *)mp6_host_web_coro_pool();
        slots[slot] = (pool && slot < MP6_WEB_CORO_FIXED_SLOTS)
                          ? pool + (size_t)slot * MP6_CORO_SLOT_SIZE
                          : (uint8_t *)aligned_alloc(65536, MP6_CORO_SLOT_SIZE);
    }
    return slots[slot];
}

int mp6_host_image_below_4gb(void)
{
    return sizeof(void *) == 4;
}

/* The game's own statics, as address ranges of linear memory. Everything in
 * one wasm image shares its data segment -- libc's allocator, SDL, Aurora
 * and the browser host sit beside the game's globals -- so the ranges come
 * from the link map: tools/build_wasm.py writes the data and bss of the
 * port's own objects (minus the carve-out, include/mp6_host_section.h) into
 * this table in the linked .wasm. The fill pattern keeps the table in the
 * data segment and reads as "no table" (fail closed) if it was not patched. */
#define MP6_WEB_STATE_RANGES_MAX 32
#define MP6_FILL4 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u
#define MP6_FILL16 MP6_FILL4, MP6_FILL4, MP6_FILL4, MP6_FILL4
__attribute__((used)) static volatile uint32_t g_stateRanges[4 + 2 * MP6_WEB_STATE_RANGES_MAX] = {
    0x5353364Du, 0x454C4241u, /* "M6SSABLE": the patcher's search key */
    0xA5A5A5A5u, 0xA5A5A5A5u, MP6_FILL16, MP6_FILL16, MP6_FILL16, MP6_FILL16};

int mp6_host_image_writable_sections(Mp6HostImageSection *out, int maxOut)
{
    uint32_t count = g_stateRanges[2], i;
    if (g_stateRanges[3] != 0x7AB1E000u || count == 0 || count > MP6_WEB_STATE_RANGES_MAX ||
        (int)count > maxOut) {
        return 0; /* not patched: savestates unsupported (fail closed) */
    }
    for (i = 0; i < count; i++) {
        uint32_t start = g_stateRanges[4 + 2 * i], end = g_stateRanges[5 + 2 * i];
        if (end <= start) return 0;
        snprintf(out[i].name, sizeof(out[i].name), "data%u", (unsigned)i);
        out[i].addr = (void *)(uintptr_t)start;
        out[i].size = end - start;
    }
    return (int)count;
}


static const char *mp6_web_base(void)
{
    static char base[1200];
    static int init;
    static int valid;
    if (!init) {
        const char *env = getenv("MP6_HOST_BASE");
        valid = mp6_path_copy_checked(base, sizeof(base), (env && env[0]) ? env : "/mp6") == 0;
        /* Saves and settings live under the base; in the browser it starts
         * out as an empty in-memory directory (not persisted yet). */
        if (valid) mkdir(base, 0755);
        init = 1;
    }
    return valid ? base : NULL;
}

int mp6_host_disc_root(char *buf, size_t n)
{
    char candidateRoot[1200];
    char candidateFst[1200];
    const char *base = mp6_web_base();
    FILE *probe;

    if (base == NULL ||
        mp6_path_join_checked(candidateRoot, sizeof(candidateRoot), base, "GP6E01") != 0 ||
        mp6_path_join_checked(candidateFst, sizeof(candidateFst), candidateRoot,
                              "sys/fst.bin") != 0) {
        return -1;
    }
    probe = mp6_fopen_utf8(candidateFst, "rb");
    if (!probe) {
        return -1;
    }
    fclose(probe);
    return mp6_path_copy_checked(buf, n, candidateRoot);
}

int mp6_host_save_dir(char *buf, size_t n)
{
    const char *base = mp6_web_base();
    return base ? mp6_path_join_checked(buf, n, base, "saves") : -1;
}

int mp6_host_pref_dir(char *buf, size_t n)
{
    const char *base = mp6_web_base();
    return base ? mp6_path_copy_checked(buf, n, base) : -1;
}

int mp6_host_mkdir(const char *path)
{
    struct stat st;
    if (path == NULL || path[0] == '\0') return -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    return 0;
}

uint64_t mp6_host_process_id(void)
{
    return 1;
}

int mp6_host_sync_file_path(const char *path)
{
    int fd, ok;
    if (path == NULL) return -1;
    fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    ok = fsync(fd) == 0 ? 0 : -1;
    if (close(fd) != 0) ok = -1;
    return ok;
}

int mp6_host_atomic_replace_file(const char *replacement, const char *destination)
{
    if (replacement == NULL || destination == NULL || destination[0] == '\0') return -1;
    return rename(replacement, destination) == 0 ? 0 : -1;
}

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

int mp6_host_thread_start(void (*fn)(void *), void *arg)
{
    (void)fn;
    (void)arg;
    return ENOSYS;
}

/* No worker threads on the web (mp6_host_thread_start above). */
int mp6_host_sem_init(Mp6Sem *s)
{
    (void)s;
    return 0;
}

void mp6_host_sem_post(Mp6Sem *s) { (void)s; }
void mp6_host_sem_wait(Mp6Sem *s) { (void)s; }
int mp6_host_cpu_count(void) { return 1; }

/* Read-only content files: the page's provider for the disc it serves
 * (async reads through ASYNCIFY), host_file_posix.h for everything else
 * (NODERAWFS under node). */
struct Mp6HostFile {
    int fd;          /* -1 for provider content */
    int64_t webSize; /* provider content only */
    char webPath[1];
};

Mp6HostFile *mp6_host_file_open(const char *utf8Path)
{
    Mp6HostFile *f;
    int64_t webSize = -1;
    size_t len;
    if (utf8Path == NULL) return NULL;
    len = strlen(utf8Path);
    if (mp6_web_content_open(utf8Path, &webSize)) {
        if (webSize < 0) return NULL;
        f = (Mp6HostFile *)malloc(sizeof(*f) + len);
        if (f == NULL) return NULL;
        f->fd = -1;
        f->webSize = webSize;
        memcpy(f->webPath, utf8Path, len + 1);
        return f;
    }
    f = (Mp6HostFile *)malloc(sizeof(*f));
    if (f == NULL) return NULL;
    f->fd = mp6_posix_file_open(utf8Path);
    if (f->fd < 0) {
        free(f);
        return NULL;
    }
    f->webSize = -1;
    f->webPath[0] = '\0';
    return f;
}

int64_t mp6_host_file_size(Mp6HostFile *f)
{
    if (f == NULL) return -1;
    return f->fd < 0 ? f->webSize : mp6_posix_file_size(f->fd);
}

int64_t mp6_host_file_read_at(Mp6HostFile *f, void *dst, size_t len, uint64_t offset)
{
    if (f == NULL) return -1;
    if (f->fd < 0) return mp6_web_content_read_at(f->webPath, f->webSize, dst, len, offset);
    return mp6_posix_file_read_at(f->fd, dst, len, offset);
}

void mp6_host_file_close(Mp6HostFile *f)
{
    if (f == NULL) return;
    if (f->fd >= 0) close(f->fd);
    free(f);
}

int mp6_host_file_prefetch(const char *utf8Path, uint64_t offset, uint64_t len)
{
    return mp6_web_content_prefetch(utf8Path, offset, len);
}

size_t mp6_host_rss_bytes(void)
{
    return emscripten_get_heap_size();
}

void mp6_host_crash_install(void)
{
    /* Traps surface as JS exceptions with a wasm stack trace in the
     * console; there is no in-process handler to install. */
}

void mp6_symbolize_addr(void *addr, char *outBuf, size_t outBufSz)
{
    if (outBufSz == 0) {
        return;
    }
    snprintf(outBuf, outBufSz, "<wasm %p>", addr);
}
