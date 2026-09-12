/* MP6 native port -- the host OUTPUT the window is actually on, and the
 * present sync that paces it.
 *
 * WHY THIS SEAM EXISTS. The launcher's FPS badge counts presents, and with
 * vsync on the present cadence is capped by the refresh rate of whichever
 * output the window happens to sit on -- not by the tick rate, not by the
 * engine's throughput. A bare "70 FPS" on a badge is therefore unreadable:
 * it names a measurement without naming the thing that produced it. This
 * module supplies the two missing facts (the current output's exact refresh
 * rate and the live present mode) so the badge and `stat fps` can say
 * "75 / 75 Hz FIFO" and be self-explanatory.
 *
 * It also owns the three pieces of live host display state nothing else can:
 *
 *   - the launch OUTPUT choice (video.display / video.window_x / _y), resolved
 *     before aurora_initialize because aurora shows the window inside it;
 *   - the SDL display id the window was last seen on, so a drag across
 *     outputs can force a surface reconfigure exactly once per crossing;
 *   - the live vsync flag, because aurora_enable_vsync() writes only
 *     wgpu's presentMode and keeps no queryable state of its own
 *     (external_refs/repos/aurora/lib/webgpu/gpu.cpp:1331).
 *
 * DECLARATIONS ONLY, NO SDL AND NO AURORA TYPES. This header is included by
 * src/gx/console/console_stats.c, which links in BOTH the windowed and
 * the --headless build (tools/build.py's PLATFORM_SOURCES_COMMON), while the
 * implementation lives in src/gx/aurora_bridge.c, which is
 * PLATFORM_AURORA_ONLY. A single SDL or aurora type in this file would break
 * the headless compile; every function here is instead referenced only from
 * code that the headless build never compiles. The one SDL-typed entry point
 * (the event observer) is deliberately absent and declared locally at its two
 * call sites, the same way aurora_bridge.c already declares
 * mp6_launcher_forward_sdl_event.
 *
 * The implementation's statics live in aurora_bridge.c because that TU is
 * already inside the savestate host-state carve-out (mp6_host_section.h at
 * preprocessor depth 0, plus a HOST_STATE_SECTION_SOURCES entry). Restoring
 * a capturing process's display id into a loading one would make the
 * cross-output edge detector miss a real crossing; restoring its cached
 * display info would make the badge report an output the window is not on.
 */
#ifndef MP6_DISPLAY_H
#define MP6_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the badge and `stat fps` need about the output the window is on
 * RIGHT NOW. Plain scalars and fixed char arrays so the struct can cross the
 * C/C++ seam and the headless/windowed split without dragging a type in.
 *
 * refreshHz is the rounded integer the badge shows; refreshMilliHz is the
 * exact rate in thousandths, computed from SDL_DisplayMode's
 * refresh_rate_numerator/denominator when the denominator is nonzero. The
 * distinction is not pedantry: a 240 Hz panel reports 240000 exactly through
 * the rational pair, while Win32_VideoController rounds it to 239, and a
 * "75 Hz" virtual display can genuinely be 74.973. `stat fps` prints the
 * exact value so a rate that cannot reach the nominal number explains itself.
 *
 * presentMode is UPPERCASE-ready text, never an enum: aurora exports no
 * present-mode getter, so the value is either the exact wgpu mode name
 * scraped from aurora's own startup log line (presentModeExact = 1) or, after
 * a runtime vsync toggle, the requested CLASS only (presentModeExact = 0).
 * best_present_mode() (gpu.cpp:272-295) chooses FifoRelaxed-vs-Fifo and
 * Mailbox-vs-Immediate from surface capabilities this side cannot see, so a
 * post-toggle label must never claim one of a pair. */
typedef struct {
    int  valid;            /* 0 when no window/SDL yet -- callers print nothing */
    char name[64];         /* SDL_GetDisplayName, e.g. "LG ULTRAGEAR" */
    int  w, h;             /* the output's current mode, in its own pixels */
    int  refreshHz;        /* rounded, for the badge */
    int  refreshMilliHz;   /* exact rate * 1000, for `stat fps` */
    int  isHighestRefresh; /* 0 = some attached output refreshes faster */
    int  vsyncOn;          /* the LIVE present-sync state, not the config */
    char presentMode[24];  /* "FIFO", "FIFORELAXED", "IMMEDIATE", ... */
    int  presentModeExact; /* 1 = aurora's own logged mode; 0 = requested class */
} Mp6DisplayInfo;

/* Fills *out and returns 1, or returns 0 and leaves *out untouched when the
 * window or the SDL display query is not available. Callers must omit their
 * row/label on 0 rather than printing a made-up number -- the same discipline
 * mp6_render_res_get() already uses for its RenderRes row.
 *
 * Cheap enough for the badge's twice-per-second cadence: the display walk
 * that answers isHighestRefresh is memoized behind a 500 ms monotonic stamp,
 * and the stamp is invalidated whenever the window crosses outputs or vsync
 * is toggled, so a stale answer cannot outlive the event that changed it. */
int mp6_display_info_get(Mp6DisplayInfo *out);

/* The live present-sync state. Seeded once from the value aurora was actually
 * initialized with (config + MP6_VSYNC resolved) and updated only by
 * mp6_display_set_vsync below -- never read back out of aurora, which does
 * not track it. */
int mp6_display_vsync_enabled(void);

