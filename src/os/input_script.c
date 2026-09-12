/* MP6 native port -- the deterministic input-script engine.
 *
 * WHY THIS FILE EXISTS AT ALL (it is a MOVE, not new code).
 *
 * Every line of the engine below was, until this file, a private section of
 * src/gx/aurora_bridge.c -- which is AURORA-ONLY (tools/build.py's
 * PLATFORM_AURORA_ONLY). The consequence was structural, not stylistic:
 * --headless had no `--input-script` at all. Passing one was silently
 * IGNORED, argv and all, so a headless invocation of the retail Party-Mode
 * route (tools/w01_party_route.py's ROUTE) sat on the warning screen for its
 * entire tick budget while the driver reported nothing wrong. The only
 * headless input that ever existed was MP6_AUTO_START_TICKS' scheduled
 * START (src/null/shims_manual.c's PADRead), which cannot press A and
 * therefore cannot clear file-select, mode-select or party setup.
 *
 * That mattered the moment a board defect had to be proven: reaching the
 * board is an A-press route, so every board drive had to be WINDOWED -- one
 * shared GPU lock, ~6 minutes per attempt (docs/history/
 * BOARD_PAN_FLOAT_TRAP.md opens by saying exactly that). The engine has no
 * SDL, Aurora, window or focus dependency whatever: it reads this process's
 * own tick counter and the in-process event bus (include/mp6_events.h),
 * and returns PAD bits. Nothing about it was ever windowed except where it
 * happened to be compiled.
 *
 * So it lives here, in PLATFORM_SOURCES_COMMON, and both builds call the
 * same three entry points:
 *   - the Aurora build from mp6_pump_keyboard_to_pad() (unchanged call
 *     site, unchanged semantics -- it OR-merges the returned bits into the
 *     virtual PADStatus alongside the keyboard);
 *   - the headless build from src/null/shims_manual.c's
 *     VIWaitForRetrace(), one advance per tick, immediately before the
 *     game's post-retrace callback (game/pad.c's PadReadVSync) fires and
 *     samples PADRead -- the same order Aurora uses.
 *
 * The engine is INERT until mp6_input_script_init() is called, which only
 * happens for an explicit `--input-script` argv. A run without one behaves
 * exactly as it did before this file existed, in both build modes, which is
 * what keeps the headless boot log byte-identical for the ua1 log-diff gate.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <dolphin/pad.h>

#include "mp6_boot.h"    /* mp6_tick_count + the three entry points' decls */
#include "mp6_events.h"  /* the game-event bus waitev/pressuntil block on */
#include "mp6_parse.h"   /* strict operational script numbers */

