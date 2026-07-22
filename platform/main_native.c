/* MP6 native port -- process entry point.
 *
 * game/main.c's `main()` is renamed to GameMain() at compile time (see
 * tools/build.py's MAIN_C_FLAGS, -Dmain=GameMain, scoped to that one file --
 * the only `main` in the whole decomp slice) so it can coexist with this
 * real process entry point. GameMain() runs the same init chain the
 * original GameCube boot did (HuSysInit -> HuPrcInit -> ... -> omMasterInit
 * -> VIWaitForRetrace -> the infinite per-frame loop that ticks HuPrcCall)
 * -- nothing in it is bypassed or special-cased; every carve-out lives
 * behind the platform/ shim layer instead (arena, REL loader bridge, HuPrc
 * coroutine scheduler, VI tick budget), so GameMain() itself is unmodified
 * decomp control flow.
 *
 * GameMain() never returns (it's a `while(1)`, matching real hardware) --
 * the only way this process exits is a shim calling exit(): the VI
 * tick-budget bridge (mp6_tick_and_maybe_exit, code 0), the aurora frame
 * bridge seeing the window close (also code 0, aurora build only), or
 * OSPanic (code 1) on a genuine fatal condition.
 *
 * The file has two build-time shapes, selected by tools/build.py's
 * --headless flag (-DMP6_HEADLESS_BUILD):
 *   - --headless: no Aurora anywhere in the build;
 *     platform/null/shims_manual.c's own VIInit/VIWaitForRetrace/GXInit
 *     provide the tick-counting behavior. This is a build-time (not
 *     run-time) switch deliberately: Aurora's GX layer is a software FIFO
 *     that is only drained by aurora_end_frame() -- if an Aurora build were
 *     linked but a run-time flag merely skipped aurora_initialize()/
 *     begin_frame/end_frame, every GX call the game still makes would keep
 *     writing into that FIFO with nothing reading it out, an
 *     unbounded-accumulation risk over a long run. A build-time split has
 *     no such risk.
 *   - default (no flag): Aurora owns the window/GPU instance; this file
 *     calls aurora_initialize() BEFORE GameMain() ever reaches the game's
 *     own GXInit() (HuSysInit, the first call inside GameMain, calls
 *     VIInit/PADInit/GXInit in that order -- see src/game/init.c).
 *     <aurora/main.h> renames THIS file's `main` to `aurora_main` (a
 *     macro); the real process entry point the OS invokes is Aurora's own
 *     lib/main.cpp, which does nothing beyond calling aurora_main.
 */
#include "mp6_boot.h"
#include "mp6_widescreen.h" /* mp6_widescreen_set_enabled */
#include "host.h" /* mp6_host_crash_install, mp6_host_image_below_4gb */

#include <stdio.h>
#include <stdlib.h>

extern void GameMain(void); /* game/main.c's `main`, renamed via -Dmain=GameMain */

void mp6_arena_init(void);

/* Fail fast at boot if the executable's own image (code + static data) is
 * not proven to sit entirely below 4 GB. The low-arena model rests on
 * u32<->pointer round-trips for CODE/static addresses too, not just heap
 * data: jmp_buf.lr and OSModuleHeader prolog/epilog store code addresses
 * truncated to u32, and game/memory.c's BLOCK_CHECK sanity-tests the high
 * bit. The image is linked at 0x10000000 with ASLR off (tools/build.py's
 * --image-base=0x10000000 + --no-dynamicbase) so this holds by
 * construction; this assert guards against a future build/link change
 * silently shipping a binary that truncates code pointers at runtime.
 * Called from both mains right after the crash handler installs (so a
 * fault here is symbolized) and before any arena/game work. Silent on
 * success, keeping the boot log byte-stable for the log-diff gates; the
 * seam function returns 0 on query failure too, treated as "cannot prove
 * the invariant" (fail closed). */
static void mp6_assert_image_low(void)
{
    if (!mp6_host_image_below_4gb()) {
        fprintf(stderr,
                "[FATAL] mp6 boot: the game image (code + static data) is NOT proven entirely below "
                "4GB -- the u32<->pointer model requires it (code addresses live in jmp_buf.lr and "
                "OSModuleHeader prolog/epilog; game/memory.c BLOCK_CHECK tests the high bit). Verify "
                "tools/build.py still links with --image-base=0x10000000 and --no-dynamicbase "
                "(ASLR off).\n");
        fflush(stderr);
        exit(1);
    }
}