/* Turn present sync on/off for the REST OF THIS SESSION. No-op when the state
 * already matches, so a settings row that re-selects its current value costs
 * nothing.
 *
 * The apply is aurora_enable_vsync(), which assigns wgpu's presentMode and
 * pushes a RefreshSurface custom event; the surface is reconfigured when that
 * event is DEQUEUED, which in this port always happens between frames (see
 * aurora_bridge.c's display section for the drain-site argument). One tick of
 * latency, never a mid-frame reconfigure. */
void mp6_display_set_vsync(int on);

/* Game-thread changes; output movement and AA are applied between frames. */
int mp6_display_select(const char *name);
void mp6_tick_rate_refresh(void);
void mp6_launcher_apply_pending_settings(void);

/* How many times the window has been observed crossing to a different output
 * this session, i.e. how many forced surface reconfigures the follow logic has
 * issued. Exported so "zero extra reconfigures on a no-drag run" can be
 * asserted by a COUNTER rather than by the absence of a log line. */
long mp6_display_follow_count(void);

/* One-time init: seeds the live vsync state from what aurora actually got,
 * seeds the cross-output latch from the output the window came up on, and
 * registers the `vsync` console command. Call once with the same config.vsync
 * that was passed to aurora_initialize(), and AFTER the SDL window handle is
 * available (mp6_bridge_window_policy_init) -- the latch seed needs it.
 *
 * Without the vsync seed the badge and the follow logic would assume vsync-on
 * in an automation run that came up vsync-off (main_native.c's MP6_VSYNC
 * default). Without the LATCH seed the first genuine cross-output crossing is
 * consumed as the initial latch and does nothing -- measured, not theoretical:
 * SDL emits no display-changed event at window creation on this machine, so the
 * first drag across outputs was silently swallowed and only the second acted. */
void mp6_display_init(int vsyncOn);

/* Applies the launch position resolved below, now that aurora has created the
 * window. Call once, right after mp6_display_init().
 *
 * WHY A SECOND STEP EXISTS AT ALL. aurora's create_window() maps ANY negative
 * coordinate to SDL_WINDOWPOS_UNDEFINED (lib/window.cpp:314-318), so on a
 * desktop whose highest-refresh output sits left of the primary -- where every
 * x is negative -- the resolved position was silently discarded at exactly the
 * output the choice exists for. The shared aurora checkout must stay
 * byte-unchanged for every other lane, so this side moves the window instead:
 * SDL_SetWindowPosition has no such sentinel. A no-op when the resolver
 * declined, when the coordinates were positive (aurora already honored them),
 * or when there is no window.
 *
 * Moving across outputs is itself a crossing, so this also runs the swapchain
 * follow -- otherwise the surface would stay configured against the OS-default
 * output the window merely flashed on. */
void mp6_display_apply_launch_pos(void);

/* The cross-output reconfigure's second half, owed to the next event drain.
 * Call once per drain from every SDL event walk, immediately after
 * aurora_update() returns.
 *
 * A forced same-size Configure with an UNCHANGED present mode provably does not
 * re-associate the surface with the new output (Dawn reuses the DXGI swapchain
 * when the configuration matches; measured: presents stayed pinned at 75/s on a
 * 240 Hz output). Flipping the present mode away and back does force a real
 * swapchain creation -- but only if the two Configures see DIFFERENT modes,
 * which means they cannot be issued back to back: aurora defers the Configure
 * to the dequeue of a custom event, so a back-to-back pair would both see the
 * restored value. Hence one half per drain, and hence this call site.
 *
 * Costs one integer compare when nothing is pending, which is every frame of
 * every session in which the window never changes output. */
void mp6_display_pump_pending_reconfigure(void);

/* Records the EXACT wgpu present mode aurora chose, scraped out of aurora's
 * own "Using surface format ..., present mode X" INFO line by
 * main_native.c's log callback -- the same read-what-is-already-logged
 * technique mp6_aurora_record_max_texture_dimension_2d() uses, and for the
 * same reason: the value is a live negotiation result and the vendored
 * library is shared with every other lane, so it must not be patched to add a
 * getter. Safe to call with any token; a malformed or over-long one is
 * ignored and the label degrades to the requested class. */
void mp6_display_note_init_present_mode(const char *token);

/* Resolves the window's LAUNCH position, before aurora_initialize (aurora
 * shows the window inside it, so there is no later moment). Returns 1 and
 * writes an absolute desktop position when a choice was made, 0 when the
 * caller should keep its own SDL_WINDOWPOS_UNDEFINED default.
 *
 * launcherMode 0 (automation) returns 0 unless MP6_WINDOW_DISPLAY is set, so
 * a harness run's placement is bit-for-bit what it was before this existed.
 * reqW/reqH are the requested window size; the function mirrors aurora's own
 * create_window() defaulting (0 -> 1280x960, clamped up to 640x480) because
 * centering against the wrong rectangle is centering in the wrong place.
 *
 * ABSOLUTE COORDINATES, not SDL_WINDOWPOS_CENTERED_DISPLAY: enumerating here
 * requires initializing SDL's video subsystem and then quitting it, so that
 * aurora's own window::initialize() still performs a fresh init and its
 * pre-init SDL_HINT_ORIENTATIONS keeps its original ordering. SDL_DisplayIDs
 * do not survive that quit; desktop coordinates are OS-arrangement facts and
 * do. */
int mp6_display_resolve_launch_pos(int launcherMode, unsigned int reqW,
                                   unsigned int reqH, int *outX, int *outY);

#ifdef __cplusplus
}
#endif

#endif /* MP6_DISPLAY_H */
