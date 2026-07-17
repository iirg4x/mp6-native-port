/* mp6launcher -- the tiny on-device bootstrap exe for the headless Android
 * build. Establishes the loader shape the whole-image-below-4GB memory
 * model requires:
 *
 *   1. Reserve a low PROT_NONE region for the game image at the arena's
 *      candidate bases (0x10000000 first -- kept symmetric with the
 *      Windows build's --image-base -- then 0x14/0x18/0x1C000000), using
 *      MAP_FIXED_NOREPLACE with a plain-hint fallback for pre-4.17 kernels.
 *   2. android_dlopen_ext(libmp6game.so, RTLD_NOW,
 *      ANDROID_DLEXT_RESERVED_ADDRESS) -- the STRICT flag: the loader
 *      places every PT_LOAD of the game image inside our low reservation
 *      or fails outright. No hint fallback here on purpose: a game image
 *      outside the low 4GB would silently truncate u32<->pointer
 *      round-trips, so refusing to run IS the correct behavior (the
 *      in-.so startup assert, mp6_assert_image_low -> dl_iterate_phdr,
 *      independently double-checks the same invariant from inside).
 *   3. dlsym mp6_headless_main and run it with the tick budget.
 *
 * The GAME ARENA is deliberately NOT reserved here: mp6_host_arena_reserve
 * (platform/host/host_android.c) mmaps it from inside the .so at the same
 * candidate bases arena.c always tries (0x80000000 first), exactly like
 * the Windows build -- the launcher only owns the image region, same
 * split as Windows (linker places image / arena.c places arena).
 *
 * Paths: the game resolves everything under MP6_HOST_BASE (see
 * host_android.c); this launcher defaults it to its OWN directory without
 * overwriting a value the caller already exported.
 *
 * Usage: mp6launcher [ticks] [libmp6game.so path] [base dir]
 *   ticks    default 600
 *   lib path default "<dir of this exe>/libmp6game.so"
 *   base dir default "<dir of this exe>"
 *
 * Every launcher-side line is prefixed [HOST] so a comparison against the
 * Windows headless run can strip platform-tagged lines mechanically and
 * compare game-flow lines only. Exit: the game exits the process itself
 * via exit(0) when the tick budget is reached (mp6_tick_and_maybe_exit),
 * so a normal run never returns here; nonzero exits below are launcher
 * setup failures (2) or loader-shape failures (3).
 */
#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define MP6_SO_RESERVE (256ull * 1024 * 1024) /* headroom over the ~100MB debug .so;
                                               * ends at 0x20000000, clear of every
                                               * arena candidate base */

static const uint64_t kSoBases[] = {
    0x10000000ull, 0x14000000ull, 0x18000000ull, 0x1C000000ull,
};
#define N_SO_BASES (sizeof(kSoBases) / sizeof(kSoBases[0]))

typedef int (*Mp6HeadlessMainFn)(int);

/* Non-clobbering fixed-base attempt (same shape as host_android.c's arena
 * loop): MAP_FIXED_NOREPLACE, with a plain-hint fallback for kernels that
 * ignore the flag. PROT_NONE -- the loader re-maps the region's pages
 * itself. */
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