#ifdef MP6_HEADLESS_BUILD

/* The headless boot body, shared by both process shapes:
 *   - Windows: main() below parses argv and calls this.
 *   - Android: libmp6game.so exports this (every extern symbol in the .so
 *     is dynamic); platform/android/mp6launcher.c android_dlopen_ext()s
 *     the image into a caller-reserved low region, dlsym()s this by name,
 *     and calls it with the tick budget. There is no main() in the .so.
 * ticks <= 0 keeps the mp6_max_ticks default (60), same as a
 * non-numeric argv[1]. */
int mp6_headless_main(int ticks)
{
    /* stdout is fully buffered (not line-buffered) once redirected to a
     * file/pipe -- fine for normal OSReport traffic (shims_manual.c's
     * OSReport fflush()es after every call), but the decomp's own
     * game/process.c has one unflushed printf on its "stack overlap error"
     * canary-check failure path, immediately followed by `while(1);` --
     * that diagnostic would silently vanish if the path were ever hit,
     * making a real bug look like a silent hang. Line-buffering stdout
     * here (platform code, not a decomp edit) guarantees every printf'd
     * line survives even if the process is killed instead of exiting. */
    setvbuf(stdout, NULL, _IOLBF, 4096);
    mp6_host_crash_install();
    mp6_assert_image_low(); /* whole-image-below-4GB invariant */

    if (ticks > 0) mp6_max_ticks = ticks;

    printf("[BOOT] mp6native starting (--headless: null platform, no Aurora) -- tick budget %d\n", mp6_max_ticks);
    fflush(stdout);

    mp6_os_globals_init();
    mp6_arena_init();

    printf("[BOOT] calling GameMain() (game/main.c's main(), renamed)\n");
    fflush(stdout);

    GameMain();

    /* Unreachable in practice -- GameMain() is an infinite loop on real
     * hardware too. Fall through defensively rather than relying on that. */
    printf("[BOOT] GameMain() returned unexpectedly -- exiting 0\n");
    return 0;
}

#ifndef __ANDROID__
int main(int argc, char **argv)
{
    /* Same arg contract as always: argv[1], when a positive number, is the
     * tick budget; anything else keeps the default (see mp6_headless_main). */
    return mp6_headless_main(argc > 1 ? atoi(argv[1]) : 0);
}
#endif /* !__ANDROID__ -- the Android .so has no main; see mp6_headless_main */

#else /* !MP6_HEADLESS_BUILD -- real Aurora window/GX build */

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h> /* #define main aurora_main -- see file header comment */

#include <string.h> /* memset, strcmp */

/* Pre-boot launcher settings menu. The implementation behind this
 * six-function seam is platform/gx/ui/launcher_core.cpp + the RmlUi
 * framework adapted from partyboard (docs/PARTYBOARD_PROVENANCE.md). The
 * mode decision (launcher vs automation) lives next to its skip-logic
 * truth table in the launcher TU; automation mode never reads the config
 * file at all, so every harness invocation boots identically to a build
 * with no launcher, on EITHER platform (see docs/TESTING.md, "automation
 * contract"): tick-budget argv / --input-script / MP6_AUTO_START_TICKS /
 * MP6_LAUNCHER=0 (Android's `-e straight_boot 1` intent extra maps onto
 * the same contract, Mp6Activity.java).
 *
 * The android windowed row compiles the launcher too -- touch drives the
 * same RmlUi menu, and a missing-content onboarding dialog replaces
 * booting straight into [DVD] degradation on a plain launch.
 *
 * mp6_launcher_content_ready: boot-time content re-validation --
 * launcher-mode boots that would SKIP the menu (launcher.skip) still open
 * it when no game content is present, because the alternative is booting
 * into a content-less game world; the menu opens directly on its "Select
 * Game" state. Automation mode never calls it. */
