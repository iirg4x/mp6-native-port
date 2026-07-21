/* MP6 native port -- shared boot/tick-budget state between
 * platform/main_native.c (owns it, parses the CLI arg) and
 * platform/null/shims_manual.c (VIWaitForRetrace counts against it). */
#ifndef MP6_BOOT_H
#define MP6_BOOT_H

#include <stddef.h> /* size_t, for mp6_symbolize_addr below */
#include <stdint.h> /* uint32_t, for mp6_heap_block_data_size below */

#ifdef __cplusplus
extern "C" {
#endif

extern int mp6_max_ticks;   /* default 60; CLI arg 1 overrides */
extern long mp6_tick_count; /* incremented once per VIWaitForRetrace call */

/* A plain double-click launches with NO CLI arg, and a fixed 60-tick
 * (~1s) budget would make that look like the process "auto closed" --
 * confusing for anything but a bounded CI/verification run.
 * platform/main_native.c's Aurora (non-headless) main() sets this when
 * argc<=1 (no explicit tick-budget arg given), and mp6_tick_advance()
 * below checks it FIRST: nonzero means "never trigger the tick-budget
 * exit," i.e. run until the window is closed instead (VIWaitForRetrace's
 * own AURORA_EXIT handling already does a clean aurora_shutdown()+exit(0)
 * for that). Passing a numeric CLI arg still sets mp6_max_ticks and takes
 * priority (main_native.c sets this flag BEFORE checking argv, so a real
 * arg simply never gets a chance to set it) -- bounded/CI runs are
 * unaffected. The --headless build's own main() never touches this flag
 * at all, so its no-arg default (60 ticks) is unchanged. */
extern int mp6_ticks_unlimited;

/* Call from any shim that represents "one vblank" (VIWaitForRetrace).
 * Exits the process cleanly (code 0) once mp6_tick_count reaches
 * mp6_max_ticks -- "tick N frames then exit". --headless only
 * (platform/null/shims_manual.c's own VIWaitForRetrace) -- see
 * mp6_tick_advance() below for why the aurora build needs a different
 * shape of the same budget check. */
void mp6_tick_and_maybe_exit(void);

/* Increments mp6_tick_count and returns nonzero the ONE time it reaches
 * mp6_max_ticks (never again after -- matches mp6_tick_and_maybe_exit's
 * "exit exactly once" contract), WITHOUT itself exiting.
 * platform/gx/aurora_bridge.c's VIWaitForRetrace calls this instead of
 * mp6_tick_and_maybe_exit(): calling exit(0) directly while Aurora's GPU
 * device/frame is still live (mid-flight, no aurora_shutdown() ever
 * called) makes Aurora's own C++ static-destructor teardown (which still
 * runs during a plain exit()) treat the device loss as FATAL rather than
 * the clean WARNING a real aurora_shutdown()-then-exit sequence produces
 * -- and this port's own aurora log callback (platform/main_native.c)
 * calls abort() on a FATAL message, turning a clean tick-budget exit into
 * a crash. The aurora build's VIWaitForRetrace calls aurora_shutdown()
 * itself, in the right order, before exiting -- see that file. */
int mp6_tick_advance(void);

/* Seeds __OSBusClock/__OSCoreClock with real GameCube constants; call
 * once, early, before anything computes OSTicksToMilliseconds. */
void mp6_os_globals_init(void);

/* The crash handler installer is the host seam's mp6_host_crash_install,
 * declared in platform/host/host.h -- call-once-early contract. */

/* Test tooling: parses a deterministic input script
 * ("wait:180;press:start;wait:60;press:a") and arms it for
 * platform/gx/aurora_bridge.c's own per-tick PAD injection -- see that
 * file's input-script section for the full design (removes
 * window-focus/SendKeys timing races from automated testing entirely).
 * Defined in platform/gx/aurora_bridge.c (Aurora/non-headless build only,
 * matching that file's own scope); called from platform/main_native.c's
 * CLI parsing when --input-script is given. A plain no-op declaration
 * would be pointless for --headless (no window/focus concern exists
 * there at all), so this is intentionally not provided for that build. */
void mp6_input_script_init(const char *spec);

/* The exe has no explicit /SUBSYSTEM flag, so launching it with no parent
 * console (the ordinary "just run the .exe" path) makes Windows allocate
 * a brand-new, independently-placed console window for our stdout/
 * OSReport traffic -- both it and the Aurora game window default to "let
 * Windows decide" placement and can visibly overlap. This repositions the
 * console (if this process owns one exclusively -- see
 * platform/gx/aurora_bridge.c's GetConsoleProcessList guard that protects
 * an inherited interactive shell from ever being moved) to a screen rect
 * that provably does not intersect the game window's rect. Call once,
 * right after aurora_initialize() returns (the window must already
 * exist). sdlWindow is really an SDL_Window* -- typed void* here so this
 * shared header never needs to know about SDL types. Defined in
 * platform/gx/aurora_bridge.c (Aurora/non-headless build only -- headless
 * has no window or console-overlap concern at all). */
void mp6_bridge_post_window_init(void *sdlWindow);

/* Normal-window placement + the WINDOW half of the fixed-aspect policy:
 *   (a) the game window can look borderless at launch (the frame/caption
 *       exists but sits off-screen -- see platform/main_native.c's
 *       windowPosX/windowPosY comment for the root cause) -- rescued here
 *       (defensive; OS-default placement already avoids it);
 *   (b) constrain interactive resizes to 4:3 (SDL_SetWindowAspectRatio) so
 *       a freely-dragged window doesn't waste most of its area on the
 *       content-fit letterbox bars (the CONTENT half of this policy,
 *       mp6_bridge_apply_content_aspect_policy() below).
 * Call once, right after aurora_initialize() returns, BEFORE the first
 * game frame. sdlWindow is really an SDL_Window* -- typed void* here so
 * this shared header never needs to know about SDL types. Defined in
 * platform/gx/aurora_bridge.c (Aurora/non-headless build only -- headless
 * has no window at all). MP6_FREE_ASPECT=1 skips the resize constraint
 * (debug escape hatch back to free-stretch behavior; the bordered-window
 * placement fix is unconditional).
 *
 * A5: this used to ALSO flip Aurora's content framebuffer to
 * AURORA_VIEWPORT_FIT (the (b) that distorts the GAME's scene if left
 * free-stretch) right here, i.e. before the RmlUi launcher menu ever
 * ran. That call does far more than fit the 640x480 GX viewport: it
 * retargets window::get_window_size()'s fb_width/fb_height (aurora
 * lib/window.cpp), which aurora::rmlui's own presentation-dimension calc
 * (lib/rmlui.cpp) consumes too -- so the ENTIRE RmlUi launcher (menu,
 * wordmark, watermark, disc-info, version-info) was being composed for a
 * phantom letterboxed 4:3 sub-rectangle instead of the real window/
 * display surface. Invisible on the desktop default 1024x768 window
 * (already 4:3, so the fit is a no-op) but severe on any non-4:3 physical
 * surface (confirmed on a Galaxy S22+, ~21.5:9 landscape --
 * docs/A5_LAUNCHER_ASPECT.md). See mp6_bridge_apply_content_aspect_policy()
 * below for where that half of the policy moved. */
void mp6_bridge_window_policy_init(void *sdlWindow);

/* A5: the CONTENT half of the fixed-aspect policy -- AuroraSetViewportPolicy
 * itself, deferred from "before the launcher" to "right before GameMain()"
 * so the RmlUi launcher (menu + persistent overlay) always composes
 * against the real window/display surface, on every aspect. aspectLockedCfg
 * is the resolved user preference: g_cfg.aspectLocked in launcher mode
 * (mp6_launcher_cfg_aspect_locked(), launcher_core.cpp, mirroring the
 * existing mp6_launcher_cfg_backend()/_vsync() pattern) or a fixed 1
 * (today's automation-mode default) otherwise. MP6_FREE_ASPECT keeps
 * absolute priority in both directions, same as before. Call once, right
 * before GameMain(), on EVERY boot path (automation, launcher.skip, and
 * interactive Play alike) -- identical final gameplay-time state to
 * pre-A5, just applied later; zero frames render between the old call
 * site and this one on any path (the launcher menu loop and any
 * skip-mode gap both render zero frames of their own before GameMain()
 * starts). Defined in platform/gx/aurora_bridge.c (Aurora/non-headless
 * build only). */
void mp6_bridge_apply_content_aspect_policy(int aspectLockedCfg);

/* The "everything but minigames" contract. dll/m6*.rel (plus a handful of
 * extra numeric IDs found on the real disc outside that contiguous range
 * -- m433/m535/m541/m559/m562/m571/m580 -- see platform/os/dll_bridge.c's
 * own comment) have no recovered C source; this port cannot run their
 * real gameplay logic at all. Rather than let game/objdll.c's omDLLLink()
 * either silently no-op or jump into whatever raw, un-relocated bytes
 * happen to be at the file's own prolog offset, dll_bridge.c recognizes
 * this whole filename family as another synthetic DLL category (matching
 * the statically-linked overlays' existing precedent) and binds a STUB
 * prolog/epilog: log a clear banner, flip this flag, return success --
 * exactly like a real minigame's prolog would, just with no gameplay
 * behind it. Checked once per frame by platform/gx/aurora_bridge.c's
 * VIWaitForRetrace (Aurora build) to force a black clear regardless of
 * whatever else the game logic still tries to draw underneath -- a
 * graceful, correctly-labeled placeholder instead of a crash or garbage
 * frame the moment a board ever tries to load a real minigame. */
extern volatile int mp6_dll_stub_black_screen_active;

/* Test-only forced-load hook: if the MP6_TEST_LOAD_DLL env var is set (to
 * a synthetic REL filename, e.g. "m601Dll" or "m601Dll.rel"), synthesizes
 * one full DVDOpen -> DVDReadAsyncPrio -> OSLink -> prolog() round trip
 * for that exact name via dll_bridge.c's own real entry points -- proving
 * the stub path end to end for an ARBITRARY m6xx-shaped name without
 * needing real (not-yet-decompiled) board code to ever request one for
 * real. No-op if the env var isn't set. Called once, lazily, from
 * mp6_tick_advance() (platform/null/shims_manual.c) on the first tick --
 * late enough that the heap/arena (HuMemDirectMalloc) this whole dance
 * allocates through is already up in both build modes. */
void mp6_dll_bridge_selftest_check_env(void);

/* Resolves a code address to "func+0xNN (file:line)" (or the best partial
 * match dbghelp can give) using the SAME dbghelp recipe
 * mp6_host_crash_install's own exception filter already uses
 * (SYMOPT_LOAD_ANYTHING + a real file handle passed to SymLoadModuleEx --
 * what makes in-process symbolization actually work). Deliberately a
 * SEPARATE lazy init from the crash filter's own inline sequence (not a
 * shared refactor of it) -- that filter is a tested, last-resort path and
 * is left completely untouched; this is an additive capability for
 * resolving addresses in a normal, still-running process (e.g. the
 * draw-call bisect harness's "which call site is this" logging).
 * Idempotent and safe to call many times: the real
 * SymInitialize/SymLoadModuleEx cost is paid at most once per process
 * (guarded internally), no matter which caller reaches it first or how
 * many times any caller asks. Always writes a NUL-terminated string into
 * outBuf (falls back to "<no symbol...>"-shaped text rather than failing
 * silently); outBufSz including the NUL. Defined per host backend
 * (platform/host/host_win32.c dbghelp; host_android.c dladdr). */
void mp6_symbolize_addr(void *addr, char *outBuf, size_t outBufSz);

/* platform/os/malloc_direct.c's all-heap allocation census -- see that
 * file's own header comment above HuMemDirectMalloc for the full design.
 * Called once per tick from mp6_tick_advance() (platform/null/
 * shims_manual.c, the one hook both VIWaitForRetrace implementations
 * already call every tick -- same precedent as the RSS watchdog right
 * next to it). A complete no-op unless MP6_ALLOC_CENSUS_START_TICK is
 * set. */
void mp6_alloc_census_tick_check(void);

/* Bytes readable from `ptr` through the end of its own HuMemDirectMalloc
 * block, or 0 when `ptr` is not verifiably a live block base (wrong shadow
 * magic, outside every heap, NULL). Lets a consumer handed a bare buffer
 * pointer with no length (the HSF loader's file buffers, allocated by
 * game/data.c) bound its reads to the allocation instead of trusting
 * in-file offsets. Conservative: may under-report by up to 8 bytes, never
 * over-reports. Defined in platform/os/malloc_direct.c next to the
 * shadow-header convention it reads. uint32_t (not the decomp's u32) so
 * this header stays includable from TUs without dolphin/types.h. */
uint32_t mp6_heap_block_data_size(const void *ptr);

/* WS6 (docs/WS6_OVERLAY_CAMERAS.md): the real GPU's maxTextureDimension2D,
 * as actually negotiated for THIS run -- captured by main_native.c's
 * mp6_aurora_log_callback, which scans Aurora's own startup "Using
 * limits:\n  maxTextureDimension2D: N" INFO log line (lib/webgpu/gpu.cpp)
 * for that exact substring and parses N. This is a live runtime query in
 * spirit without touching or rebuilding the vendored Aurora library: Aurora
 * already logs the value it queried from the real adapter/device, and the
 * port already registers a log callback that sees every line -- this just
 * reads what was already there. Returns 0 if the line hasn't been seen yet
 * (never called before aurora_initialize() returns, in practice) or this
 * build has no Aurora log stream at all (--headless, where
 * mp6_widescreen_render_width() is a fixed-640 stub that never consults
 * this at all) -- callers must treat 0 as "unknown, use a safe fallback",
 * never as a real zero-sized limit. */
int mp6_aurora_queried_max_texture_dimension_2d(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_BOOT_H */
