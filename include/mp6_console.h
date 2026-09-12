/* MP6 native port -- the in-game developer console.
 *
 * ONE C SEAM. Nothing outside src/gx/console/ and the four call sites
 * listed below may know the console exists; every hook in a hot file is a
 * single extern call declared here, the same discipline
 * src/gx/framescope.c's mp6_fs_frame_end() and
 * src/gx/ui/launcher_core.cpp's mp6_launcher_frame_overlay() already
 * established.
 *
 * THREE UNITS BEHIND IT
 *   src/gx/console/console_core.c   -- command registry, output ring,
 *       lever (cvar) table, tokenizer, history, open/available state. Plain
 *       C (stdio/string/stdarg only) in the style of src/os/
 *       mp6_events.c, so it is in tools/build.py's PLATFORM_SOURCES_COMMON
 *       and LINKS HEADLESS. That is what lets tools/console_selftest.c and
 *       tools/test_console_contract.py drive the real code with no window.
 *   src/gx/console/console_stats.c  -- the sampler and every stat
 *       panel's TEXT. Also in both builds, split internally by #ifdef
 *       MP6_HEADLESS_BUILD exactly like src/gx/frame_dump.c.
 *   src/gx/ui/console.cpp           -- the RmlUi view of the BAR (the
 *       input line, its autocomplete popup and the log page). Aurora only.
 *       It renders what the two C units above already decided; it holds no
 *       console state of its own.
 *   src/gx/ui/overlay.cpp           -- the RmlUi view of the STAT
 *       OVERLAYS. They live in the always-on passive overlay document rather
 *       than in the console's, because they must survive the bar closing and
 *       must never claim input; that document is already alive every frame
 *       and already owns the FPS badge `stat unit` absorbs.
 *
 * AVAILABILITY IS THE AUTOMATION CONTRACT. The console starts UNAVAILABLE.
 * mp6_console_set_available(1) is called from exactly one place --
 * mp6_launcher_frame_overlay()'s first-in-game-frame block, behind that
 * function's existing `if (!g_launcherMode || !g_uiReady) return;` -- so a
 * tick-budget, --input-script or MP6_AUTO_START_TICKS run can never open
 * the bar, can never raise an OVERLAY, and can never have a keystroke
 * captured. While unavailable, open/toggle and every overlay setter are
 * no-ops and mp6_console_captures_input() is 0 by construction, not by
 * convention. tools/test_console_contract.py asserts every half.
 */