extern int mp6_launcher_decide_mode(int hasNumericArg, int hasInputScript, int *outShowMenu);
extern int mp6_launcher_cfg_backend(void);
extern int mp6_launcher_cfg_vsync(void);
extern int mp6_launcher_cfg_msaa(void); /* Anti-Aliasing (MSAA): aurora_initialize-time, restart-pending like backend/vsync */
extern int mp6_launcher_cfg_post_aa(void); /* Anti-Aliasing P2 (FXAA): AuroraPostAA value, applied LIVE (not restart-pending) */
extern float mp6_launcher_cfg_ssaa(void);  /* Anti-Aliasing P3 (SSAA): AuroraConfig.ssaa factor, aurora_initialize-time, restart-pending (desktop-only) */
extern void mp6_launcher_note_session_aa(int msaa, float ssaa, int postAa); /* Anti-Aliasing: what aurora ACTUALLY got this session (config + env levers resolved) */
extern int mp6_launcher_cfg_aspect_locked(void); /* A5: gameplay-time-only aspect policy */
extern int mp6_launcher_cfg_widescreen(void); /* gameplay-time-only dynamic-widescreen policy */
extern void mp6_launcher_apply_display_settings(void *sdlWindow);
extern int mp6_launcher_run_menu(void *sdlWindow);
extern void mp6_launcher_apply_game_settings(void);
extern int mp6_launcher_content_ready(void);

/* Captured out of Aurora's own startup
 * log line (see mp6_boot.h's declaration comment for the full mechanism --
 * this is what mp6_aurora_queried_max_texture_dimension_2d() below reads
 * back). 0 = not seen yet. */
static int g_mp6AuroraMaxTextureDimension2D = 0;

/* Scans one already-received log message for Aurora's own
 * "maxTextureDimension2D: <N>" substring (lib/webgpu/gpu.cpp's
 * Log.info("Using limits: ...") -- {fmt}-style formatting, so N is always
 * plain decimal digits, no separators) and stashes N if found. Safe to
 * call on every message (cheap strstr + bounded strtol); a message that
 * doesn't contain the substring is a no-op. Only accepts a positive,
 * sane-range value, so a truncated/garbled line can't poison the stashed
 * value with 0 or something absurd. */
static void mp6_aurora_scan_log_for_texture_limit(const char *message)
{
    static const char needle[] = "maxTextureDimension2D: ";
    const char *hit = strstr(message, needle);
    long parsed;
    char *end;
    if (hit == NULL) {
        return;
    }
    hit += sizeof(needle) - 1;
    parsed = strtol(hit, &end, 10);
    if (end != hit && parsed > 0 && parsed <= 1000000L) {
        g_mp6AuroraMaxTextureDimension2D = (int)parsed;
    }
}

int mp6_aurora_queried_max_texture_dimension_2d(void)
{
    return g_mp6AuroraMaxTextureDimension2D;
}

static void mp6_aurora_log_callback(AuroraLogLevel level, const char *module, const char *message, unsigned int len)
{
    const char *levelStr;
    (void)len;
    switch (level) {
    case LOG_DEBUG:   levelStr = "DEBUG";   break;
    case LOG_INFO:    levelStr = "INFO";    break;
    case LOG_WARNING: levelStr = "WARNING"; break;
    case LOG_ERROR:   levelStr = "ERROR";   break;
    case LOG_FATAL:   levelStr = "FATAL";   break;
    default:          levelStr = "?";       break;
    }
    printf("[AURORA %s: %s] %s\n", levelStr, module, message);
    fflush(stdout);
    mp6_aurora_scan_log_for_texture_limit(message);
    if (level == LOG_FATAL) {
        fflush(stdout);
        abort();
    }
}