/* ---------------------------------------------------------------------
 * The deterministic input-script bridge (test tooling).
 *
 * SendKeys/keybd_event-driven testing has a real reliability problem
 * independent of anything in the game itself: window-focus races (a bare
 * ALT keypress can activate a window's system menu and swallow the very
 * next key), OS input-injection timing, and possible cross-talk between
 * multiple automated test runs against similarly-named windows all
 * introduce noise that's indistinguishable, from the outside, from a
 * genuine "did the game actually see this button" question.
 * --input-script "<spec>" removes ALL of that: it injects
 * PAD button state directly into the SAME virtual-status mechanism the
 * keyboard bridge above already uses (PADSetVirtualStatus), timed purely
 * by this process's own internal tick counter -- no window focus, no OS
 * input queue, no real keyboard hardware involved at any point. This is
 * strictly a TESTING tool (real players still use the keyboard bridge
 * above, unaffected); it exists so a scripted test run is 100%
 * reproducible frame-for-frame, independent of automation timing.
 *
 * Spec syntax: semicolon-separated steps, e.g.
 * "wait:180;press:start;wait:60;press:a" --
 *   wait:N   -- advance N ticks before the next step.
 *   press:X  -- latch button X (start/a/b/up/down/left/right) for
 *               exactly one tick (matching mp6_latch_key_down_event's own
 *               one-tick-then-consumed semantics above -- a script press
 *               is exactly as "instantaneous" as a real key-down event
 *               is), then immediately move to the next step. Multiple
 *               press steps with no wait between them all land on the
 *               SAME tick (OR'd together) -- add an explicit wait:1 if a
 *               script needs them on separate ticks instead.
 *   stick:X  -- tilt the ANALOG stick (up/down/left/right, +-72 of the
 *               +-~72 hardware range) for exactly one tick, same
 *               one-tick semantics as press. Needed because several
 *               menus (file-select name entry via
 *               REL/fileseldll/filename.c) read ONLY HuPadDStkRep --
 *               game/pad.c's PadADConv derives that from the analog
 *               stick alone, so press:up/down/left/right (dpad BUTTON
 *               bits) can never move those cursors.
 *
 * EVENT-BOUND STEPS -- why wait:N is not enough
 * --------------------------------------------
 * wait:N is a TICK OFFSET, and tick offsets are the wrong unit for
 * driving this game. The boot/menu path interleaves frame-counted waits
 * with wall-clock waits (boot.c: `for (frame = 0; frame < 90; frame++)`
 * next to `while (OSTicksToMilliseconds(OSGetTick() - start) < 1500)`),
 * so the tick at which any screen becomes input-ready moves with host CPU
 * speed, host-file latency, GPU frame pacing and first-run shader
 * compilation. A press scheduled at a guessed tick lands in the wrong
 * screen and is silently swallowed -- the single most common failure mode
 * of every tick-offset route tried against this port.
 *
 * The following steps bind input to OBSERVED GAME STATE instead
 * (include/mp6_events.h owns the event vocabulary). `/` separates a
 * step's sub-arguments, since `:` already separates keyword from value:
 *
 *   waitev:<key>          -- block until event <key> fires (strictly after
 *                            this step began -- an event that already fired
 *                            earlier does NOT satisfy it, so a route can
 *                            wait on the same key twice).
 *   waitev:<key>/<value>  -- block until <key>'s latest value equals
 *                            <value>. Value matching accepts either the
 *                            event's string form or its numeric form, so
 *                            "waitev:ovl.start/w01dll" and
 *                            "waitev:ovl.start/123" are the same wait.
 *                            Unlike the bare form this is satisfied
 *                            immediately if the key ALREADY holds that
 *                            value -- it asserts a state, not an edge.
 *   pressuntil:<btn>/<key>[/<value>]
 *                         -- press <btn> every `period` ticks until that
 *                            same condition holds. This is the timing-
 *                            immune confirm: if the screen was not ready
 *                            yet, the press simply repeats. Nothing about
 *                            it depends on knowing when the screen came up.
 *   period:<N>            -- ticks between repeats for subsequent
 *                            pressuntil steps (default 60 = ~1s).
 *   timeout:<N>           -- tick cap for subsequent waitev/pressuntil
 *                            steps (default 5400 = ~90s). On expiry the
 *                            step prints "[EVENT] script.timeout=<key>"
 *                            and the script CONTINUES to the next step
 *                            rather than hanging. That is deliberate: a
 *                            stalled route must still terminate and must
 *                            leave, in the log, the exact event it was
 *                            waiting for -- which is precisely the "report
 *                            the furthest state reached" evidence a failed
 *                            run needs.
 *
 * A fully event-bound route therefore contains no tick guesses at all,
 * e.g.:
 *   "waitev:selmenu.ready;stick:right;...;pressuntil:a/ovl.start/w01dll"
 */
typedef enum {
    MP6_SCRIPT_WAIT,
    MP6_SCRIPT_PRESS,
    MP6_SCRIPT_STICK,
    MP6_SCRIPT_WAITEV,
    MP6_SCRIPT_PRESSUNTIL,
    MP6_SCRIPT_SET_PERIOD,
    MP6_SCRIPT_SET_TIMEOUT
} MP6ScriptStepType;

#define MP6_SCRIPT_EVKEY_MAX 24
#define MP6_SCRIPT_EVVAL_MAX 40

typedef struct {
    MP6ScriptStepType type;
    u32 waitFrames; /* MP6_SCRIPT_WAIT / SET_PERIOD / SET_TIMEOUT */
    u16 button;     /* MP6_SCRIPT_PRESS / MP6_SCRIPT_PRESSUNTIL */
    s8 stickX;      /* MP6_SCRIPT_STICK */
    s8 stickY;      /* MP6_SCRIPT_STICK */
    char evKey[MP6_SCRIPT_EVKEY_MAX]; /* WAITEV / PRESSUNTIL */
    char evVal[MP6_SCRIPT_EVVAL_MAX]; /* "" == any-fire (edge) form */
} MP6ScriptStep;

#define MP6_SCRIPT_MAX_STEPS 256
static MP6ScriptStep g_script[MP6_SCRIPT_MAX_STEPS];
static int g_scriptStepCount = 0;
static int g_scriptCursor = 0;
static u32 g_scriptWaitRemaining = 0;
static bool g_scriptActive = false;