int main(int argc, char **argv)
{
    char exeDir[512];
    char libPath[1024];
    char baseDir[1024];
    int ticks = 600;
    void *soRegion = MAP_FAILED;
    uint64_t soBase = 0;
    void *handle = NULL;
    Mp6HeadlessMainFn entry = NULL;

    /* Line-buffer stdout so everything up to a crash reaches adb (the .so
     * entry sets the same policy again -- same process stdout, harmless). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* ---- defaults from this exe's own location ------------------------- */
    {
        ssize_t n = readlink("/proc/self/exe", exeDir, sizeof(exeDir) - 1);
        char *slash;
        if (n <= 0) {
            fprintf(stderr, "[HOST] FATAL: readlink(/proc/self/exe): %s\n", strerror(errno));
            return 2;
        }
        exeDir[n] = 0;
        slash = strrchr(exeDir, '/');
        if (slash) *slash = 0;
    }
    if (argc >= 2) ticks = atoi(argv[1]);
    if (argc >= 3) snprintf(libPath, sizeof(libPath), "%s", argv[2]);
    else           snprintf(libPath, sizeof(libPath), "%s/libmp6game.so", exeDir);
    if (argc >= 4) snprintf(baseDir, sizeof(baseDir), "%s", argv[3]);
    else           snprintf(baseDir, sizeof(baseDir), "%s", exeDir);

    if (access(libPath, R_OK) != 0) {
        fprintf(stderr, "[HOST] FATAL: game image not readable: %s (%s)\n", libPath, strerror(errno));
        return 2;
    }

    /* host_android.c resolves disc/save/pref paths under this (does not
     * overwrite a caller-exported value). */
    setenv("MP6_HOST_BASE", baseDir, 0);

    /* Diagnostic lever: MP6_LAUNCH_HIGH_TEST=1 loads the game image with a
     * PLAIN dlopen -- bionic places it at its default HIGH (>=4GB) address
     * -- so the .so's own startup assert (mp6_assert_image_low ->
     * mp6_host_image_below_4gb, dl_iterate_phdr) must FATAL and exit(1)
     * BEFORE any game work. A negative proof that the low-image invariant
     * is enforced, not accidental. */
    if (getenv("MP6_LAUNCH_HIGH_TEST")) {
        printf("[HOST] MP6_LAUNCH_HIGH_TEST: plain dlopen (loader picks a HIGH address) -- "
               "expecting the in-.so image-low assert to FATAL\n");
        fflush(stdout);
        handle = dlopen(libPath, RTLD_NOW);
        if (!handle) {
            fprintf(stderr, "[HOST] FATAL: plain dlopen failed: %s\n", dlerror());
            return 2;
        }
        entry = (Mp6HeadlessMainFn)dlsym(handle, "mp6_headless_main");
        if (!entry) {
            fprintf(stderr, "[HOST] FATAL: dlsym(mp6_headless_main): %s\n", dlerror());
            return 2;
        }
        printf("[HOST]   mp6_headless_main = %p (below4G=%s)\n", (void *)entry,
               (uintptr_t)entry < 0x100000000ull ? "yes" : "NO");
        fflush(stdout);
        return entry(ticks); /* expected: exit(1) inside the assert */
    }

    printf("[HOST] mp6launcher: headless boot (whole-image-below-4GB loader shape)\n");
    printf("[HOST]   lib=%s\n", libPath);
    printf("[HOST]   base=%s ticks=%d pagesize=%ld\n", getenv("MP6_HOST_BASE"), ticks,
           sysconf(_SC_PAGESIZE));

    /* ---- (1) reserve the low image region ------------------------------ */
    {
        size_t i;
        for (i = 0; i < N_SO_BASES; i++) {
            const char *how = "?";
            soRegion = try_reserve_exact(kSoBases[i], MP6_SO_RESERVE, &how);
            printf("[HOST]   image region base 0x%09llx: %s (%s)\n",
                   (unsigned long long)kSoBases[i],
                   soRegion != MAP_FAILED ? "RESERVED" : "unavailable", how);
            if (soRegion != MAP_FAILED) { soBase = kSoBases[i]; break; }
        }
    }
    if (soRegion == MAP_FAILED) {
        fprintf(stderr, "[HOST] FATAL: no low image region reservable (all %d candidate bases) -- "
                        "the whole-image-below-4GB loader shape is unavailable on this device\n",
                (int)N_SO_BASES);
        return 3;
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
        fprintf(stderr, "[HOST] FATAL: android_dlopen_ext(RESERVED_ADDRESS @0x%09llx, %lluMB) "
                        "failed: %s\n",
                (unsigned long long)soBase, (unsigned long long)(MP6_SO_RESERVE >> 20), dlerror());
        return 3;
    }
    printf("[HOST]   android_dlopen_ext(RESERVED_ADDRESS @0x%09llx): OK\n",
           (unsigned long long)soBase);

    /* ---- (3) resolve + run the game ------------------------------------ */
    entry = (Mp6HeadlessMainFn)dlsym(handle, "mp6_headless_main");
    if (!entry) {
        fprintf(stderr, "[HOST] FATAL: dlsym(mp6_headless_main): %s\n", dlerror());
        return 3;
    }
    {
        uint64_t entryAddr = (uint64_t)(uintptr_t)entry;
        int inRegion = entryAddr >= soBase && entryAddr < soBase + MP6_SO_RESERVE;
        printf("[HOST]   mp6_headless_main = 0x%09llx (below4G=%s, inside reservation=%s)\n",
               (unsigned long long)entryAddr,
               entryAddr < 0x100000000ull ? "yes" : "NO", inRegion ? "yes" : "NO");
        if (!inRegion) {
            /* Should be impossible under the strict flag -- refuse rather
             * than run with a high image (see file header). */
            fprintf(stderr, "[HOST] FATAL: game image landed outside the low reservation\n");
            return 3;
        }
    }
    printf("[HOST] handing off to mp6_headless_main(%d)\n", ticks);
    fflush(stdout);

    /* The game exits the process itself (exit(0) at the tick budget); a
     * return here would mean GameMain returned unexpectedly -- pass it up. */
    return entry(ticks);
}