int main(int argc, char **argv) /* expands to aurora_main via aurora/main.h */
{
    setvbuf(stdout, NULL, _IOLBF, 4096);
    mp6_host_crash_install();
    mp6_assert_image_low(); /* whole-image-below-4GB invariant */

    /* A plain double-click (no CLI arg) runs unlimited: without this it
     * would inherit the shared 60-tick (~1s) default and immediately
     * exit, which looks like the process "auto closed" to anyone just
     * trying to play. A real numeric arg (bounded/CI runs, e.g. the
     * screenshot harness) still wins -- set the unlimited flag first,
     * then let the arg scan below override mp6_max_ticks.
     * mp6_tick_advance() (shims_manual.c) checks the flag before
     * comparing against mp6_max_ticks at all. The --headless build's own
     * main() (above) never sets this, so its no-arg default (60 ticks)
     * is unchanged. */
    if (argc <= 1) {
        mp6_ticks_unlimited = 1;
    }
    /* Scan all args so --input-script "<spec>" (deterministic PAD-button
     * scripting for tests, see platform/gx/aurora_bridge.c's input-script
     * section) can coexist with the numeric tick-budget arg in either
     * order. Note argc>1 is true the instant ANY arg (including
     * --input-script itself) is given, so the `if(argc<=1)` unlimited
     * default above never fires for scripted runs -- --input-script
     * therefore also implies unlimited ticks (run until the window closes)
     * unless an explicit numeric arg is ALSO given, which still wins. A
     * scripted run cut off at the 60-tick default would end before its
     * first `press` step could land, making every step silently inert. */
    int launcherMode; /* see mp6_launcher_decide_mode's truth table */
    int showMenu = 0;
    {
        int i;
        int hasInputScript = 0;
        int hasNumericArg = 0;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--input-script") == 0 && i + 1 < argc) {
                mp6_input_script_init(argv[i + 1]);
                hasInputScript = 1;
                i++; /* consume the spec argument too */
            } else {
                int n = atoi(argv[i]);
                if (n > 0) { mp6_max_ticks = n; hasNumericArg = 1; }
            }
        }
        if (hasInputScript && !hasNumericArg) {
            mp6_ticks_unlimited = 1;
        }
        /* One decide_mode for every platform -- android plain launches
         * are interactive by definition; the automation levers (numeric
         * args / --input-script / MP6_AUTO_START_TICKS / MP6_LAUNCHER=0,
         * all deliverable through the "args"/straight_boot intent extras)
         * keep automation boots launcher-free exactly like Windows. */
        launcherMode = mp6_launcher_decide_mode(hasNumericArg, hasInputScript, &showMenu);
    }

    if (mp6_ticks_unlimited) {
        printf("[BOOT] mp6native starting (Aurora GX/VI/PAD backend) -- no tick budget, runs until the window is closed\n");
    } else {
        printf("[BOOT] mp6native starting (Aurora GX/VI/PAD backend) -- tick budget %d\n", mp6_max_ticks);
    }
    fflush(stdout);

    /* AuroraConfig zero-initialized first: mem1Size/mem2Size default to 0
     * (disabled) this way -- our OWN game arena (platform/os/arena.c, a
     * fixed low-4GB VirtualAlloc region) already serves every allocation
     * the game makes, so Aurora's optional built-in MEM1/ARAM emulation
     * would just be dead weight (or worse, a second, confusing address
     * range) if enabled here. */
    AuroraConfig config;
    memset(&config, 0, sizeof(config));
    /* Test-isolation protocol: the title includes this checkout's own
     * workspace directory name so a title-based window lookup from
     * automated test tooling can never accidentally grab a DIFFERENT
     * mp6-native checkout's window if more than one is running at once. */
    config.appName = "mp6native [mp6-native]";
    config.desiredBackend = BACKEND_AUTO;
    config.vsync = true;
    /* Widescreen test lever: MP6_WINDOW_SIZE=WxH
     * sets the INITIAL window size directly (AuroraConfig.windowWidth/
     * windowHeight -- aurora lib/window.cpp create_window(), consumed only
     * as the OS window's initial size). Exists so a scripted verification
     * run (tools/vtest.sh + --input-script, which never touches
     * mp6_config.json) can reliably land at an exact 4:3/16:9/21:9 shape
     * to screenshot, the same automation-compatible-test-lever shape as
     * MP6_AUTO_START_TICKS/MP6_FREE_ASPECT/MP6_WIDESCREEN. Unset (the
     * default): zero effect -- Aurora's own create_window() default
     * (1280x960) applies exactly as before this lever existed. */
    {
        const char *winSize = getenv("MP6_WINDOW_SIZE");
        if (winSize != NULL && winSize[0] != '\0') {
            unsigned w = 0, h = 0;
            if (sscanf(winSize, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
                config.windowWidth = w;
                config.windowHeight = h;
                printf("[BOOT] MP6_WINDOW_SIZE=%ux%u -- initial window size overridden\n", w, h);
            } else {
                printf("[BOOT] MP6_WINDOW_SIZE=\"%s\" -- malformed (expected WxH), ignoring\n", winSize);
            }
        }
    }
    /* backend/vsync are aurora_initialize-time parameters, so they are
     * the launcher config's two "take effect next launch" settings. In
     * automation mode both accessors return exactly the defaults above
     * (the config file was never read), so this assignment is inert there. */
    if (launcherMode) {
        config.desiredBackend = (AuroraBackend)mp6_launcher_cfg_backend();
        config.vsync = mp6_launcher_cfg_vsync() ? true : false;
    } else {
        /* Automation/straight-boot: default vsync OFF. A drive that only needs
         * to REACH a screen shouldn't be paced down to the display refresh
         * (~60Hz -> ~150s to mode-select). With vsync off AND the tick throttle
         * off (its automation default, see mp6_tick_throttle_init), a scripted
         * drive runs as fast as the machine allows (~30s). Real-time gates
         * (leakgate) set MP6_TICK_HZ=60, which paces ticks via the throttle
         * regardless of vsync, so their per-minute measurement is unaffected.
         * MP6_VSYNC=1 forces vsync back on for the rare automation run wanting
         * it (e.g. tearing-free capture of a moving scene). */
        const char *vs = getenv("MP6_VSYNC");
        config.vsync = (vs != NULL && vs[0] == '1') ? true : false;
    }
    /* Anti-Aliasing (MSAA 4x / SSAA 1.5x-2x): aurora_initialize-time parameters
     * like backend/vsync above -- aurora builds the multisampled targets/resolve/
     * per-pipeline sampleCount and the scaled content framebuffer off these two
     * values at that call, which is why the UI calls them restart-pending.
     *
     * ENV WINS, IN BOTH MODES. MP6_MSAA / MP6_SSAA are checked FIRST and beat the
     * saved config, exactly like MP6_SHADOW_QUALITY, MP6_UNLOCKED_FPS and
     * MP6_WIDESCREEN document themselves ("env lever set: it wins outright").
     * They used to be read only OUTSIDE launcher mode, which made the launcher's
     * Anti-Aliasing row grey itself out (settings.cpp isDisabled) for levers that
     * were then ignored -- the control was locked for no reason. Tolerant parsing
     * either way: only "4" turns MSAA on, only "1.5"/"2" set an SSAA factor;
     * anything else (unset included) is off, matching the config parser's own
     * tolerant clamp, so an unset lever leaves the byte-identical native path. */
    {
        const char *ms = getenv("MP6_MSAA");
        if (ms != NULL) {
            config.msaa = (strcmp(ms, "4") == 0) ? 4 : 1;
        } else {
            config.msaa = launcherMode ? (uint32_t)mp6_launcher_cfg_msaa() : 1;
        }
    }
#ifndef __ANDROID__
    /* SSAA is DESKTOP-ONLY: aurora ignores the field on __ANDROID__ too, but the
     * port never even sends it there. */
    {
        const char *ss = getenv("MP6_SSAA");
        if (ss != NULL) {
            config.ssaa = (strcmp(ss, "2") == 0) ? 2.0f : (strcmp(ss, "1.5") == 0) ? 1.5f : 1.0f;
        } else {
            config.ssaa = launcherMode ? mp6_launcher_cfg_ssaa() : 1.0f;
        }
    }
#endif
#ifdef __ANDROID__
    /* A4: launcher resources ("res/rml/...", "res/fonts/...") ship as APK
     * assets and load through aurora's SDL_IOFromFile-backed RmlUi file
     * interface, which opens RELATIVE paths from the asset system. An
     * empty resourcesPath keeps aurora from prefixing SDL_GetBasePath()'s
     * android value ("./") onto them -- AAssetManager does no "./"
     * normalization, so a prefixed path would miss the asset table
     * (launcher_core.cpp mp6_resource_base has the full story). */
    config.resourcesPath = "";
#endif
    config.logCallback = &mp6_aurora_log_callback;
    config.logLevel = LOG_INFO;
    /* windowPosX/windowPosY must be negative: Aurora's create_window
     * (lib/window.cpp) treats non-negative values as an EXACT screen
     * position, not "unset" -- only negative maps to
     * SDL_WINDOWPOS_UNDEFINED (OS-default placement). SDL window positions
     * are CLIENT-AREA coordinates on Windows, so an exact (0,0) pins the
     * drawable area to the screen's top-left corner and pushes the title
     * bar + left border entirely off-screen -- the window looks
     * borderless while the frame is actually parked above the desktop. */
    config.windowPosX = -1;
    config.windowPosY = -1;

    printf("[BOOT] calling aurora_initialize() before GameMain() reaches the game's GXInit\n");
    fflush(stdout);
    AuroraInfo info = aurora_initialize(argc, argv, &config);
    /* Anti-Aliasing (FXAA): a live post-process, applied here right after aurora
     * is up rather than through AuroraConfig (unlike msaa) -- so it is NOT
     * restart-pending. MP6_FXAA is checked FIRST and wins in BOTH modes, same
     * "env wins outright" contract as MP6_MSAA/MP6_SSAA above; tolerant, only
     * "1" turns it on. Off is a no-op -- the shader's post_aa_mode stays 0, the
     * present path byte-identical.
     *
     * The resolved trio is then reported to the launcher: it is the only place
     * that knows what aurora ACTUALLY got (config after env override), and the
     * settings row needs it to keep the modes mutually exclusive at runtime --
     * see mp6_launcher_aa_apply_live() (launcher_core.cpp). */
    {
        const char *fx = getenv("MP6_FXAA");
        int fxaaOn;
        if (fx != NULL) {
            fxaaOn = (fx[0] == '1') ? 1 : 0;
        } else {
            fxaaOn = launcherMode ? mp6_launcher_cfg_post_aa() : 0;
        }
        /* Mutually exclusive at boot too: an init-time mechanism (MSAA/SSAA)
         * already owns this session's image, so FXAA on top would double-AA. */
        if (fxaaOn && (config.msaa > 1
#ifndef __ANDROID__
                       || config.ssaa > 1.0f
#endif
                       )) {
            printf("[BOOT] AA: FXAA suppressed -- this session initialized with "
                   "MSAA %ux / SSAA %.2fx (one AA mechanism at a time)\n",
                   (unsigned)config.msaa,
#ifndef __ANDROID__
                   (double)config.ssaa
#else
                   1.0
#endif
            );
            fflush(stdout);
            fxaaOn = 0;
        }
        aurora_set_post_aa(fxaaOn ? AURORA_POST_AA_FXAA : AURORA_POST_AA_NONE);
        mp6_launcher_note_session_aa((int)config.msaa,
#ifndef __ANDROID__
                                     config.ssaa,
#else
                                     1.0f,
#endif
                                     fxaaOn);
    }
    /* Normal-window placement + the WINDOW half of the fixed-aspect policy
     * (bordered windowed window, interactive resizes constrained to 4:3).
     * Must run before the first game frame; see platform/gx/aurora_bridge.c's
     * window-policy section. The CONTENT half (letterboxed present) is
     * applied separately, later -- see mp6_bridge_apply_content_aspect_policy()
     * below. */
    mp6_bridge_window_policy_init((void *)info.window);
    /* Move a self-owned debug console clear of the game window now that
     * it exists -- see mp6_boot.h and platform/gx/aurora_bridge.c. */
    mp6_bridge_post_window_init((void *)info.window);

    /* The launcher menu, interactive launches only. Runs the exact
     * aurora frame cycle the game itself uses, BEFORE any game state
     * exists; Play falls through to the unchanged boot below, Quit exits
     * cleanly. In automation mode (every harness invocation) launcherMode
     * is false and this whole block is two untaken branches. */
    if (launcherMode) {
        /* Window mode/size from config -- applied before the menu (so it
         * opens in the configured mode) and equally on `launcher.skip`
         * boots where the menu never shows. A5: the CONTENT aspect-fit
         * policy is deliberately NOT applied here anymore (see the
         * mp6_bridge_apply_content_aspect_policy() call below, right
         * before GameMain()) -- doing it here used to letterbox the
         * RmlUi launcher itself on any non-4:3 display. */
        mp6_launcher_apply_display_settings((void *)info.window);
        /* A4: boot-time content re-validation. A launcher.skip boot with
         * no game content present opens the menu anyway (on its "Select
         * Game" onboarding state) -- the pre-A4 alternative was booting
         * into the [DVD] missing-FST degradation, which on a phone
         * presented as a black-screen crash loop. Content present ->
         * exactly the pre-A4 flow. */
        if (!showMenu && !mp6_launcher_content_ready()) {
            printf("[LAUNCHER] no game content found -- opening the menu (launcher.skip overridden "
                   "for onboarding)\n");
            fflush(stdout);
            showMenu = 1;
        }
        if (showMenu && mp6_launcher_run_menu((void *)info.window)) {
            printf("[BOOT] launcher menu: quit before game boot -- shutting down Aurora, exiting 0\n");
            fflush(stdout);
            { /* UI documents down BEFORE the RmlUi context dies -- else the
               * CRT destroys them post-shutdown, an observed teardown UAF
               * (mp6_launcher_ui_teardown's comment, launcher_core.cpp). */
                extern void mp6_launcher_ui_teardown(void);
                mp6_launcher_ui_teardown();
            }
            aurora_shutdown();
            return 0;
        }
        /* Menu-or-not, apply the boot-time settings (tick rate, content
         * root, volume) so `launcher.skip` boots honor the config too. */
        mp6_launcher_apply_game_settings();
    }

    /* Decided once, right before
     * GameMain(), on every boot path (automation, launcher.skip, and
     * interactive Play alike) -- mp6_launcher_cfg_widescreen() returns
     * g_cfg.widescreen in launcher mode or a fixed 0 (matching every
     * existing automated gate's assumption, and this setting's own
     * default) otherwise. MUST run BEFORE mp6_bridge_apply_content_aspect_
     * policy() right below: that call reads mp6_widescreen_enabled() to
     * decide FIT-at-native-4:3 vs FIT-at-the-live-wide-aspect, so the
     * enabled/disabled state must already be set by the time it runs.
     * Everything else downstream (render width, camera aspect, 2D shim)
     * also reads mp6_widescreen_enabled() fresh on every call -- this is
     * the only place the ON/OFF decision itself is made. */
    mp6_widescreen_set_enabled(mp6_launcher_cfg_widescreen());

    /* A5: the CONTENT half of the fixed-aspect policy, engaged exactly
     * here -- right before GameMain(), on every boot path (automation,
     * launcher.skip, and interactive Play alike). Must still run BEFORE
     * the first game frame (see mp6_bridge_window_policy_init's own
     * comment); moved out of the block above so the RmlUi launcher menu
     * (and any skip-mode gap before this point) always renders against
     * the real window/display surface instead of a phantom letterboxed
     * one. mp6_launcher_cfg_aspect_locked() returns g_cfg.aspectLocked in
     * launcher mode or a fixed 1 (today's automation default) otherwise --
     * identical final gameplay-time state to before. Widescreen extends this same
     * call (mp6_bridge_apply_content_aspect_policy(), aurora_bridge.c) to
     * also consult mp6_widescreen_enabled() (set immediately above) and
     * stay on FIT -- never STRETCH -- at the live wide aspect when active. */
    mp6_bridge_apply_content_aspect_policy(mp6_launcher_cfg_aspect_locked());

    mp6_os_globals_init();
    mp6_arena_init();

    printf("[BOOT] calling GameMain() (game/main.c's main(), renamed)\n");
    fflush(stdout);

    GameMain();

    /* Unreachable in practice (GameMain() is an infinite loop, same as real
     * hardware) -- platform/gx/aurora_bridge.c's VIWaitForRetrace is what
     * actually exits the process, either via mp6_tick_and_maybe_exit()
     * (tick budget reached) or on an AURORA_EXIT event (window closed).
     * Fall through defensively rather than relying on GameMain() ever
     * returning. */
    printf("[BOOT] GameMain() returned unexpectedly -- shutting down Aurora, exiting 0\n");
    fflush(stdout);
    aurora_shutdown();
    return 0;
}