/* Event-bound step state (see the spec comment above). g_scriptEvBaseSeq is
 * the global event sequence sampled when the CURRENT waitev/pressuntil step
 * was entered -- that is what makes the bare "waitev:<key>" form an EDGE
 * ("fires from now on") rather than a level ("has ever fired"), so a route
 * may wait on the same key repeatedly. g_scriptEvElapsed is the per-step
 * tick counter both the timeout and the pressuntil repeat period run off. */
static bool g_scriptEvEntered = false;
static unsigned long g_scriptEvBaseSeq = 0;
static u32 g_scriptEvElapsed = 0;
static u32 g_scriptEvPeriod = 60;    /* period:N default -- ~1s between repeats */
static u32 g_scriptEvTimeout = 5400; /* timeout:N default -- ~90s cap per wait */

static u16 mp6_script_button_from_name(const char *name, size_t len)
{
    if (len == 5 && strncmp(name, "start", 5) == 0) return PAD_BUTTON_START;
    if (len == 1 && name[0] == 'a') return PAD_BUTTON_A;
    if (len == 1 && name[0] == 'b') return PAD_BUTTON_B;
    if (len == 2 && strncmp(name, "up", 2) == 0) return PAD_BUTTON_UP;
    if (len == 4 && strncmp(name, "down", 4) == 0) return PAD_BUTTON_DOWN;
    if (len == 4 && strncmp(name, "left", 4) == 0) return PAD_BUTTON_LEFT;
    if (len == 5 && strncmp(name, "right", 5) == 0) return PAD_BUTTON_RIGHT;
    printf("[TEST] input-script: unrecognized button name (len=%zu)\n", len);
    fflush(stdout);
    return 0;
}

/* Splits an event argument span "<key>" or "<key>/<value>" into the step's
 * two bounded fields. Both are truncated rather than rejected on overflow:
 * an over-long key simply never matches a real event, which surfaces as a
 * clean, logged wait timeout instead of a parse-time abort. */
static void mp6_script_copy_ev_args(MP6ScriptStep *st, const char *span, size_t spanLen)
{
    const char *slash = (const char *)memchr(span, '/', spanLen);
    size_t keyLen = slash ? (size_t)(slash - span) : spanLen;
    size_t valLen = slash ? spanLen - keyLen - 1 : 0;

    if (keyLen >= sizeof(st->evKey)) {
        keyLen = sizeof(st->evKey) - 1;
    }
    memcpy(st->evKey, span, keyLen);
    st->evKey[keyLen] = '\0';

    if (valLen >= sizeof(st->evVal)) {
        valLen = sizeof(st->evVal) - 1;
    }
    if (valLen > 0) {
        memcpy(st->evVal, slash + 1, valLen);
    }
    st->evVal[valLen] = '\0';
}

