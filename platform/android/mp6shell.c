/* mp6shell -- the libmain.so bootstrap the APK's SDL activity loads.
 *
 * Re-hosts mp6launcher.c's whole-image-below-4GB loader shape inside an
 * APK. It differs from the standalone launcher in exactly one way: INSIDE
 * an APK the low-image load must happen before ANY SDL JNI binding exists,
 * because SDL3 is linked STATIC into libmp6game.so itself (aurora's android
 * shape: lib/window.cpp calls SDL-internal Android_Lock/UnlockActivityMutex,
 * which no shared libSDL3.so exports -- the same single-.so shape the MP4
 * port ships). So the whole bootstrap runs from THIS library's JNI_OnLoad,
 * which System.loadLibrary("main") invokes on the activity's class-loader
 * context during Mp6Activity.onCreate:
 *
 *   0. install the stdout/stderr -> logcat pump (inside an APK stdout goes
 *      nowhere; every [BOOT]/[HEAPTRACE]/[DVD]/[AURORA] line lands in
 *      logcat tag "mp6"),
 *   1. reserve the low image region at the candidate bases (VERBATIM
 *      try_reserve_exact/kSoBases from mp6launcher.c),
 *   2. android_dlopen_ext(libmp6game.so, RTLD_NOW,
 *      ANDROID_DLEXT_RESERVED_ADDRESS) -- the STRICT flag, no fallback: a
 *      game image outside the low 4GB must refuse to run (the in-.so
 *      startup assert double-checks the same invariant from inside),
 *   3. chain-call the game image's own JNI_OnLoad (SDL3's, exported
 *      JNIEXPORT from libmp6game.so) with the real JavaVM -- SDL3
 *      registers its natives via FindClass+RegisterNatives (see
 *      SDL_android.c), which binds the org.libsdl.app classes to the
 *      low-loaded game image no matter HOW that image entered the
 *      process. Every later Java->native SDL call, and the eventual
 *      nativeRunMain -> mp6_android_main handoff (platform/
 *      main_native.c), lands in the game .so.
 *
 * Mp6Activity overrides getMainSharedObject() to name libmp6game.so --
 * SDL's nativeRunMain then dlopens it AGAIN, which bionic dedupes to the
 * already-low-loaded module (same soname/inode; refcount bump, no second
 * mapping), and getMainFunction()="mp6_android_main" resolves the game's
 * own windowed entry. Path/env policy lives THERE (main_native.c), not
 * here: this file stays a dumb loader by design.
 */
#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* IN-APP REALITY: unlike a clean exec'd process (no mappings in the low
 * window), a zygote-forked APP process already has ART's
 * compressed-reference heap sitting in the low 4GB -- typically two 256MB
 * dalvik region spaces plus JIT/boot-image clusters scattered below 4GB.
 * A 256MB reservation at 0x10000000 therefore EEXISTs in-app. Two
 * adaptations:
 *   - reserve 64MB, not 256MB: the game image maps ~25MB of VA (debug info
 *     is not mapped), so 64MB keeps 2.5x headroom while fitting the real
 *     inter-space gaps;
 *   - walk more candidate bases, chosen inside the ART free windows while
 *     staying OFF platform/os/arena.c's own candidate list
 *     (0x80/0x90/0xA0/0xB0/0x40/0x50/0x60/0x70/0x20/0x30000000) so the
 *     image never steals an arena base.
 * ART placement varies per boot/process, so this stays an EEXIST-tolerant
 * probe LOOP (the same walk arena.c uses), not a pinned address. */
#define MP6_SO_RESERVE (64ull * 1024 * 1024)

static const uint64_t kSoBases[] = {
    0x10000000ull, /* the symbolic Windows --image-base; free in exec'd
                    * processes, usually EEXIST in-app (dalvik space) */
    0x14000000ull, 0x18000000ull, 0x1C000000ull, /* 0x12000000-0x22000000 gap */
    0x34000000ull, 0x38000000ull, 0x3C000000ull, /* 0x32000000-0x42000000 gap */
    0x4C000000ull, 0x54000000ull, 0x64000000ull, 0x6C000000ull, /* 0x4a-0x705 gap */
    0xC0000000ull, 0xD0000000ull, 0xE0000000ull, /* high free window, above every
                                                  * arena candidate, still <4GB */
};
#define N_SO_BASES (sizeof(kSoBases) / sizeof(kSoBases[0]))