#ifdef __ANDROID__
/* The in-.so android windowed entry.
 *
 * Call chain on device: Mp6Activity (getMainSharedObject()=libmp6game.so,
 * getMainFunction()="mp6_android_main") -> SDL3's nativeRunMain dlopens the
 * game image AGAIN (bionic dedupes to the already-low-loaded module the
 * mp6shell bootstrap placed -- refcount bump, no second mapping), dlsyms
 * THIS function and runs it on the dedicated SDL thread via SDL_RunApp.
 *
 * Boot policy lives here rather than in the shell so it stays next to the
 * aurora main it feeds (and can call the statically-linked SDL storage
 * getters, which the shell -- deliberately SDL-free -- cannot):
 *   1. app arguments of the form MP6_NAME=VALUE become environment
 *      variables (the android equivalent of prefixing env vars onto the
 *      Windows exe command line: `adb shell am start ... --es args
 *      "MP6_AUTO_START_TICKS=200,320"`); everything else (tick budgets,
 *      --input-script) forwards to aurora_main's existing argv scanner.
 *   2. the base dir for host_android.c's MP6_HOST_BASE: the app's EXTERNAL
 *      files dir (/sdcard/Android/data/<pkg>/files/mp6 -- user-reachable
 *      for the disc push) preferred, internal files dir as fallback,
 *      whichever actually holds GP6E01/sys/fst.bin; with neither populated
 *      external is still published so dvd_files.c's honest "[DVD] couldn't
 *      open FST" degradation names the directory the user should fill.
 * The SDL storage getters are declared by hand (SDL3's SDL_system.h shape)
 * to keep this file's include set unchanged. */