void mp6_input_script_init(const char *spec)
{
    const char *p = spec;
    printf("[TEST] input-script armed: %s\n", spec);
    while (*p && g_scriptStepCount < MP6_SCRIPT_MAX_STEPS) {
        const char *sep = strchr(p, ';');
        size_t stepLen = sep ? (size_t)(sep - p) : strlen(p);
        const char *colon = (const char *)memchr(p, ':', stepLen);
        if (colon) {
            size_t keyLen = (size_t)(colon - p);
            const char *valStart = colon + 1;
            size_t valLen = stepLen - keyLen - 1;
            if (keyLen == 4 && strncmp(p, "wait", 4) == 0) {
                uint32_t waitFrames;
                if (mp6_parse_u32_span(valStart, valLen, 0, 10000000u, &waitFrames)) {
                    g_script[g_scriptStepCount].type = MP6_SCRIPT_WAIT;
                    g_script[g_scriptStepCount].waitFrames = waitFrames;
                    g_scriptStepCount++;
                } else {
                    printf("[TEST] input-script: invalid wait (expected 0..10000000 ticks)\n");
                }
            } else if (keyLen == 5 && strncmp(p, "press", 5) == 0) {
                g_script[g_scriptStepCount].type = MP6_SCRIPT_PRESS;
                g_script[g_scriptStepCount].button = mp6_script_button_from_name(valStart, valLen);
                g_scriptStepCount++;
            } else if (keyLen == 5 && strncmp(p, "stick", 5) == 0) {
                /* analog-stick tilt step -- see the spec comment above. */
                MP6ScriptStep *st = &g_script[g_scriptStepCount];
                st->type = MP6_SCRIPT_STICK;
                st->button = 0;
                st->stickX = 0;
                st->stickY = 0;
                if (valLen == 2 && strncmp(valStart, "up", 2) == 0) st->stickY = 72;
                else if (valLen == 4 && strncmp(valStart, "down", 4) == 0) st->stickY = -72;
                else if (valLen == 4 && strncmp(valStart, "left", 4) == 0) st->stickX = -72;
                else if (valLen == 5 && strncmp(valStart, "right", 5) == 0) st->stickX = 72;
                else { printf("[TEST] input-script: unrecognized stick direction (len=%zu)\n", valLen); }
                g_scriptStepCount++;
            } else if (keyLen == 6 && strncmp(p, "waitev", 6) == 0) {
                /* waitev:<key>[/<value>] -- see the spec comment above. */
                MP6ScriptStep *st = &g_script[g_scriptStepCount];
                st->type = MP6_SCRIPT_WAITEV;
                st->button = 0;
                mp6_script_copy_ev_args(st, valStart, valLen);
                g_scriptStepCount++;
            } else if (keyLen == 10 && strncmp(p, "pressuntil", 10) == 0) {
                /* pressuntil:<btn>/<key>[/<value>] */
                const char *slash = (const char *)memchr(valStart, '/', valLen);
                if (slash == NULL) {
                    printf("[TEST] input-script: pressuntil needs <button>/<event-key>[/<value>]\n");
                } else {
                    MP6ScriptStep *st = &g_script[g_scriptStepCount];
                    size_t btnLen = (size_t)(slash - valStart);
                    st->type = MP6_SCRIPT_PRESSUNTIL;
                    st->button = mp6_script_button_from_name(valStart, btnLen);
                    mp6_script_copy_ev_args(st, slash + 1, valLen - btnLen - 1);
                    g_scriptStepCount++;
                }
            } else if (keyLen == 6 && strncmp(p, "period", 6) == 0) {
                uint32_t ticks;
                if (mp6_parse_u32_span(valStart, valLen, 1, 100000u, &ticks)) {
                    g_script[g_scriptStepCount].type = MP6_SCRIPT_SET_PERIOD;
                    g_script[g_scriptStepCount].waitFrames = ticks;
                    g_scriptStepCount++;
                } else {
                    printf("[TEST] input-script: invalid period (expected 1..100000 ticks)\n");
                }
            } else if (keyLen == 7 && strncmp(p, "timeout", 7) == 0) {
                uint32_t ticks;
                if (mp6_parse_u32_span(valStart, valLen, 1, 10000000u, &ticks)) {
                    g_script[g_scriptStepCount].type = MP6_SCRIPT_SET_TIMEOUT;
                    g_script[g_scriptStepCount].waitFrames = ticks;
                    g_scriptStepCount++;
                } else {
                    printf("[TEST] input-script: invalid timeout (expected 1..10000000 ticks)\n");
                }
            } else {
                printf("[TEST] input-script: unrecognized step keyword (len=%zu)\n", keyLen);
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    g_scriptCursor = 0;
    g_scriptWaitRemaining = 0;
    g_scriptActive = (g_scriptStepCount > 0);
    printf("[TEST] input-script: parsed %d step(s)\n", g_scriptStepCount);
    fflush(stdout);
}

/* Called EXACTLY ONCE PER TICK, before the caller computes the PADStatus
 * the game will sample this tick -- advances the script state machine and
 * returns any button(s) that should be latched THIS tick (0 if none, or no
 * script armed at all). Aurora calls it from mp6_pump_keyboard_to_pad();
 * headless calls it from VIWaitForRetrace(), before the post-retrace
 * callback that reads PADRead. Calling it twice in one tick would consume
 * two steps' worth of script, which is why neither caller puts it in
 * PADRead itself (game/sreset.c calls PADRead off the retrace path too). */
static s8 g_scriptTickStickX = 0; /* this tick's scripted analog tilt */
static s8 g_scriptTickStickY = 0;

u16 mp6_input_script_advance(void)
{
    u16 pressedThisTick = 0;
    g_scriptTickStickX = 0;
    g_scriptTickStickY = 0;
    while (g_scriptActive && g_scriptCursor < g_scriptStepCount) {
        MP6ScriptStep *step = &g_script[g_scriptCursor];
        if (step->type == MP6_SCRIPT_WAIT) {
            if (g_scriptWaitRemaining == 0) {
                if (step->waitFrames == 0) {
                    g_scriptCursor++;
                    continue; /* zero-length wait -- no-op separator, move on now */
                }
                g_scriptWaitRemaining = step->waitFrames;
            }
            g_scriptWaitRemaining--;
            if (g_scriptWaitRemaining == 0) {
                g_scriptCursor++;
            }
            break; /* this tick is spent on the wait either way */
        } else if (step->type == MP6_SCRIPT_SET_PERIOD) {
            g_scriptEvPeriod = step->waitFrames;
            g_scriptCursor++;
            continue; /* configuration only -- costs no tick */
        } else if (step->type == MP6_SCRIPT_SET_TIMEOUT) {
            g_scriptEvTimeout = step->waitFrames;
            g_scriptCursor++;
            continue; /* configuration only -- costs no tick */
        } else if (step->type == MP6_SCRIPT_WAITEV || step->type == MP6_SCRIPT_PRESSUNTIL) {
            /* The event-bound steps -- the whole point of this engine (see
             * the spec comment). Both share one condition evaluator; the
             * only difference is that PRESSUNTIL also emits its button on
             * entry and then once per `period` ticks while it waits. */
            bool satisfied;
            if (!g_scriptEvEntered) {
                g_scriptEvEntered = true;
                g_scriptEvBaseSeq = mp6_event_seq();
                g_scriptEvElapsed = 0;
                printf("[EVENT] script.wait=%s num=0 tick=%ld seq=%lu\n",
                       step->evKey, mp6_tick_count, g_scriptEvBaseSeq);
                fflush(stdout);
                if (step->type == MP6_SCRIPT_PRESSUNTIL) {
                    pressedThisTick |= step->button; /* first attempt lands immediately */
                }
            }
            if (step->evVal[0] != '\0') {
                /* Value form asserts a STATE: satisfied the moment the key
                 * holds that value, even if it got there before this step
                 * (a route that says "be in overlay X" is already right if
                 * it is already in overlay X). */
                satisfied = mp6_event_matches(step->evKey, step->evVal) != 0;
            } else {
                /* Bare form asserts an EDGE: only a fire strictly after
                 * this step began counts. */
                satisfied = mp6_event_last_seq(step->evKey) > g_scriptEvBaseSeq;
            }
            if (satisfied) {
                printf("[EVENT] script.satisfied=%s num=%u tick=%ld seq=%lu\n",
                       step->evKey, (unsigned)g_scriptEvElapsed, mp6_tick_count, mp6_event_seq());
                fflush(stdout);
                g_scriptEvEntered = false;
                g_scriptCursor++;
                break; /* the satisfying tick is spent; next step starts next tick */
            }
            g_scriptEvElapsed++;
            if (step->type == MP6_SCRIPT_PRESSUNTIL &&
                g_scriptEvPeriod != 0 && (g_scriptEvElapsed % g_scriptEvPeriod) == 0) {
                pressedThisTick |= step->button;
            }
            if (g_scriptEvElapsed >= g_scriptEvTimeout) {
                /* Deliberately non-fatal: the run must still terminate and
                 * the log must name the exact event it never saw. */
                printf("[EVENT] script.timeout=%s num=%u tick=%ld seq=%lu\n",
                       step->evKey, (unsigned)g_scriptEvElapsed, mp6_tick_count, mp6_event_seq());
                fflush(stdout);
                g_scriptEvEntered = false;
                g_scriptCursor++;
            }
            break; /* this tick is spent waiting either way */
        } else if (step->type == MP6_SCRIPT_STICK) {
            g_scriptTickStickX = step->stickX; /* last stick step this tick wins */
            g_scriptTickStickY = step->stickY;
            g_scriptCursor++;
            /* keep looping: consume any further zero-gap steps too */
        } else {
            pressedThisTick |= step->button;
            g_scriptCursor++;
            /* keep looping: consume any further zero-gap press steps too */
        }
    }
    if (g_scriptActive && g_scriptCursor >= g_scriptStepCount) {
        g_scriptActive = false;
        printf("[TEST] input-script: complete\n");
        fflush(stdout);
    }
    return pressedThisTick;
}

/* This tick's scripted analog tilt, as latched by the advance above. Split
 * out of the button return value because the two go to different PADStatus
 * fields and a caller that has no analog concept can simply not ask. Both
 * outputs are zero unless a stick: step landed on this exact tick. */
void mp6_input_script_stick_get(s8 *stickX, s8 *stickY)
{
    if (stickX != NULL) *stickX = g_scriptTickStickX;
    if (stickY != NULL) *stickY = g_scriptTickStickY;
}

/* Nonzero once mp6_input_script_init() has armed at least one step. The
 * headless PAD path uses it to decide whether to synthesise a status at all,
 * so a run with no --input-script keeps the byte-identical pre-existing
 * behaviour the boot log-diff baseline was recorded against. */
int mp6_input_script_armed(void)
{
    return g_scriptStepCount > 0;
}