#define LOG_TAG "mp6"
#define SHELL_LOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

/* =======================================================================
 * stdout/stderr -> logcat pump. dup2 both onto a pipe and drain it from a
 * detached reader thread, one logcat line per text line. Installed before
 * the game image loads; the game's own setvbuf(stdout, _IOLBF) then
 * flushes per-line into the pipe, so game-flow lines appear in logcat in
 * order under tag "mp6".
 * ======================================================================= */
static void *mp6_logcat_pump_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    static char line[2048];
    size_t len = 0;
    char buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        ssize_t i;
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || len == sizeof(line) - 1) {
                line[len] = 0;
                if (len) __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
                len = 0;
                if (c != '\n') line[len++] = c;
            } else {
                line[len++] = c;
            }
        }
    }
    return NULL;
}

static void mp6_install_logcat_pump(void)
{
    int fds[2];
    pthread_t t;
    if (pipe(fds) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    if (pthread_create(&t, NULL, mp6_logcat_pump_thread, (void *)(intptr_t)fds[0]) == 0) {
        pthread_detach(t);
    }
}

/* =======================================================================
 * Low image reservation -- VERBATIM from platform/android/mp6launcher.c
 * (see that file's own comments).
 * ======================================================================= */
static void *try_reserve_exact(uint64_t base, uint64_t size, const char **how)
{
    void *want = (void *)(uintptr_t)base;
    void *got = mmap(want, (size_t)size, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == want) { *how = "MAP_FIXED_NOREPLACE"; return got; }
    if (got == MAP_FAILED) {
        if (errno == EEXIST) { *how = "occupied (EEXIST)"; return MAP_FAILED; }
    } else {
        munmap(got, (size_t)size);
        *how = "hint diverted (old kernel, base unavailable)";
        return MAP_FAILED;
    }
    got = mmap(want, (size_t)size, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (got == want) { *how = "plain hint (kernel lacks FIXED_NOREPLACE)"; return got; }
    if (got != MAP_FAILED) {
        munmap(got, (size_t)size);
        *how = "hint diverted (base unavailable)";
    } else {
        *how = "mmap failed";
    }
    return MAP_FAILED;
}

/* =======================================================================
 * JNI_OnLoad -- System.loadLibrary("main") invokes this on the app's
 * class-loader context; everything happens here (see file header).
 * ======================================================================= */
typedef jint (*Mp6JniOnLoadFn)(JavaVM *, void *);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    char libPath[1024];
    void *soRegion = MAP_FAILED;
    uint64_t soBase = 0;
    void *handle = NULL;
    Mp6JniOnLoadFn gameOnLoad = NULL;

    mp6_install_logcat_pump();
    SHELL_LOG("[SHELL] mp6shell: windowed boot (whole-image-below-4GB loader shape)");
    SHELL_LOG("[SHELL]   pagesize=%ld", sysconf(_SC_PAGESIZE));

    /* ---- libmp6game.so sits next to this .so (the APK's nativeLibraryDir) */
    {
        Dl_info info;
        char *slash;
        if (dladdr((void *)&JNI_OnLoad, &info) && info.dli_fname) {
            snprintf(libPath, sizeof(libPath), "%s", info.dli_fname);
            slash = strrchr(libPath, '/');
            if (slash) *slash = 0;
            strncat(libPath, "/libmp6game.so", sizeof(libPath) - strlen(libPath) - 1);
        } else {
            /* Loader-namespace fallback: a bare soname resolves in the
             * app's own nativeLibraryDir. */
            snprintf(libPath, sizeof(libPath), "libmp6game.so");
        }
    }
    SHELL_LOG("[SHELL]   lib=%s", libPath);

    /* ---- (1) reserve the low image region (mp6launcher.c verbatim) ----- */
    {
        size_t b;
        for (b = 0; b < N_SO_BASES; b++) {
            const char *how = "?";
            soRegion = try_reserve_exact(kSoBases[b], MP6_SO_RESERVE, &how);
            SHELL_LOG("[SHELL]   image region base 0x%09llx: %s (%s)",
                      (unsigned long long)kSoBases[b],
                      soRegion != MAP_FAILED ? "RESERVED" : "unavailable", how);
            if (soRegion != MAP_FAILED) { soBase = kSoBases[b]; break; }
        }
    }
    if (soRegion == MAP_FAILED) {
        SHELL_LOG("[SHELL] FATAL: no low image region reservable (all %d candidate bases) -- "
                  "the whole-image-below-4GB loader shape is unavailable in this process",
                  (int)N_SO_BASES);
        return JNI_ERR;
    }

    /* ---- (2) load the game image INTO the reservation ------------------ */
    {
        android_dlextinfo info;
        memset(&info, 0, sizeof(info));
        info.flags = ANDROID_DLEXT_RESERVED_ADDRESS; /* strict -- see file header */
        info.reserved_addr = soRegion;
        info.reserved_size = (size_t)MP6_SO_RESERVE;
        handle = android_dlopen_ext(libPath, RTLD_NOW, &info);
    }
    if (!handle) {
        SHELL_LOG("[SHELL] FATAL: android_dlopen_ext(RESERVED_ADDRESS @0x%09llx, %lluMB) failed: %s",
                  (unsigned long long)soBase, (unsigned long long)(MP6_SO_RESERVE >> 20), dlerror());
        return JNI_ERR;
    }
    SHELL_LOG("[SHELL]   android_dlopen_ext(RESERVED_ADDRESS @0x%09llx): OK",
              (unsigned long long)soBase);

    /* ---- (3) chain the game image's JNI_OnLoad (SDL3's RegisterNatives) - */
    gameOnLoad = (Mp6JniOnLoadFn)dlsym(handle, "JNI_OnLoad");
    if (!gameOnLoad) {
        SHELL_LOG("[SHELL] FATAL: dlsym(JNI_OnLoad in libmp6game.so): %s", dlerror());
        return JNI_ERR;
    }
    {
        uint64_t addr = (uint64_t)(uintptr_t)gameOnLoad;
        int inRegion = addr >= soBase && addr < soBase + MP6_SO_RESERVE;
        SHELL_LOG("[SHELL]   game JNI_OnLoad = 0x%09llx (below4G=%s, inside reservation=%s)",
                  (unsigned long long)addr,
                  addr < 0x100000000ull ? "yes" : "NO", inRegion ? "yes" : "NO");
        if (!inRegion) {
            /* Should be impossible under the strict flag -- refuse rather
             * than run with a high image (see mp6launcher.c's header). */
            SHELL_LOG("[SHELL] FATAL: game image landed outside the low reservation");
            return JNI_ERR;
        }
    }
    SHELL_LOG("[SHELL] chaining libmp6game.so JNI_OnLoad (SDL3 RegisterNatives)");
    {
        jint rc = gameOnLoad(vm, reserved);

        /* aurora's android lifecycle hook (lib/window.cpp):
         * Java_org_libsdl_app_SDLSurface_auroraNativeSetSurfaceReady gates
         * every frame (is_presentable(): g_surfaceReady) and the vendored
         * SDLSurface.java calls it -- but ART's name-based native-method
         * lookup only searches System.loadLibrary'd libraries, and the game
         * image entered via android_dlopen_ext. Register it explicitly on
         * the class, exactly like SDL3 registers its own natives. Without
         * this the first surfaceCreated() throws UnsatisfiedLinkError. */
        JNIEnv *env = NULL;
        if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_4) == JNI_OK && env) {
            void *fn = dlsym(handle, "Java_org_libsdl_app_SDLSurface_auroraNativeSetSurfaceReady");
            jclass cls = (*env)->FindClass(env, "org/libsdl/app/SDLSurface");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            if (fn && cls) {
                JNINativeMethod m;
                m.name = "auroraNativeSetSurfaceReady";
                m.signature = "(Z)V";
                m.fnPtr = fn;
                if ((*env)->RegisterNatives(env, cls, &m, 1) == 0) {
                    SHELL_LOG("[SHELL] registered SDLSurface.auroraNativeSetSurfaceReady -> low game image");
                } else {
                    SHELL_LOG("[SHELL] WARN: RegisterNatives(auroraNativeSetSurfaceReady) failed");
                    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                }
            } else {
                SHELL_LOG("[SHELL] WARN: aurora surface-ready hook unavailable (fn=%p cls=%p)",
                          fn, (void *)cls);
            }
            if (cls) (*env)->DeleteLocalRef(env, cls);
        }
        return rc;
    }
}