#include <sys/stat.h> /* mkdir */

extern const char *SDL_GetAndroidExternalStoragePath(void);
extern const char *SDL_GetAndroidInternalStoragePath(void);

static int mp6_android_has_fst(const char *base)
{
    char probe[1024];
    FILE *f;
    snprintf(probe, sizeof(probe), "%s/GP6E01/sys/fst.bin", base);
    f = fopen(probe, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int mp6_android_main(int argc, char **argv)
{
    static char *fwdArgv[33];
    static char baseDir[1024];
    int fwdArgc = 0;
    int i;

    /* Back-button policy. SDL3 reads
     * hints through the environment (SDL_GetHint falls back to getenv, and
     * the Java side's nativeGetHintBoolean goes through the same store), so
     * a plain setenv -- before aurora_main ever initializes SDL -- arms
     * SDLActivity.onBackPressed()'s trap branch: BACK can never
     * finish()/kill the activity (no instant death mid-save-write); it
     * reaches the game as a normal SDL_SCANCODE_AC_BACK key event instead,
     * which aurora_bridge.c's Android keybind row maps onto PAD B (menu
     * "cancel/back"). Exit stays HOME/recents, like any fullscreen game.
     * overwrite=0: an explicit MP6-arg/env override still wins. */
    setenv("SDL_ANDROID_TRAP_BACK_BUTTON", "1", 0);

    /* (1) MP6_*=... app arguments -> env; the rest forwards. */
    fwdArgv[fwdArgc++] = (argc > 0 && argv && argv[0]) ? argv[0] : (char *)"mp6native";
    for (i = 1; i < argc && argv && fwdArgc < 32; i++) {
        char *eq = argv[i] ? strchr(argv[i], '=') : NULL;
        if (argv[i] && strncmp(argv[i], "MP6_", 4) == 0 && eq) {
            *eq = 0;
            setenv(argv[i], eq + 1, 1);
            printf("[BOOT] android arg -> env %s=%s\n", argv[i], eq + 1);
            *eq = '=';
        } else if (argv[i]) {
            fwdArgv[fwdArgc++] = argv[i];
        }
    }
    fwdArgv[fwdArgc] = NULL;

    /* (2) base dir -> MP6_HOST_BASE (host_android.c resolves disc/save/pref
     * under it; a caller-exported value still wins, same as the launcher). */
    {
        const char *ext = SDL_GetAndroidExternalStoragePath();
        const char *inl = SDL_GetAndroidInternalStoragePath();
        char extBase[512] = {0};
        char inlBase[512] = {0};
        if (ext && ext[0]) snprintf(extBase, sizeof(extBase), "%s/mp6", ext);
        if (inl && inl[0]) snprintf(inlBase, sizeof(inlBase), "%s/mp6", inl);
        if (extBase[0] && mp6_android_has_fst(extBase)) {
            snprintf(baseDir, sizeof(baseDir), "%s", extBase);
            printf("[BOOT] android base dir: %s (external, fst.bin present)\n", baseDir);
        } else if (inlBase[0] && mp6_android_has_fst(inlBase)) {
            snprintf(baseDir, sizeof(baseDir), "%s", inlBase);
            printf("[BOOT] android base dir: %s (internal, fst.bin present)\n", baseDir);
        } else {
            snprintf(baseDir, sizeof(baseDir), "%s",
                     extBase[0] ? extBase : (inlBase[0] ? inlBase : "/sdcard/mp6"));
            printf("[BOOT] android base dir: %s (fst.bin NOT found -- push the disc tree "
                   "here; expect the [DVD] missing-FST degradation)\n", baseDir);
        }
        fflush(stdout);
        mkdir(baseDir, 0755); /* EEXIST fine -- pref/save writers expect it */
        setenv("MP6_HOST_BASE", baseDir, 0);
    }

    return aurora_main(fwdArgc, fwdArgv); /* the same windowed boot every platform runs */
}
#endif /* __ANDROID__ */

#endif /* MP6_HEADLESS_BUILD */