#ifndef MP6_CONSOLE_H
#define MP6_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Availability + the input BAR's state.
 *
 * THE CONSOLE IS A BOTTOM INPUT BAR, not a window. Three states, and the
 * toggle key cycles them:
 *
 *   CLOSED  nothing of the console is on screen. Stat overlays may still be
 *           up -- they are independent latches (below), which is the whole
 *           point: you play while watching the numbers.
 *   INPUT   one translucent line pinned to the bottom edge, `> ` prompt,
 *           autocomplete popup rising above it. This is the state that owns
 *           the keyboard.
 *   LOG     the same bar with the scrollback page above it. The 512-line
 *           output view lives THERE and nowhere else.
 *
 * INPUT and LOG both capture input; CLOSED never does. mp6_console_is_open()
 * means "the bar is up in either state", which is what every caller outside
 * this file actually asks (the Android touch overlay, the text-swallow guard,
 * the sampler's arming predicate).
 * ------------------------------------------------------------------- */
enum {
    MP6_CONSOLE_BAR_CLOSED = 0,
    MP6_CONSOLE_BAR_INPUT,
    MP6_CONSOLE_BAR_LOG
};

/* Latched once by the launcher (see the header comment). Setting 0 also
 * force-closes the bar AND drops every stat overlay, so losing availability
 * can never strand the input capture on nor leave an overlay drawn over a
 * torn-down UI stack. */
void mp6_console_set_available(int available);
int  mp6_console_available(void);

int  mp6_console_bar(void);           /* MP6_CONSOLE_BAR_* -- 0 when unavailable */
void mp6_console_set_bar(int state);
int  mp6_console_is_open(void);       /* bar >= INPUT */
int  mp6_console_log_page(void);      /* bar == LOG */
void mp6_console_open(void);          /* CLOSED -> INPUT; keeps LOG if already up */
void mp6_console_close(void);
void mp6_console_toggle(void);        /* CLOSED -> INPUT -> LOG -> CLOSED */
void mp6_console_toggle_log(void);    /* the modifier: INPUT <-> LOG, opens if closed */

/* THE input-capture predicate. src/gx/aurora_bridge.c's
 * mp6_pump_keyboard_to_pad() consults it before sampling the keyboard at
 * all: while the bar owns the keys, not one of them may also press a bound
 * game button. Always 0 when unavailable, and always 0 when only overlays
 * are up -- an overlay is furniture, not a surface. */
int  mp6_console_captures_input(void);

/* ---------------------------------------------------------------------
 * Stat overlays.
 *
 * EVERY PANEL IS A PERSISTENT OVERLAY, and each one is its own latch. This
 * generalizes what `stat unit` already was: a corner HUD that survived the
 * console closing, because a number you can only read while a console is
 * covering the game is a number you cannot watch while playing.
 *
 *   `stat <name>`  toggles that panel's overlay
 *   `stat none`    drops every one of them
 *
 * Overlays STACK -- the reference's FPS/ms corner readout coexists with the
 * scenerendering table -- and they are independent of the bar's state in both
 * directions: closing the bar leaves them up, and raising one does not open
 * the bar. src/gx/ui/overlay.cpp draws them, because that is the
 * document that is already alive on every frame with no input claim of its
 * own; the console document holds no panel at all any more.
 *
 * MP6_CONSOLE_PANEL_UNIT is first because it is the odd shape: a compact
 * right-aligned Label: value corner block rather than a table, and the one
 * that absorbs the launcher's FPS badge (overlay.cpp draws one or the other,
 * never both). It is a latch in the same array as every other panel -- that
 * uniformity IS this increment.
 * ------------------------------------------------------------------- */
enum {
    MP6_CONSOLE_PANEL_NONE = 0,
    MP6_CONSOLE_PANEL_UNIT,
    MP6_CONSOLE_PANEL_FPS,
    MP6_CONSOLE_PANEL_SCENE,
    MP6_CONSOLE_PANEL_GAME,
    MP6_CONSOLE_PANEL_GPU,
    MP6_CONSOLE_PANEL_MEMORY,
    MP6_CONSOLE_PANEL_AUDIO,
    MP6_CONSOLE_PANEL_BOARD,
    MP6_CONSOLE_PANEL_COUNT
};

const char *mp6_console_panel_name(int panel);
int  mp6_console_panel_from_name(const char *name); /* -1 = unknown; "none" is 0 */

/* 0 when unavailable, exactly like the bar. */
int  mp6_console_overlay(int panel);
void mp6_console_set_overlay(int panel, int on);
int  mp6_console_toggle_overlay(int panel);  /* returns the NEW state */
int  mp6_console_overlay_count(void);        /* how many are latched right now */
void mp6_console_clear_overlays(void);       /* `stat none` */

/* One panel as a RECORD BLOCK (grammar below), owned by console_stats.c.
 * Never NULL. Costs a snapshot + snprintf, so the view calls it on a tick
 * rate limit, never per present. The buffer is SHARED between panels: each
 * caller consumes the block into DOM before asking for the next one. */
const char *mp6_console_panel_text(int panel);

/* ---------------------------------------------------------------------
 * Autocomplete.
 *
 * The reference's popup rises above the input bar: prefix matches with a
 * highlighted selection, up/down to move, tab/enter to complete, and a
 * "[N more matches]" row when the list is capped. All of that needs is a
 * pure function over the command registry, so it lives HERE, in the headless
 * unit, and tools/console_selftest.c drives it with no window.
 *
 * `line` is the text left of the caret. Candidates are context-sensitive
 * because a flat command list is not what a user is completing by the time
 * they have typed `stat s`: the FIRST token completes against command names,
 * `stat <tok>` against panel names, and `get`/`set`/`unset <tok>` against
 * lever names.
 *
 * Returns the TOTAL number of matches (which may exceed `max` -- that excess
 * is the "[N more matches]" row). Fills out[0 .. min(total,max)-1] with the
 * full replacement TOKEN (not the whole line) in registry order, and
 * *tokenStart with the byte offset in `line` at which that token begins, so
 * the view splices rather than guessing where the word started. Any pointer
 * argument may be NULL. */
int mp6_console_complete(const char *line, const char **out, int max,
                         int *tokenStart);

/* ---------------------------------------------------------------------
 * PANEL RECORDS -- the wire format between the sampler and the view.
 *
 * The reference layout these panels have to match is a TABLE with
 * right-aligned numeric columns, and a proportional font cannot align
 * columns with spaces. res/fonts has no monospace face and adding one is a
 * font-licensing decision, not a port decision -- so alignment is solved in
 * LAYOUT instead: the C side emits one record per line and
 * src/gx/ui/console.cpp turns each into RmlUi flex boxes with
 * fixed-width right-aligned numeric cells.
 *
 * That is also why the panels stopped being preformatted text: a view that
 * receives "\n"-joined columns can only ever render them in one font at one
 * size, while a view that receives ROWS can right-align them, colour them
 * per the reference, draw the proportional bar, and reflow on a phone.
 *
 * Fields are TAB-separated. The first field is a one-character kind:
 *
 *   T <text>                   panel title            -- white
 *   G <text>                   group label            -- orange
 *   C <col> <col> ...          column headers         -- yellow
 *   R <kind> <bar> <cells...>  value row              -- green
 *   M <text>                   truncation note        "[N more stats ...]"
 *   N <text>                   free note line
 *
 * On an R row the first cell is the row LABEL and the rest are values,
 * right-aligned in column order. Its two leading fields are:
 *   <kind>  one character -- '.' plain, 'p' pool, 'c' category,
 *           'g' within budget (green), 'r' over budget (red),
 *           'x' excluded/informational (dim)
 *   <bar>   0..100: the reference's red proportional bar, as a percentage
 *           of the hottest row in the group. 0 draws no bar.
 * ------------------------------------------------------------------- */
#define MP6_CONSOLE_REC_TITLE   'T'
#define MP6_CONSOLE_REC_GROUP   'G'
#define MP6_CONSOLE_REC_COLS    'C'
#define MP6_CONSOLE_REC_ROW     'R'
#define MP6_CONSOLE_REC_MORE    'M'
#define MP6_CONSOLE_REC_NOTE    'N'

/* ---------------------------------------------------------------------
 * Command registry.
 * ------------------------------------------------------------------- */
typedef void (*Mp6ConsoleCmdFn)(int argc, const char *const *argv);

/* argv[0] is the command name. Returns 1 on success, 0 if the table is
 * full or the arguments are malformed. */
int mp6_console_register(const char *name, const char *help, Mp6ConsoleCmdFn fn);

/* Tokenize and dispatch one line. Returns 1 if a command ran, 0 for an
 * empty line or an unknown command (which is also reported into the output
 * ring, so the user always sees why nothing happened). */
int mp6_console_exec(const char *line);

int mp6_console_command_count(void);
const char *mp6_console_command_name(int index);
const char *mp6_console_command_help(int index);

/* ---------------------------------------------------------------------
 * Output ring.
 * ------------------------------------------------------------------- */
void mp6_console_log(const char *fmt, ...);
void mp6_console_clear_log(void);

/* Lines EVER emitted -- a generation counter the view compares against to
 * decide whether the output DOM needs rebuilding at all. */
int mp6_console_log_total(void);
/* Lines currently retained (<= the ring capacity). */
int mp6_console_log_count(void);
/* back == 0 is the newest retained line; NULL past the end. */
const char *mp6_console_log_line(int back);

/* ---------------------------------------------------------------------
 * Input history (newest first).
 * ------------------------------------------------------------------- */
int mp6_console_history_count(void);
const char *mp6_console_history(int back);

/* ---------------------------------------------------------------------
 * Runtime levers ("cvars").
 *
 * Every diagnostic stream in this port is an env var latched into one file
 * static. The console does not replace that: ENV SUPPLIES THE INITIAL
 * VALUE, A CONSOLE SETTER OVERRIDES IT. Each lever's own getter changes by
 * exactly one line --
 *
 *     return mp6_console_cvar_get(MP6_CVAR_FI_DIAG, s_diag);
 *
 * -- which both applies any override AND records the env-latched value, so
 * the toggles panel can show both numbers and say which one is winning.
 * Indexed, not name-looked-up, because some of these getters sit in
 * per-frame paths: the call is an array index and a branch.
 *
 * MP6_OM_CENSUS / MP6_DRAW_CENSUS(_OBJ) are deliberately absent. They are
 * read inside compat/decomp/ TUs, not in src/, so a runtime setter
 * for them has to write a variable the patch reads -- a decomp-patch
 * change, not a port change. The toggles panel lists them as env-only
 * rather than pretending they are settable.
 * ------------------------------------------------------------------- */
enum {
    MP6_CVAR_FI_DIAG = 0,
    MP6_CVAR_TICK_RATE_LOG,
    MP6_CVAR_PRESENT_RATE_LOG,
    MP6_CVAR_AUDIO_TIMELINE,
    MP6_CVAR_FRAMESCOPE,
    MP6_CVAR_FRAME_DUMP,
    MP6_CVAR_ALLOC_CENSUS,
    MP6_CVAR_DIAG_DRAWCOUNT,
    MP6_CVAR_SKIP_DRAW_LO,
    MP6_CVAR_SKIP_DRAW_HI,
    MP6_CVAR_ARRAYPROBE,
    MP6_CVAR_COUNT
};

int mp6_console_cvar_get(int id, int envValue);
int mp6_console_cvar_set(int id, int value);       /* 1 = accepted */
int mp6_console_cvar_clear(int id);                /* drop the override */
int mp6_console_cvar_id_from_name(const char *name); /* -1 = unknown */

const char *mp6_console_cvar_name(int id);
const char *mp6_console_cvar_env(int id);
const char *mp6_console_cvar_help(int id);
int mp6_console_cvar_min(int id);
int mp6_console_cvar_max(int id);
int mp6_console_cvar_has_override(int id);
int mp6_console_cvar_override(int id);
int mp6_console_cvar_env_seen(int id);  /* 1 once the getter has reported one */
int mp6_console_cvar_env_value(int id);

/* ---------------------------------------------------------------------
 * Sampler hooks (src/gx/console/console_stats.c). Every one of these
 * is the single extern line a hot file receives.
 * ------------------------------------------------------------------- */

/* Called from src/gx/aurora_bridge.c's mp6_present_counters_add() --
 * the ONE funnel every present passes through (the real frame end, the
 * real frame begin, and every interpolated present in frame_interp.c). A
 * present is an END; the interpolated call is the only one that passes
 * both a begin and an end, which is how replay-ness is derived here
 * without any new plumbing. */
void mp6_console_note_present(long begins, long ends);

/* Called once per tick from VIWaitForRetrace, after the phase buckets
 * close. All values in nanoseconds; the caller passes 0 for a bucket it
 * did not sample (the phase clock is inert unless the rate log is on or
 * the console is open). */
void mp6_console_note_tick_phase(long long gameNs, long long endFrameNs,
                                 long long overlayNs, long long sealNs,
                                 long long viPostNs, long long lateNs,
                                 long long slackNs);

/* 1 while the console needs the per-tick phase clock running: the BAR is up
 * OR ANY stat overlay is latched. Read by aurora_bridge.c's
 * mp6_tick_phase_now() arming predicate, by the sampler's own rings and
 * (through mp6_console_stats_active below) by the hot GX hook sites, so all
 * of them agree about when sampling is live. Overlays outlive the bar -- that
 * is exactly why this is not is_open().
 *
 * DEFINED IN console_core.c, not the sampler: it reads only the two latch
 * sets at the top of this header, so putting it beside them is what makes the
 * bar/overlay independence provable from tools/console_selftest.c. */
int mp6_console_stats_armed(void);

/* One HuPrc process slice, from src/os/process_native.c's
 * DispatchProcessAndWait -- the single boundary at which a process runs at all.
 * `entry` is process->jump.lr, the entry function HuPrcCreate stored and
 * nothing rewrites, which is what lets `stat game` name the row with the same
 * mp6_symbolize_addr() the OM census uses. Guarded at the call site by the
 * mp6_console_stats_active relaxed load, so a closed console never reads a
 * clock here. */
void mp6_console_note_proc_slice(void *entry, long long ns);

/* The GX census hook sites sit in per-draw paths, so they get ONE RELAXED LOAD
 * rather than a call -- the shape src/gx/framescope.c's mp6_fs_active()
 * and src/gx/frame_dump.c's s_fdEnabled test already use. Owned by
 * console_stats.c and refreshed once per tick from
 * mp6_console_note_tick_phase(); 0 whenever the console is closed. */
extern int mp6_console_stats_active;

/* GX census, all called from aurora_bridge.c / framescope.c. Counters are
 * per-frame and reset by mp6_console_note_frame_reset(). */
void mp6_console_note_prim(int primType, unsigned nverts);
void mp6_console_note_dl_call(unsigned nbytes);
void mp6_console_note_dl_recorded(unsigned nbytes);
void mp6_console_note_efb_copy(void);
void mp6_console_note_tex_bind(void);
void mp6_console_note_frame_reset(void);

/* ---------------------------------------------------------------------
 * Savestate (src/os/savestate.c, beside mp6_fi_savestate_reset()).
 * Both console TUs are carved out of the image, but the sampler holds
 * monotonic timestamps taken from the RUNNING process: retained across a
 * restore they describe PRE-restore frames. Drop them.
 * ------------------------------------------------------------------- */
void mp6_console_savestate_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_CONSOLE_H */
