/* MP6 native port -- developer console SAMPLER and stat-panel text.
 *
 * Two jobs, and the split between them is the whole point of this file.
 *
 * 1. SAMPLE what nothing else records. There are exactly two such things: a
 *    ring of per-present timestamps (nothing in this tree computes a 1% or
 *    0.1% low -- every existing line reports avg/max only, which cannot see a
 *    hitch), and a ring of per-tick phase quadruples (aurora_bridge.c
 *    accumulates those into window SUMS and resets them every 5 seconds, so
 *    the individual ticks are gone by the time anyone looks).
 *
 * 2. READ everything else. The counters this port already maintains are not
 *    re-implemented here, and that is a hard rule rather than a preference:
 *    platform/gx/frame_interp.c owns a PERMANENT decline census and cost
 *    accounting -- incremented on every path whether or not MP6_FI_DIAG prints
 *    -- and a second copy kept here could disagree with the [MP6-FI-CENSUS]
 *    line about why a window produced no interpolated present. So the
 *    fps/unit panels call mp6_fi_stats_get() and report what that census
 *    already knows. tools/test_console_contract.py asserts this file holds no
 *    decline counter of its own.
 *
 * COST WHEN CLOSED. mp6_console_stats_active is one relaxed int load -- the
 * shape platform/gx/framescope.c's mp6_fs_active() and platform/gx/
 * frame_dump.c's s_fdEnabled test already use -- and the GX census hook sites
 * read it before calling in. The per-present and per-tick hooks are called
 * unconditionally (60-500/s, immaterial) and return before touching a clock
 * or a ring while the console is closed.
 *
 * BUILD SPLIT. In tools/build.py's PLATFORM_SOURCES_COMMON like frame_dump.c,
 * and split internally by #ifdef MP6_HEADLESS_BUILD for the same reason: the
 * memory/audio/board panels are pure C over probes that exist in both builds
 * (so a headless run can answer them too), while fps/unit/scenerendering need
 * aurora and the GX bridge, and say so plainly when asked headless.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp6_console.h"
#include "mp6_diag_probe.h"
#include "mp6_boot.h" /* mp6_tick_count, mp6_symbolize_addr */
#include "host.h"     /* mp6_host_monotonic_ns / _rss_bytes / coro slots */

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md). Every ring below holds monotonic
 * timestamps taken from the RUNNING process's timer. Restored from a capturing
 * process they would mix two timelines into one percentile window and report a
 * "frame time" measured across the load itself. Registered in tools/build.py
 * HOST_STATE_SECTION_SOURCES; placed AFTER this file's own includes at
 * preprocessor top level, which verify_host_section_sources() enforces. */
#include "mp6_host_section.h"


/* One relaxed load for the hot GX hook sites. Refreshed once per tick from
 * mp6_console_note_tick_phase(), which runs on every tick regardless -- so a
 * closed console costs each of those sites one load and a branch. */
int mp6_console_stats_active = 0;

/* Defined further down and called from the sampler hooks above them. The two
 * *_counters_* halves are renderer-only (stubbed in the headless build); the
 * window boundary itself is common, because `stat game`'s process rows ride
 * the same window and those probes link in both builds. */
static void cs_frame_counters_sample(void); /* per-frame Counters accumulation */
static void cs_counters_roll(void);         /* publish + zero the GX counters */
static void cs_window_roll(void);           /* the window boundary itself */
static void cs_game_counters_sample(void);  /* per-TICK Counters accumulation */
static void cs_game_roll(void);             /* publish + zero the game counters */

/* =======================================================================
 * 1. Rings.
 * ======================================================================= */

#define CS_PRESENT_RING 1024 /* 0.1% of a full ring is ~1 sample; the panel
                              * prints n so that is visible, not implied */
#define CS_PHASE_RING    512

static long long     s_presentNs[CS_PRESENT_RING];
static unsigned char s_presentReplay[CS_PRESENT_RING];
static int           s_presentHead;
static int           s_presentCount;

typedef struct {
    long long game, endFrame, overlay, seal, viPost, late, slack;
} CsPhase;

static CsPhase s_phase[CS_PHASE_RING];
static int     s_phaseHead;
static int     s_phaseCount;

static long s_presentsTotal;
static long s_replaysTotal;

/* Rate window: presents and ticks counted between two panel refreshes, so the
 * fps panel reports a MEASURED rate and not the configured one. */
static long long s_rateWindowNs;
static long      s_rateWindowTick;
static long      s_rateWindowPresents;
static long      s_rateWindowReplays;
static double    s_rateTicksPerSec;
static double    s_ratePresentsPerSec;
static double    s_rateReplaysPerSec;

void mp6_console_note_present(long begins, long ends)
{
    int slot;
    if (ends <= 0) return;
    /* Replay-ness with no new plumbing: mp6_present_counters_add() has exactly
     * three call sites -- the real frame END passes (0,1), the real frame
     * BEGIN passes (1,0), and frame_interp.c's interpolated present is the
     * only one that passes both. So "an end that also carried a begin" IS an
     * interpolated present. */
    s_presentsTotal++;
    if (begins > 0) s_replaysTotal++;
    if (!mp6_console_stats_armed()) return;
    slot = s_presentHead;
    s_presentNs[slot] = (long long)mp6_host_monotonic_ns();
    s_presentReplay[slot] = (begins > 0) ? 1u : 0u;
    s_presentHead = (s_presentHead + 1) % CS_PRESENT_RING;
    if (s_presentCount < CS_PRESENT_RING) s_presentCount++;
}

void mp6_console_note_tick_phase(long long gameNs, long long endFrameNs,
                                 long long overlayNs, long long sealNs,
                                 long long viPostNs, long long lateNs,
                                 long long slackNs)
{
    CsPhase *p;
    mp6_console_stats_active = mp6_console_stats_armed();
    if (!mp6_console_stats_active) return;
    cs_window_roll();
    /* AFTER the roll, so this tick's sample lands in the window that is now
     * open rather than in the one just published. `stat game`'s Counters are
     * per-TICK quantities (processes alive, the OM walk's verdicts, events
     * posted, voices sounding), so this -- not the frame reset -- is their
     * sampling point, and it is in the common build because every probe
     * behind them links headless. */
    cs_game_counters_sample();
    p = &s_phase[s_phaseHead];
    p->game = gameNs;
    p->endFrame = endFrameNs;
    p->overlay = overlayNs;
    p->seal = sealNs;
    p->viPost = viPostNs;
    p->late = lateNs;
    p->slack = slackNs;
    s_phaseHead = (s_phaseHead + 1) % CS_PHASE_RING;
    if (s_phaseCount < CS_PHASE_RING) s_phaseCount++;
}

/* mp6_console_stats_armed() -- THE one arming predicate -- used to live here,
 * beside the rings it gates. It moved to console_core.c because it reads
 * nothing from this file: it is a pure function of the bar state and the
 * overlay latches, which are console STATE. That move is what lets
 * tools/console_selftest.c prove the property that matters -- a latched
 * overlay keeps the sampler armed after the bar closes -- against the real
 * code, with no window and no sampler stubs. */

/* =======================================================================
 * 2. GX census.
 *
 * Per-frame counters, snapshotted into a "last complete frame" set by
 * mp6_console_note_frame_reset() at the point aurora_bridge.c already resets
 * its own per-frame draw index. Reporting the PREVIOUS frame is what
 * MP6_DIAG_DRAWCOUNT does too, and for the same reason: a panel reading the
 * live counters would show a frame caught mid-emission.
 * ======================================================================= */

static unsigned s_primCur[8], s_primLast[8];
static unsigned s_vertsCur, s_vertsLast;
static unsigned s_dlCallsCur, s_dlCallsLast;
static unsigned s_dlBytesCur, s_dlBytesLast;
static unsigned s_dlRecBytesCur, s_dlRecBytesLast;
static unsigned s_efbCopiesCur, s_efbCopiesLast;
static unsigned s_texBindsCur, s_texBindsLast;
static unsigned s_drawIndexLast;

void mp6_console_note_prim(int primType, unsigned nverts)
{
    /* GXPrimitive is 0x80..0xB8 in steps of 8 (quads .. points). */
    int idx = (primType - 0x80) >> 3;
    if (idx >= 0 && idx < 8) s_primCur[idx]++;
    s_vertsCur += nverts;
}

void mp6_console_note_dl_call(unsigned nbytes)
{
    s_dlCallsCur++;
    s_dlBytesCur += nbytes;
}

void mp6_console_note_dl_recorded(unsigned nbytes) { s_dlRecBytesCur += nbytes; }
void mp6_console_note_efb_copy(void)               { s_efbCopiesCur++; }
void mp6_console_note_tex_bind(void)               { s_texBindsCur++; }

void mp6_console_note_frame_reset(void)
{
    int i;
#ifndef MP6_HEADLESS_BUILD
    { extern unsigned mp6_current_draw_index(void); s_drawIndexLast = mp6_current_draw_index(); }
#else
    s_drawIndexLast = 0; /* no GX bridge headless -- the scene panel says so */
#endif
    for (i = 0; i < 8; i++) { s_primLast[i] = s_primCur[i]; s_primCur[i] = 0; }
    s_vertsLast = s_vertsCur;             s_vertsCur = 0;
    s_dlCallsLast = s_dlCallsCur;         s_dlCallsCur = 0;
    s_dlBytesLast = s_dlBytesCur;         s_dlBytesCur = 0;
    s_dlRecBytesLast = s_dlRecBytesCur;   s_dlRecBytesCur = 0;
    s_efbCopiesLast = s_efbCopiesCur;     s_efbCopiesCur = 0;
    s_texBindsLast = s_texBindsCur;       s_texBindsCur = 0;
    /* The Counters block is Average|Max|Min over a window, so the frame that
     * just completed is one SAMPLE, not the answer. */
    if (mp6_console_stats_active) cs_frame_counters_sample();
}

/* =======================================================================
 * 2b. HuPrc process slices -- `stat game`'s cycle rows.
 *
 * The retail game's "game thread" is the HuPrc process list, so the reference's
 * per-stage breakdown maps onto PER-PROCESS execution time. platform/os/
 * process_native.c's DispatchProcessAndWait is the one boundary a process runs
 * across, and it hands each slice here keyed by the process's entry function --
 * the same identity the OM census symbolizes objFuncs with.
 *
 * Rows are keyed by entry POINTER and symbolized lazily, once, at panel build
 * time: mp6_symbolize_addr() goes through dbghelp, which must never run on the
 * dispatcher path. The counters are windowed exactly like the Counters block
 * (cs_window_roll publishes and zeroes), so CallCount is genuinely
 * slices-per-window rather than a total that only ever grows.
 * ======================================================================= */

#define CS_PROC_MAX 64
#define CS_PROC_ROWS 24 /* the reference's row cap; the rest become "[N more]" */

typedef struct {
    void      *entry;
    char       name[56];
    int        named;
    long       calls;      /* current window */
    long long  sumNs, maxNs;
    long       lastCalls;  /* previous COMPLETE window -- what the panel reads */
    long long  lastSumNs, lastMaxNs;
} CsProcRow;

static CsProcRow s_proc[CS_PROC_MAX];
static int       s_procN;
static long      s_procDropped; /* slices whose entry did not fit the table */

void mp6_console_note_proc_slice(void *entry, long long ns)
{
    int i;
    CsProcRow *r;
    if (ns < 0) return;
    for (i = 0; i < s_procN; i++) {
        if (s_proc[i].entry == entry) {
            r = &s_proc[i];
            goto found;
        }
    }
    if (s_procN >= CS_PROC_MAX) { s_procDropped++; return; }
    r = &s_proc[s_procN++];
    r->entry = entry;
    r->named = 0;
found:
    r->calls++;
    r->sumNs += ns;
    if (ns > r->maxNs) r->maxNs = ns;
}

/* The rolling window every Average|Max|Min and every CallCount is measured
 * over. Long enough for a Min to mean something, short enough to react to
 * while looking at it. */
#define CS_WINDOW_TICKS 120 /* 2 s at 60 Hz */

static long s_windowTicks;
static long s_windowTicksLast;

static void cs_proc_roll(void);

static void cs_window_roll(void)
{
    if (++s_windowTicks < CS_WINDOW_TICKS) return;
    s_windowTicksLast = s_windowTicks;
    s_windowTicks = 0;
    cs_proc_roll();
    cs_counters_roll();
    cs_game_roll();
}

static void cs_proc_roll(void)
{
    int i;
    for (i = 0; i < s_procN; i++) {
        s_proc[i].lastCalls = s_proc[i].calls;
        s_proc[i].lastSumNs = s_proc[i].sumNs;
        s_proc[i].lastMaxNs = s_proc[i].maxNs;
        s_proc[i].calls = 0;
        s_proc[i].sumNs = 0;
        s_proc[i].maxNs = 0;
    }
}

/* dbghelp resolves to "func+0xNN (file:line)"; the row wants the symbol. */
static void cs_proc_name(CsProcRow *r)
{
    char sym[256];
    size_t i;
    if (r->named) return;
    r->named = 1;
    sym[0] = '\0';
    if (r->entry != NULL) mp6_symbolize_addr(r->entry, sym, sizeof(sym));
    for (i = 0; sym[i] != '\0'; i++) {
        if (sym[i] == '+' || sym[i] == ' ' || sym[i] == '(') { sym[i] = '\0'; break; }
    }
    if (sym[0] == '\0' || sym[0] == '<') {
        /* Unresolved -- hex, never a made-up name. */
        snprintf(r->name, sizeof(r->name), "0x%08lX",
                 (unsigned long)(uintptr_t)r->entry);
        return;
    }
    snprintf(r->name, sizeof(r->name), "%s", sym);
}

/* =======================================================================
 * 3. Panel record buffer.
 *
 * The grammar (T/G/C/R/M/N, tab-separated) is documented once, in
 * shim/include/mp6_console.h beside mp6_console_panel_text(). Everything here
 * is the emitter for it: cs_put appends without a newline so a row can be
 * assembled cell by cell, and every panel below is written in terms of
 * cs_title, cs_group, cs_cols, cs_row_begin/cs_cell/cs_row_end, cs_more and
 * cs_note rather than laying anything out itself.
 * ======================================================================= */

#define CS_TEXT_MAX 16384
static char   s_text[CS_TEXT_MAX];
static size_t s_textUsed;

static void cs_reset(void) { s_textUsed = 0; s_text[0] = '\0'; }

static void cs_vput(const char *fmt, va_list ap)
{
    int n;
    if (s_textUsed + 2 >= CS_TEXT_MAX) return;
    n = vsnprintf(s_text + s_textUsed, CS_TEXT_MAX - s_textUsed, fmt, ap);
    if (n < 0) return;
    s_textUsed += (size_t)n;
    if (s_textUsed >= CS_TEXT_MAX - 1) s_textUsed = CS_TEXT_MAX - 1;
    s_text[s_textUsed] = '\0';
}

static void cs_put(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    cs_vput(fmt, ap);
    va_end(ap);
}

static void cs_nl(void)
{
    if (s_textUsed + 2 >= CS_TEXT_MAX) return;
    s_text[s_textUsed++] = '\n';
    s_text[s_textUsed] = '\0';
}

static void cs_line(const char *fmt, ...) /* one whole record, already tabbed */
{
    va_list ap;
    va_start(ap, fmt);
    cs_vput(fmt, ap);
    va_end(ap);
    cs_nl();
}

static void cs_title(const char *fmt, ...)
{
    va_list ap;
    cs_put("T\t");
    va_start(ap, fmt);
    cs_vput(fmt, ap);
    va_end(ap);
    cs_nl();
}

static void cs_group(const char *fmt, ...)
{
    va_list ap;
    cs_put("G\t");
    va_start(ap, fmt);
    cs_vput(fmt, ap);
    va_end(ap);
    cs_nl();
}

static void cs_note(const char *fmt, ...)
{
    va_list ap;
    cs_put("N\t");
    va_start(ap, fmt);
    cs_vput(fmt, ap);
    va_end(ap);
    cs_nl();
}

static void cs_more(int hidden)
{
    if (hidden > 0) cs_line("M\t[%d more stats ...]", hidden);
}

/* `cols` is already tab-separated; every caller writes it as one literal so
 * the column ORDER is readable at the call site instead of assembled. */
static void cs_cols(const char *cols) { cs_line("C\t%s", cols); }

static void cs_row_begin(char kind, int barPct)
{
    if (barPct < 0) barPct = 0;
    if (barPct > 100) barPct = 100;
    cs_put("R\t%c\t%d", kind, barPct);
}

static void cs_cell(const char *fmt, ...)
{
    va_list ap;
    cs_put("\t");
    va_start(ap, fmt);
    cs_vput(fmt, ap);
    va_end(ap);
}

static void cs_row_end(void) { cs_nl(); }

/* =======================================================================
 * 3b. Rolling window accumulators.
 *
 * The reference's Counters block is Average | Max | Min over a window, and
 * neither a per-frame snapshot nor a cumulative total can produce a Min. So
 * each counter gets a two-buffer accumulator: samples land in `cur`, and every
 * CS_WINDOW_TICKS ticks `cur` is published into `last` and zeroed. The panel
 * reads `last` -- the previous COMPLETE window -- which is the same discipline
 * the GX census already uses for the previous complete FRAME, and for the same
 * reason: a half-filled window's Min is whatever happened to arrive first.
 * ======================================================================= */

typedef struct {
    double sum, mn, mx;
    long   n;
} CsAcc;

static void cs_acc_add(CsAcc *a, double v)
{
    if (a->n == 0 || v < a->mn) a->mn = v;
    if (a->n == 0 || v > a->mx) a->mx = v;
    a->sum += v;
    a->n++;
}

static double cs_acc_avg(const CsAcc *a) { return a->n ? a->sum / (double)a->n : 0.0; }

/* The window label every panel that reads a rolled accumulator prints, so
 * "over what?" is never a guess -- and so a window that has not completed once
 * yet says so instead of showing an empty table. */
static void cs_window_note(void)
{
    if (s_windowTicksLast == 0) {
        cs_note("Rolling window: still filling its first %d ticks -- rows read '-' until then.",
                CS_WINDOW_TICKS);
    } else {
        cs_note("Rolling window: the previous complete %ld ticks (~%.1f s at the configured rate).",
                s_windowTicksLast, (double)s_windowTicksLast / 60.0);
    }
}

/* One Counters row from an accumulator. Empty windows print '-' rather than a
 * zero, because "no sample" and "measured zero" are different answers. */
static void cs_acc_row(const CsAcc *a, const char *label, const char *unitFmt)
{
    cs_row_begin('.', 0);
    cs_cell("%s", label);
    if (a->n == 0) {
        cs_cell("-"); cs_cell("-"); cs_cell("-");
    } else {
        cs_cell(unitFmt, cs_acc_avg(a));
        cs_cell(unitFmt, a->mx);
        cs_cell(unitFmt, a->mn);
    }
    cs_row_end();
}

/* The Counters block's one column header, everywhere it appears. It is a
 * define rather than a literal per panel because "Average | Max | Min over
 * the rolling window" is the reference's contract for the WHOLE block: a
 * panel that quietly printed a bare Value column under the same "Counters"
 * label would be claiming window semantics it does not have. */
#define CS_COUNTER_COLS "Counter\tAverage\tMax\tMin"

/* --- `stat game`'s per-TICK counter set -------------------------------
 *
 * Same two-buffer discipline as the GX counters above, but sampled per TICK
 * from mp6_console_note_tick_phase() and living in the COMMON build, because
 * every probe behind these rows (the scheduler's process list, the OM walk's
 * verdict tallies, the event bus, the mixer) exists headless too.
 *
 * Events are the one derived row: the bus keeps cumulative per-key totals and
 * no global counter, so "events posted" is the DIFFERENCE between consecutive
 * ticks -- which is also the only form with a meaningful Min. A negative delta
 * (a restore rewinding the totals) is clamped to zero rather than reported.
 * ------------------------------------------------------------------- */
enum {
    CS_G_PROCESSES = 0,
    CS_G_FIBERS,
    CS_G_OM_RAN,
    CS_G_OM_GATED,
    CS_G_OM_NULL,
    CS_G_OM_DEAD,
    CS_G_EVENTS,
    CS_G_VOICES,
    CS_G_CHANS,
    CS_G_COUNT
};

static CsAcc s_gCur[CS_G_COUNT];
static CsAcc s_gLast[CS_G_COUNT];
static unsigned long s_gEventsPrev;
static int           s_gEventsSeen;

static void cs_game_roll(void)
{
    memcpy(s_gLast, s_gCur, sizeof(s_gLast));
    memset(s_gCur, 0, sizeof(s_gCur));
}

static unsigned long cs_event_total(void)
{
    int i, slots = mp6_diag_event_slot_count();
    unsigned long total = 0;
    for (i = 0; i < slots; i++) {
        unsigned long c = 0;
        mp6_diag_event_slot(i, NULL, NULL, NULL, &c, NULL);
        total += c;
    }
    return total;
}

static void cs_game_counters_sample(void)
{
    Mp6DiagAudio audio;
    unsigned long events;
    int ran = 0, funcNull = 0, gated = 0, dead = 0;

    cs_acc_add(&s_gCur[CS_G_PROCESSES], (double)mp6_diag_process_count());
    cs_acc_add(&s_gCur[CS_G_FIBERS], (double)mp6_diag_process_fibers());

    mp6_diag_om_tallies(&ran, &funcNull, &gated, &dead);
    cs_acc_add(&s_gCur[CS_G_OM_RAN], (double)ran);
    cs_acc_add(&s_gCur[CS_G_OM_GATED], (double)gated);
    cs_acc_add(&s_gCur[CS_G_OM_NULL], (double)funcNull);
    cs_acc_add(&s_gCur[CS_G_OM_DEAD], (double)dead);

    events = cs_event_total();
    if (s_gEventsSeen) {
        cs_acc_add(&s_gCur[CS_G_EVENTS],
                   (events >= s_gEventsPrev) ? (double)(events - s_gEventsPrev) : 0.0);
    }
    s_gEventsPrev = events;
    s_gEventsSeen = 1;

    /* The one probe here that is not a plain load: it takes the mixer lock and
     * copies a fixed struct (no formatting inside the lock -- msm_bridge.c's
     * own rule). Once per tick while the console is ARMED only, which is the
     * same budget the audio panel already spends four times a second. */
    mp6_diag_audio_snapshot(&audio);
    cs_acc_add(&s_gCur[CS_G_VOICES], (double)audio.voicesActive);
    cs_acc_add(&s_gCur[CS_G_CHANS], (double)audio.chansActive);
}

/* =======================================================================
 * 3c. Cycle-counter rows.
 *
 * Shared by `stat scenerendering` and `stat game`: both sections are the
 * reference's "Cycle counters (flat)" -- the same six columns, sorted by
 * InclusiveAvg descending, with the red proportional bar scaled against the
 * hottest row and a "[N more stats ...]" note when the list is capped.
 * ======================================================================= */

#define CS_CYCLE_COLS "Stage\tCallCount\tInclusiveAvg\tInclusiveMax\tExclusiveAvg\tExclusiveMax"

typedef struct {
    const char *name;
    long        calls;
    double      inclAvg, inclMax, exclAvg, exclMax;
} CsCycleRow;

static void cs_cycle_emit(CsCycleRow *rows, int n, int cap)
{
    int i, j;
    double top;
    /* Insertion sort: n is <= the row cap (24) and this runs at the panel
     * refresh rate, not per frame. */
    for (i = 1; i < n; i++) {
        CsCycleRow key = rows[i];
        for (j = i - 1; j >= 0 && rows[j].inclAvg < key.inclAvg; j--) rows[j + 1] = rows[j];
        rows[j + 1] = key;
    }
    cs_cols(CS_CYCLE_COLS);
    top = (n > 0) ? rows[0].inclAvg : 0.0;
    for (i = 0; i < n && i < cap; i++) {
        int bar = (top > 0.0) ? (int)((rows[i].inclAvg / top) * 100.0 + 0.5) : 0;
        cs_row_begin('.', bar);
        cs_cell("%s", rows[i].name);
        cs_cell("%ld", rows[i].calls);
        cs_cell("%.2f", rows[i].inclAvg);
        cs_cell("%.2f", rows[i].inclMax);
        cs_cell("%.2f", rows[i].exclAvg);
        cs_cell("%.2f", rows[i].exclMax);
        cs_row_end();
    }
    cs_more(n - cap);
}

/* =======================================================================
 * 4. Panels available in BOTH builds -- every field behind them comes from
 *    shim/include/mp6_diag_probe.h, which links in both.
 * ======================================================================= */

/* The reference's memory table: UsedMax | Mem% | MemPool | Pool Capacity, ALL
 * rows sorted by usage descending -- pools and categories interleaved, exactly
 * as the reference image shows them, rather than pools-then-categories in
 * source order. That is why every row is built into this array first and
 * emitted afterwards: a sort cannot happen while rows are being printed.
 *
 * POOL rows carry all four columns (used, percent of capacity, pool name,
 * capacity); CATEGORY rows carry UsedMax plus a tag only. A number that is not
 * cheaply available is omitted rather than guessed -- which is why the OS
 * arena rows are categories (a bump heap reports what is LEFT, not a capacity)
 * and why there is no RmlUi-memory row at all.
 *
 * SORT KEY IS ALWAYS BYTES. Every row that appears here has a byte quantity
 * behind it even when its cell is not formatted in MB, which is what makes one
 * ordering meaningful across pools and categories at all; a row with no byte
 * quantity sorts last on a negative key rather than being wedged in at zero
 * beside a genuinely empty heap. */
#define CS_MEM_COLS "Counter\tUsedMax\tMem%\tMemPool\tPool Capacity"
#define CS_MEM_ROWS 24

typedef struct {
    char   label[56];
    char   used[24];
    char   pct[16];
    char   pool[24];
    char   cap[24];
    double sortBytes; /* < 0 sorts last */
    int    bar;
    char   kind;      /* 'p' pool, 'c' category, 'x' informational */
} CsMemRow;

static CsMemRow s_memRows[CS_MEM_ROWS];
static int      s_memN;

static CsMemRow *cs_mem_row(char kind, const char *label)
{
    CsMemRow *r;
    if (s_memN >= CS_MEM_ROWS) return NULL;
    r = &s_memRows[s_memN++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->sortBytes = -1.0;
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->used, sizeof(r->used), "-");
    snprintf(r->pct, sizeof(r->pct), "-");
    snprintf(r->pool, sizeof(r->pool), "-");
    snprintf(r->cap, sizeof(r->cap), "-");
    return r;
}

static void cs_mem_pool(const char *label, double usedB, double capB, const char *pool)
{
    CsMemRow *r = cs_mem_row('p', label);
    if (r == NULL) return;
    r->sortBytes = usedB;
    r->bar = (capB > 0.0) ? (int)((usedB / capB) * 100.0 + 0.5) : 0;
    snprintf(r->used, sizeof(r->used), "%.2f MB", usedB / 1048576.0);
    if (capB > 0.0) {
        snprintf(r->pct, sizeof(r->pct), "%.1f%%", 100.0 * usedB / capB);
        snprintf(r->cap, sizeof(r->cap), "%.2f MB", capB / 1048576.0);
    }
    snprintf(r->pool, sizeof(r->pool), "%s", pool);
}

/* A pool whose unit is SLOTS, not bytes. It exists because formatting a slot
 * count through the MB path printed "0.00 MB" for the coroutine table -- a
 * number that is not wrong so much as meaningless. The cells say slots; the
 * SORT still uses the bytes those slots reserve, so the row lands where its
 * real footprint puts it. */
static void cs_mem_slot_pool(const char *label, double inUse, double slots,
                             double bytesEach, const char *pool)
{
    CsMemRow *r = cs_mem_row('p', label);
    if (r == NULL) return;
    r->sortBytes = inUse * bytesEach;
    r->bar = (slots > 0.0) ? (int)((inUse / slots) * 100.0 + 0.5) : 0;
    snprintf(r->used, sizeof(r->used), "%.0f slots", inUse);
    if (slots > 0.0) {
        snprintf(r->pct, sizeof(r->pct), "%.1f%%", 100.0 * inUse / slots);
        snprintf(r->cap, sizeof(r->cap), "%.0f x %.2f MB", slots, bytesEach / 1048576.0);
    }
    snprintf(r->pool, sizeof(r->pool), "%s", pool);
}

static void cs_mem_category(const char *label, const char *usedFmt, double used,
                            double sortBytes, const char *tag)
{
    CsMemRow *r = cs_mem_row('c', label);
    if (r == NULL) return;
    r->sortBytes = sortBytes;
    snprintf(r->used, sizeof(r->used), usedFmt, used);
    snprintf(r->pool, sizeof(r->pool), "%s", tag);
}

static void cs_mem_emit(void)
{
    int i, j;
    for (i = 1; i < s_memN; i++) { /* usage descending, pools and categories alike */
        CsMemRow key = s_memRows[i];
        for (j = i - 1; j >= 0 && s_memRows[j].sortBytes < key.sortBytes; j--) {
            s_memRows[j + 1] = s_memRows[j];
        }
        s_memRows[j + 1] = key;
    }
    for (i = 0; i < s_memN; i++) {
        const CsMemRow *r = &s_memRows[i];
        cs_row_begin(r->kind, r->bar);
        cs_cell("%s", r->label);
        cs_cell("%s", r->used);
        cs_cell("%s", r->pct);
        cs_cell("%s", r->pool);
        cs_cell("%s", r->cap);
        cs_row_end();
    }
}

static void cs_panel_memory(void)
{
    Mp6DiagHeap h;
    Mp6DiagArenaHeap a;
    int i, n;
    const char *capEnv;
    double rssCapMb = 4096.0;

    s_memN = 0;

    n = mp6_diag_heap_count();
    if (n > MP6_DIAG_HEAP_MAX) n = MP6_DIAG_HEAP_MAX;
    for (i = 0; i < n; i++) {
        if (!mp6_diag_heap(i, &h)) continue;
        if (!h.initialized) {
            CsMemRow *r = cs_mem_row('x', h.name);
            if (r != NULL) {
                snprintf(r->pool, sizeof(r->pool), "HuMem");
                snprintf(r->cap, sizeof(r->cap), "never created");
            }
            continue;
        }
        cs_mem_pool(h.name, (double)h.used, (double)h.capacity, "HuMem");
    }

    capEnv = getenv("MP6_RSS_CAP_MB");
    if (capEnv != NULL && capEnv[0] != '\0') {
        double v = atof(capEnv);
        if (v > 0.0) rssCapMb = v;
    }
    cs_mem_pool("process RSS", (double)mp6_host_rss_bytes(), rssCapMb * 1048576.0,
                "Host/watchdog");

    n = mp6_diag_arena_count();
    for (i = 0; i < n; i++) {
        char label[56];
        if (!mp6_diag_arena(i, &a) || !a.valid) continue;
        snprintf(label, sizeof(label), "OS arena heap %d%s free", i,
                 (i == mp6_diag_arena_current()) ? " *" : "");
        cs_mem_category(label, "%.2f MB", (double)a.remaining / 1048576.0,
                        (double)a.remaining, "Host");
    }

    {
        int slots = mp6_coro_slot_count(), inUse = 0;
        double each = (double)mp6_coro_slot_size();
        for (i = 0; i < slots; i++) if (mp6_coro_slot_in_use(i)) inUse++;
        cs_mem_slot_pool("coroutine stacks", (double)inUse, (double)slots, each,
                         "Host/arena");
        cs_mem_category("coroutine peak slots", "%.0f slots",
                        (double)mp6_coro_slots_peak(),
                        (double)mp6_coro_slots_peak() * each, "Host");
    }

    cs_title("Memory [STATGROUP_Memory]");
    cs_group("Memory Counters");
    cs_cols(CS_MEM_COLS);
    cs_mem_emit();

    cs_note("Every row is sorted by the BYTES behind it, descending -- pools and");
    cs_note("categories interleaved, per the reference. Pool rows are real capacities");
    cs_note("(HuMem's HeapSizeTbl, the RSS watchdog cap, the coroutine slot table x its");
    cs_note("slot size). Category rows have no capacity to report: a bump arena knows");
    cs_note("what is LEFT, not what it started with. Nothing here is a guess -- a number");
    cs_note("that is not cheaply available has no row.");
}

/* `stat gpu` -- REGISTERED, DELIBERATELY EMPTY.
 *
 * The command exists now so the panel set is complete and so this text is
 * where someone looking for GPU time actually lands. What it must not do is
 * show a number: the reference's GPU panel is per-pass GPU time from real
 * TIMESTAMP QUERIES, and this port has none. aurora's own gpu_prof is entirely
 * #ifdef TRACY_ENABLE and TRACY is off in this build, so every candidate
 * number here would be a CPU-side wall interval wearing a GPU label -- and a
 * CPU interval labelled "GPU" is worse than an absent row, because it reads as
 * an answer. `stat scenerendering`'s GX submit row is that same interval,
 * labelled honestly.
 *
 * The column headers ARE emitted, empty. That is deliberate: the shape the
 * real panel will have (Average | Max in ms, passes descending, [TOTAL] at the
 * top and an [unaccounted] residual) is a commitment, and printing it beside
 * the reason it is empty is the difference between "not built yet" and "not
 * thought about". */
static void cs_panel_gpu(void)
{
    cs_title("GPU [STATGROUP_GPU]");
    cs_group("Counters");
    cs_cols("Pass\tAverage\tMax");
    cs_row_begin('x', 0);
    cs_cell("[TOTAL]"); cs_cell("-"); cs_cell("-");
    cs_row_end();
    cs_row_begin('x', 0);
    cs_cell("[unaccounted]"); cs_cell("-"); cs_cell("-");
    cs_row_end();
    cs_note("GPU timestamps not yet implemented -- requires Dawn timestamp-query");
    cs_note("instrumentation. Nothing on this page is a CPU-side proxy: aurora's");
    cs_note("gpu_prof is entirely #ifdef TRACY_ENABLE and TRACY is off in this");
    cs_note("build, so there is no GPU number to show and none is invented.");
    cs_note("");
    cs_note("Next increment, in this order: request the adapter's timestamp-query");
    cs_note("feature and REPORT when it is absent (some Android drivers do not");
    cs_note("expose it) rather than silently showing zeros; bracket aurora's real");
    cs_note("render-graph passes -- scene stream submit, EFB-copy resolves, the");
    cs_note("SSAA/downsample resolve, the RmlUi overlay pass (this console), the");
    cs_note("present blit -- with a query set resolved once per frame; then rows");
    cs_note("descending with [TOTAL] and [unaccounted] = total - sum(passes).");
    cs_note("Resolution must not run while this panel is closed.");
    cs_note("");
    cs_note("Meanwhile: `stat scenerendering`'s GX submit row is the CPU-side wall");
    cs_note("cost of the submit, which is the honest thing this build can measure.");
}

/* `stat game` -- the tick broken down by RETAIL PROCESS.
 *
 * The reference's Game group is the game thread's own stage breakdown. This
 * port's game thread IS the HuPrc process list, so the rows are per-process
 * execution time, timed at the one dispatcher boundary a process runs across
 * (platform/os/process_native.c) and named by the process's entry symbol.
 *
 * v1 IS FLAT AND SAYS SO. Inclusive would have to include child-process time
 * via the HuPrcChildCreate parentage, and the dispatcher does not nest -- a
 * child is a sibling in processtop that HuPrcCall reaches on its own pass, so
 * a parent's slice genuinely does NOT contain its children's. Attributing them
 * upward needs the parentage walked per slice, which is a change to the
 * scheduler's hot path for a number nobody has asked a question of yet. So
 * Exclusive == Inclusive here, the header says it in the panel rather than in
 * a comment nobody reading the screen can see, and the parentage roll-up is
 * the declared next increment. */
static void cs_panel_game(void)
{
    CsCycleRow rows[CS_PROC_MAX];
    int n = 0, i;

    cs_title("Game [STATGROUP_Game]");
    cs_group("Cycle counters (flat) -- per HuPrc process, Exclusive == Inclusive in v1");

    for (i = 0; i < s_procN && n < CS_PROC_MAX; i++) {
        double avgMs;
        if (s_proc[i].lastCalls == 0) continue; /* not dispatched last window */
        cs_proc_name(&s_proc[i]);
        avgMs = (double)s_proc[i].lastSumNs / (double)s_proc[i].lastCalls / 1e6;
        rows[n].name = s_proc[i].name;
        rows[n].calls = s_proc[i].lastCalls;
        rows[n].inclAvg = rows[n].exclAvg = avgMs;
        rows[n].inclMax = rows[n].exclMax = (double)s_proc[i].lastMaxNs / 1e6;
        n++;
    }
    if (n == 0) {
        cs_cols(CS_CYCLE_COLS);
        cs_note("No process slice has been timed yet -- the sampler arms with this");
        cs_note("console (or the stat unit HUD) and reports the previous COMPLETE window.");
    } else {
        cs_cycle_emit(rows, n, CS_PROC_ROWS);
    }
    if (s_procDropped > 0) {
        cs_note("%ld slice(s) dropped: more than %d distinct process entry points.",
                s_procDropped, CS_PROC_MAX);
    }

    /* The reference's Counters block is Average | Max | Min over the rolling
     * window -- the same columns and the same window semantics `stat
     * scenerendering` uses, fed by the same CsAcc accumulators. It used to be
     * a bare "Value | Of" snapshot under the same "Counters" heading, which
     * claimed a window it did not have: a single live read cannot show that
     * the process list peaked at 34 while averaging 21, and peaks are the only
     * reason to watch this page during play. */
    cs_group("Counters");
    cs_cols(CS_COUNTER_COLS);
    cs_acc_row(&s_gLast[CS_G_PROCESSES], "HuPrc processes alive", "%.0f");
    cs_acc_row(&s_gLast[CS_G_FIBERS], "scheduler coroutine slots", "%.0f");
    cs_acc_row(&s_gLast[CS_G_OM_RAN], "OM objects run (per walk)", "%.0f");
    cs_acc_row(&s_gLast[CS_G_OM_GATED], "OM objects gated", "%.0f");
    cs_acc_row(&s_gLast[CS_G_OM_NULL], "OM objects with a NULL objFunc", "%.0f");
    cs_acc_row(&s_gLast[CS_G_OM_DEAD], "OM objects deleted but listed", "%.0f");
    cs_acc_row(&s_gLast[CS_G_EVENTS], "events posted / tick", "%.2f");
    cs_acc_row(&s_gLast[CS_G_VOICES], "SFX voices active", "%.1f");
    cs_acc_row(&s_gLast[CS_G_CHANS], "stream channels active", "%.1f");

    /* Quantities with no window behind them, kept OUT of the block above
     * rather than padded into it with a repeated value: a ceiling and a
     * process-lifetime total are not an Average|Max|Min of anything. */
    cs_group("Cumulative");
    cs_cols("Counter\tValue");
    cs_row_begin('.', 0);
    cs_cell("scheduler coroutine slot ceiling");
    cs_cell("%d", mp6_diag_process_fiber_max()); cs_row_end();
    cs_row_begin('.', 0);
    cs_cell("distinct process entry points");
    cs_cell("%d of %d", s_procN, CS_PROC_MAX); cs_row_end();
    cs_row_begin('.', 0);
    cs_cell("omMain walks"); cs_cell("%ld", mp6_diag_om_passes()); cs_row_end();
    cs_row_begin('.', 0);
    cs_cell("events posted (all keys, cumulative)");
    cs_cell("%lu over %d key(s)", cs_event_total(), mp6_diag_event_slot_count());
    cs_row_end();

    cs_window_note();
    cs_note("Rows are the process ENTRY function, symbolized the way the OM census");
    cs_note("names objFuncs; an unresolved entry shows as hex, never as a guess.");
}

static void cs_panel_audio(void)
{
    static const char *const kFade[4] = { "none", "->pause", "->stop", "->play" };
    Mp6DiagAudio a;
    int i;

    mp6_diag_audio_snapshot(&a); /* copies under the mixer lock; formats here */

    cs_title("Audio [STATGROUP_Audio]");
    /* "Mixer state", not "Counters": the reference's Counters block means
     * Average | Max | Min over the rolling window, and every row below is a
     * live read taken at build time. Calling them Counters would claim a
     * window they do not have -- the same false claim `stat game` carried
     * until this increment. */
    cs_group("Mixer state");
    cs_cols("Counter\tValue\tOf\tNote");
    cs_row_begin('.', a.voiceCount ? (a.voicesActive * 100) / a.voiceCount : 0);
    cs_cell("SFX voices active"); cs_cell("%d", a.voicesActive);
    cs_cell("%d", a.voiceCount); cs_cell("mixer slots"); cs_row_end();
    cs_row_begin('.', a.chanCount ? (a.chansActive * 100) / a.chanCount : 0);
    cs_cell("stream channels active"); cs_cell("%d", a.chansActive);
    cs_cell("%d", a.chanCount); cs_cell("%d by .pdt", a.chanConfigured);
    cs_row_end();
    cs_row_begin('.', 0);
    cs_cell("master volume"); cs_cell("%d", a.masterVol); cs_cell("-"); cs_cell("-");
    cs_row_end();
    cs_row_begin('.', 0);
    cs_cell("SE volume"); cs_cell("%d", a.seMasterVol); cs_cell("-"); cs_cell("-");
    cs_row_end();
    cs_row_begin('.', 0);
    cs_cell("key-group releases"); cs_cell("%lu", a.keygroupEvents);
    cs_cell("-"); cs_cell("%lu voice(s) freed", a.keygroupVoices); cs_row_end();

    cs_group("SFX voices");
    cs_cols("Slot\tseNo\tseId\tLoop\tKeyGrp\tWraps\tAge");
    for (i = 0; i < a.voiceCount; i++) {
        unsigned long long age;
        if (!a.voices[i].active) continue;
        age = ((unsigned long long)mp6_tick_count > a.voices[i].startTick)
                  ? (unsigned long long)mp6_tick_count - a.voices[i].startTick : 0ull;
        cs_row_begin('.', 0);
        cs_cell("voice %d", i);
        cs_cell("%d", a.voices[i].no);
        cs_cell("%d", a.voices[i].seId);
        cs_cell("%s", a.voices[i].loop ? "yes" : "no");
        cs_cell("%d", a.voices[i].keyGroup);
        cs_cell("%u", a.voices[i].wraps);
        cs_cell("%llu t", age);
        cs_row_end();
    }
    if (a.voicesActive == 0) cs_note("(no voice is sounding)");

    cs_group("BGM / stream channels");
    cs_cols("Channel\tStream\tPaused\tLoop\tVol\tFade");
    for (i = 0; i < a.chanCount; i++) {
        int f = (a.chans[i].fadeAction >= 0 && a.chans[i].fadeAction < 4) ? a.chans[i].fadeAction : 0;
        if (!a.chans[i].active) continue;
        cs_row_begin('.', 0);
        cs_cell("chan %d", i);
        cs_cell("%d", a.chans[i].streamId);
        cs_cell("%s", a.chans[i].paused ? "yes" : "no");
        cs_cell("%s", a.chans[i].loop ? "yes" : "no");
        cs_cell("%d", a.chans[i].vol);
        cs_cell("%s x%.2f", kFade[f], (double)a.chans[i].fadeMul);
        cs_row_end();
    }
    if (a.chansActive == 0) cs_note("(no stream channel is playing)");
}

#define CS_SEAM_TOP 12

static void cs_panel_board(void)
{
    int order[CS_SEAM_TOP];
    unsigned long counts[CS_SEAM_TOP];
    int orderN = 0;
    int i, n;

    cs_title("Board [STATGROUP_Board]");
    cs_group("Board state"); /* live reads, not a window -- see cs_panel_audio */
    cs_cols("Counter\tValue");
    cs_row_begin('.', 0); cs_cell("W01 board no"); cs_cell("%d", mp6_diag_board_no()); cs_row_end();
    cs_row_begin('.', 0); cs_cell("board ticks"); cs_cell("%u", mp6_diag_board_ticks()); cs_row_end();
    cs_row_begin('.', 0); cs_cell("board draws"); cs_cell("%u", mp6_diag_board_draws()); cs_row_end();

    /* Seams ranked by call count -- the whole reason the registry exists: a
     * seam firing every frame and one that fired twice at board open look
     * identical in the decade-throttled log until the third decade lands.
     * Straight insertion into a fixed top-N list; the table is bounded and
     * this runs at the panel refresh rate, not per frame. */
    n = mp6_diag_seam_count();
    for (i = 0; i < n; i++) {
        unsigned long c = 0;
        int at, j;
        mp6_diag_seam(i, NULL, NULL, &c);
        for (at = 0; at < orderN; at++) {
            if (c > counts[at]) break;
        }
        if (at >= CS_SEAM_TOP) continue;
        for (j = (orderN < CS_SEAM_TOP ? orderN : CS_SEAM_TOP - 1); j > at; j--) {
            order[j] = order[j - 1];
            counts[j] = counts[j - 1];
        }
        order[at] = i;
        counts[at] = c;
        if (orderN < CS_SEAM_TOP) orderN++;
    }
    cs_group("Placeholder seams -- %d fired, top %d by call count", n, orderN);
    cs_cols("Seam\tResult\tCalls");
    for (i = 0; i < orderN; i++) {
        const char *sym = "?", *res = "?";
        unsigned long c = 0;
        mp6_diag_seam(order[i], &sym, &res, &c);
        cs_row_begin('.', (orderN > 0 && counts[0] > 0) ? (int)((c * 100ul) / counts[0]) : 0);
        cs_cell("%s", sym);
        cs_cell("%s", res);
        cs_cell("%lu", c);
        cs_row_end();
    }
    cs_more(n - orderN);

    n = mp6_diag_event_tail_count();
    cs_group("Event tail -- newest first (%d retained, %d distinct keys)",
             n, mp6_diag_event_slot_count());
    cs_cols("Event");
    for (i = 0; i < n && i < 12; i++) {
        cs_row_begin('.', 0);
        cs_cell("%s", mp6_diag_event_tail(i));
        cs_row_end();
    }
    cs_more(n - 12);
}

/* =======================================================================
 * 5. Panels that need the renderer.
 * ======================================================================= */

#ifdef MP6_HEADLESS_BUILD

static void cs_panel_unavailable(const char *name)
{
    cs_title("%s -- unavailable headless", name);
    cs_note("There is no renderer, no present cadence and no GX submission to");
    cs_note("measure in this build. The memory, audio and board panels work here.");
}

static void cs_panel_fps(void)   { cs_panel_unavailable("FPS"); }
static void cs_panel_unit(void)  { cs_panel_unavailable("Unit"); }
static void cs_panel_scene(void) { cs_panel_unavailable("Scene Rendering"); }
static void cs_rates_refresh(void) { }
static void cs_counters_roll(void) { }
static void cs_frame_counters_sample(void) { }

#else

static const char *const kPrimNames[8] = {
    "quads", "quads2", "tris", "tristrip", "trifan", "lines", "linestrip", "points"
};

/* aurora's own stats block (external_refs/repos/aurora/include/aurora/gfx.h).
 * Declared locally rather than by including that header -- the same C-linkage
 * seam platform/gx/frame_dump.c uses for aurora-patches/0025, because this TU
 * compiles with the decomp's dolphin headers, not aurora's. Nine uint32_t in
 * this exact order; the size assert fails the build if that stops being true.
 * Both symbols have been exported and linked all along; nothing in the port
 * referenced either of them before this file. */
typedef struct {
    unsigned queuedPipelines;
    unsigned createdPipelines;
    unsigned drawCallCount;
    unsigned mergedDrawCallCount;
    unsigned lastVertSize;
    unsigned lastUniformSize;
    unsigned lastIndexSize;
    unsigned lastStorageSize;
    unsigned lastTextureUploadSize;
} CsAuroraStats;
typedef char cs_aurora_stats_layout_check[(sizeof(CsAuroraStats) == 36) ? 1 : -1];

extern const CsAuroraStats *aurora_get_stats(void);
extern float aurora_get_fps(void);

/* aurora_bridge.c's own present counters, tick configuration and the live
 * window/render size the HUD's RenderRes row reports. */
extern void mp6_present_counters_get(long *begins, long *ends);
extern void mp6_tick_config_get(double *hz, long long *periodNs);
extern void mp6_render_res_get(int *winW, int *winH, int *renderW, int *renderH);

/* The output the window is on, and the present sync pacing it -- the two facts
 * that turn "presents/s" from a bare number into an explained one. Included
 * INSIDE this file's non-headless arm on purpose: the header is
 * declaration-only and would compile fine either way, but placing it here
 * makes it structurally impossible for a future edit to reference the seam
 * from the headless `cs_panel_unavailable` half above, where the
 * implementation (platform/gx/aurora_bridge.c, PLATFORM_AURORA_ONLY) does not
 * link. */
#include "mp6_display.h"

/* --- percentiles ------------------------------------------------------
 * "1% low" is the frame time at the 99th percentile of the retained present
 * intervals -- the worst 1% of the frames the user actually saw, which is
 * where a stutter appears and where an average never does. */

static int cs_cmp_ll(const void *a, const void *b)
{
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

typedef struct {
    int    n;
    int    replays;   /* how many of the retained presents were interpolated */
    double avgMs, lowOnePctMs, lowTenthPctMs, maxMs, minMs, spanSec;
} CsFrameTimes;

static void cs_frame_times(CsFrameTimes *out)
{
    static long long scratch[CS_PRESENT_RING];
    int i, n = 0, idx;
    long long sum = 0;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < s_presentCount; i++) {
        idx = (s_presentHead - 1 - i + CS_PRESENT_RING * 2) % CS_PRESENT_RING;
        if (s_presentReplay[idx]) out->replays++;
    }
    if (s_presentCount < 2) return;
    /* Oldest -> newest, so consecutive differences are real intervals. */
    for (i = s_presentCount - 1; i >= 1; i--) {
        int cur = (s_presentHead - 1 - (i - 1) + CS_PRESENT_RING * 2) % CS_PRESENT_RING;
        int prev = (s_presentHead - 1 - i + CS_PRESENT_RING * 2) % CS_PRESENT_RING;
        long long d = s_presentNs[cur] - s_presentNs[prev];
        if (d <= 0) continue; /* a restore or a clock step is never a frame */
        scratch[n++] = d;
        sum += d;
    }
    if (n == 0) return;
    out->n = n;
    out->avgMs = (double)sum / (double)n / 1e6;
    out->spanSec = (double)sum / 1e9;
    qsort(scratch, (size_t)n, sizeof(scratch[0]), cs_cmp_ll);
    out->maxMs = (double)scratch[n - 1] / 1e6;
    out->minMs = (double)scratch[0] / 1e6;
    out->lowOnePctMs = (double)scratch[((n * 99) / 100 < n) ? (n * 99) / 100 : n - 1] / 1e6;
    out->lowTenthPctMs = (double)scratch[((n * 999) / 1000 < n) ? (n * 999) / 1000 : n - 1] / 1e6;
}

typedef struct { double avgMs, maxMs; } CsBucket;

static void cs_bucket(size_t offset, CsBucket *out)
{
    int i;
    long long sum = 0, mx = 0;
    out->avgMs = 0.0;
    out->maxMs = 0.0;
    if (s_phaseCount == 0) return;
    for (i = 0; i < s_phaseCount; i++) {
        int idx = (s_phaseHead - 1 - i + CS_PHASE_RING * 2) % CS_PHASE_RING;
        long long v = *(const long long *)((const char *)&s_phase[idx] + offset);
        if (v < 0) v = 0;
        sum += v;
        if (v > mx) mx = v;
    }
    out->avgMs = (double)sum / (double)s_phaseCount / 1e6;
    out->maxMs = (double)mx / 1e6;
}

/* The same reduction over the per-tick DIFFERENCE of two buckets.
 *
 * This exists because max(a) - max(b) is not max(a - b). The overlay bracket
 * sits INSIDE the endframe bracket on every tick, so "GX submit" is the
 * per-tick remainder endFrame-overlay; reducing the two buckets separately and
 * subtracting the averages happens to be correct for the MEAN (it is linear)
 * and is simply wrong for the MAX -- it reports the worst endframe tick, which
 * may be worst precisely BECAUSE the console repainted on it. That made the
 * panel's own footer ("the overlay row is not inside GX submit") false for the
 * one column a hitch shows up in. Subtract per sample, then reduce. */
static void cs_bucket_diff(size_t offset, size_t subOffset, CsBucket *out)
{
    int i;
    long long sum = 0, mx = 0;
    out->avgMs = 0.0;
    out->maxMs = 0.0;
    if (s_phaseCount == 0) return;
    for (i = 0; i < s_phaseCount; i++) {
        int idx = (s_phaseHead - 1 - i + CS_PHASE_RING * 2) % CS_PHASE_RING;
        const char *base = (const char *)&s_phase[idx];
        long long v = *(const long long *)(base + offset);
        long long sub = *(const long long *)(base + subOffset);
        v -= sub;
        if (v < 0) v = 0; /* a tick that sampled one bracket and not the other */
        sum += v;
        if (v > mx) mx = v;
    }
    out->avgMs = (double)sum / (double)s_phaseCount / 1e6;
    out->maxMs = (double)mx / 1e6;
}

#define CS_BUCKET(field, out) cs_bucket(offsetof(CsPhase, field), (out))
#define CS_BUCKET_DIFF(field, sub, out) \
    cs_bucket_diff(offsetof(CsPhase, field), offsetof(CsPhase, sub), (out))

/* --- the rolling window's counter set ---------------------------------
 * One accumulator per Counters row. `cur` collects; cs_window_roll() publishes
 * it into `last` every CS_WINDOW_TICKS and zeroes it, so every panel reads the
 * previous COMPLETE window and a Min is never the first sample of a window
 * that is still filling. */
enum {
    CS_C_TICK_RATE = 0,
    CS_C_PRESENT_RATE,
    CS_C_REAL_RATE,
    CS_C_REPLAY_RATE,
    CS_C_AURORA_FPS,
    CS_C_DRAWS,
    CS_C_AURORA_DRAWS,
    CS_C_AURORA_MERGED,
    CS_C_VERTS,
    CS_C_DL_CALLS,
    CS_C_DL_BYTES,
    CS_C_DL_REC_BYTES,
    CS_C_EFB_COPIES,
    CS_C_TEX_BINDS,
    CS_C_TEX_UPLOAD,
    CS_C_PIPE_QUEUED,
    CS_C_PRIM0,
    CS_C_COUNT = CS_C_PRIM0 + 8
};

static CsAcc s_ctrCur[CS_C_COUNT];
static CsAcc s_ctrLast[CS_C_COUNT];

static void cs_counters_roll(void)
{
    memcpy(s_ctrLast, s_ctrCur, sizeof(s_ctrLast));
    memset(s_ctrCur, 0, sizeof(s_ctrCur));
}

static void cs_frame_counters_sample(void)
{
    const CsAuroraStats *as = aurora_get_stats();
    int i;
    cs_acc_add(&s_ctrCur[CS_C_DRAWS], (double)s_drawIndexLast);
    cs_acc_add(&s_ctrCur[CS_C_VERTS], (double)s_vertsLast);
    cs_acc_add(&s_ctrCur[CS_C_DL_CALLS], (double)s_dlCallsLast);
    cs_acc_add(&s_ctrCur[CS_C_DL_BYTES], (double)s_dlBytesLast);
    cs_acc_add(&s_ctrCur[CS_C_DL_REC_BYTES], (double)s_dlRecBytesLast);
    cs_acc_add(&s_ctrCur[CS_C_EFB_COPIES], (double)s_efbCopiesLast);
    cs_acc_add(&s_ctrCur[CS_C_TEX_BINDS], (double)s_texBindsLast);
    for (i = 0; i < 8; i++) cs_acc_add(&s_ctrCur[CS_C_PRIM0 + i], (double)s_primLast[i]);
    if (as != NULL) {
        cs_acc_add(&s_ctrCur[CS_C_AURORA_DRAWS], (double)as->drawCallCount);
        cs_acc_add(&s_ctrCur[CS_C_AURORA_MERGED], (double)as->mergedDrawCallCount);
        cs_acc_add(&s_ctrCur[CS_C_TEX_UPLOAD], (double)as->lastTextureUploadSize);
        cs_acc_add(&s_ctrCur[CS_C_PIPE_QUEUED], (double)as->queuedPipelines);
    }
}

static void cs_rates_refresh(void)
{
    long long now = (long long)mp6_host_monotonic_ns();
    double elapsed;
    if (s_rateWindowNs == 0) {
        s_rateWindowNs = now;
        s_rateWindowTick = mp6_tick_count;
        s_rateWindowPresents = s_presentsTotal;
        s_rateWindowReplays = s_replaysTotal;
        return;
    }
    elapsed = (double)(now - s_rateWindowNs) / 1e9;
    if (elapsed < 0.45) return; /* twice a second, the FPS badge's own cadence */
    s_rateTicksPerSec = (double)(mp6_tick_count - s_rateWindowTick) / elapsed;
    s_ratePresentsPerSec = (double)(s_presentsTotal - s_rateWindowPresents) / elapsed;
    s_rateReplaysPerSec = (double)(s_replaysTotal - s_rateWindowReplays) / elapsed;
    s_rateWindowNs = now;
    s_rateWindowTick = mp6_tick_count;
    s_rateWindowPresents = s_presentsTotal;
    s_rateWindowReplays = s_replaysTotal;
    cs_acc_add(&s_ctrCur[CS_C_TICK_RATE], s_rateTicksPerSec);
    cs_acc_add(&s_ctrCur[CS_C_PRESENT_RATE], s_ratePresentsPerSec);
    cs_acc_add(&s_ctrCur[CS_C_REAL_RATE], s_ratePresentsPerSec - s_rateReplaysPerSec);
    cs_acc_add(&s_ctrCur[CS_C_REPLAY_RATE], s_rateReplaysPerSec);
    cs_acc_add(&s_ctrCur[CS_C_AURORA_FPS], (double)aurora_get_fps());
}

static void cs_panel_fps(void)
{
    Mp6FiStats fi;
    CsFrameTimes ft;
    double hz = 0.0;
    long long periodNs = 0;
    long begins = 0, ends = 0;

    mp6_fi_stats_get(&fi);
    cs_frame_times(&ft);
    mp6_tick_config_get(&hz, &periodNs);
    mp6_present_counters_get(&begins, &ends);

    cs_title("FPS [STATGROUP_FPS]");

    cs_group("Counters");
    cs_cols(CS_COUNTER_COLS);
    cs_acc_row(&s_ctrLast[CS_C_TICK_RATE], "ticks/s (measured)", "%.2f");
    cs_acc_row(&s_ctrLast[CS_C_PRESENT_RATE], "presents/s (total)", "%.2f");
    cs_acc_row(&s_ctrLast[CS_C_REAL_RATE], "presents/s (real)", "%.2f");
    cs_acc_row(&s_ctrLast[CS_C_REPLAY_RATE], "presents/s (interpolated)", "%.2f");
    cs_acc_row(&s_ctrLast[CS_C_AURORA_FPS], "aurora present-window FPS", "%.2f");
    cs_row_begin('.', 0);
    cs_cell("frame time ms (%d intervals)", ft.n);
    cs_cell("%.3f", ft.avgMs); cs_cell("%.3f", ft.maxMs); cs_cell("%.3f", ft.minMs);
    cs_row_end();

    {
        /* THE PRESENT CAP, spelled out. Every counter above measures presents,
         * and with present sync on the presentable ceiling is the current
         * OUTPUT's refresh rate -- so a panel that reports presents/s without
         * reporting that ceiling is reporting half the arithmetic. It sits
         * directly under the Counters block for that reason: the row that
         * explains "75.00 presents/s" should be the next thing read.
         *
         * A ONE-COLUMN SECTION, and that is the fix for a real defect rather
         * than a style choice. In a Counter|Value table the value cell is
         * `flex: 0 0 96dp` (res/rml/overlay.rcss statpanel pcell) because
         * fixed-width right-aligned cells ARE the column alignment -- which is
         * right for numbers and wrong for prose. Measured on the first windowed
         * capture: the display name rendered as "LG ULTRAGE..." and the present
         * mode as "FIFO (request...", both ellipsised inside 96dp, so neither
         * the output nor the annotation could actually be read. These four facts
         * are heterogeneous prose (a vendor name, a rate, a mode, a count), not
         * a column to compare down, so they get the shape this panel already
         * uses for prose rows -- a single-column section whose one cell is the
         * label cell (`flex: 1 1 auto`) and spans the panel's full width, the
         * same shape `stat diag`'s "Event tail" section uses. Still nowrap +
         * ellipsis, so a narrow window truncates as everything else does; at
         * 620dp the longest line here is roughly two thirds of the width.
         *
         * Rows are OMITTED entirely when the query fails, never filled with
         * zeroes -- the same discipline the RenderRes row uses: a made-up
         * number in a diagnostic panel is worse than a missing one. */
        Mp6DisplayInfo di;
        if (mp6_display_info_get(&di)) {
            cs_group("Present cap -- the output the window is on, and what paces it");
            cs_cols("Output / present path");
            cs_row_begin('.', 0);
            cs_cell("output: %s  %dx%d @ %.3f Hz%s", di.name, di.w, di.h,
                    (double)di.refreshMilliHz / 1000.0,
                    di.isHighestRefresh ? "" : "  (NOT the highest-refresh output)");
            cs_row_end();
            cs_row_begin('.', 0);
            cs_cell("present mode: %s%s", di.presentMode,
                    di.presentModeExact
                        ? "  (exact -- aurora's own startup log)"
                        : "  (requested class -- exact mode not readable at runtime)");
            cs_row_end();
            cs_row_begin('.', 0);
            /* The measured total-present rate against the cap, both on one
             * line. Deliberately the same rolling-window average the
             * "presents/s (total)" row above prints -- not a second,
             * differently-derived count -- so the two can never disagree. */
            cs_cell("present cap: %.2f of %d/s%s",
                    cs_acc_avg(&s_ctrLast[CS_C_PRESENT_RATE]), di.refreshHz,
                    di.vsyncOn ? "" : "  (present sync OFF -- no cap)");
            cs_row_end();
            cs_row_begin('.', 0);
            cs_cell("cross-output reconfigures: %ld", mp6_display_follow_count());
            cs_row_end();
        }
    }

    cs_group("Frame-time percentiles -- %d retained presents over %.2f s, %d replayed",
             ft.n, ft.spanSec, ft.replays);
    cs_cols("Percentile\tms");
    cs_row_begin('.', 100); cs_cell("0.1%% low"); cs_cell("%.3f", ft.lowTenthPctMs); cs_row_end();
    cs_row_begin('.', 70);  cs_cell("1%% low");   cs_cell("%.3f", ft.lowOnePctMs);   cs_row_end();
    cs_row_begin('.', 30);  cs_cell("average");   cs_cell("%.3f", ft.avgMs);         cs_row_end();

    cs_group("Cumulative");
    cs_cols("Counter\tValue");
    cs_row_begin('.', 0); cs_cell("configured tick rate");
    cs_cell("%.2f Hz = %.3f ms", hz, (double)periodNs / 1e6); cs_row_end();
    cs_row_begin('.', 0); cs_cell("ticks"); cs_cell("%ld", mp6_tick_count); cs_row_end();
    cs_row_begin('.', 0); cs_cell("begin-frames / end-frames");
    cs_cell("%ld / %ld", begins, ends); cs_row_end();
    cs_row_begin('.', 0); cs_cell("FI replays / snaps / skipped windows");
    cs_cell("%ld / %ld / %ld", fi.statReplays, fi.statSnaps, fi.statSkipped); cs_row_end();
    {
        /* Why the idle window did or did not present, straight out of
         * frame_interp.c's own permanent census -- never a second count. */
        int i, top = -1;
        long topN = 0, calls = 0;
        for (i = 0; i < fi.declCount; i++) {
            calls += fi.decl[i];
            if (fi.decl[i] > topN) { topN = fi.decl[i]; top = i; }
        }
        cs_row_begin('.', 0);
        cs_cell("idle-window top verdict");
        if (calls > 0 && top >= 0) {
            cs_cell("%s (%ld of %ld, %.0f%%)", mp6_fi_decl_name(top), topN, calls,
                    100.0 * (double)topN / (double)calls);
        } else {
            cs_cell("never offered (Unlocked FPS off, or no tick throttle)");
        }
        cs_row_end();
    }

    cs_window_note();
    cs_note("The launcher's FPS badge counts PRESENTS, not ticks -- with Unlocked FPS");
    cs_note("on it correctly reads above the tick rate. `stat unit` is the corner HUD.");
    cs_note("With present sync ON, no presents/s row can exceed the Present cap group's");
    cs_note("output rate: that refresh rate is a hard cap, not a symptom. `vsync 0` lifts it.");
}

/* --- the `stat unit` corner HUD ---------------------------------------
 * NOT a table: the reference is a compact right-aligned Label: value block in
 * a screen corner, colour-coded against the frame budget, alive whether or not
 * the bar is open. It is one overlay latch among the others now, but it keeps
 * its own SHAPE -- platform/gx/ui/overlay.cpp gives it the corner the FPS
 * badge it replaces used to own, while every other latched panel gets a
 * stacked table. The 1%/0.1% lows deliberately stay in `stat fps`: a corner
 * HUD that needs reading twice is not a HUD.
 *
 * Row kinds carry the budget verdict: 'g' inside the per-frame budget, 'r'
 * over it, 'x' informational (no budget applies). */
static void cs_hud_ms(const char *label, double ms, double budgetMs)
{
    cs_row_begin((budgetMs > 0.0) ? ((ms <= budgetMs) ? 'g' : 'r') : 'x', 0);
    cs_cell("%s", label);
    cs_cell("%.2f ms", ms);
    cs_row_end();
}

static void cs_hud_text(const char *label, const char *fmt, ...)
{
    va_list ap;
    cs_row_begin('x', 0);
    cs_cell("%s", label);
    cs_put("\t");
    va_start(ap, fmt);
    cs_vput(fmt, ap);
    va_end(ap);
    cs_row_end();
}

static void cs_panel_unit(void)
{
    CsBucket game, submit, overlay, seal;
    CsFrameTimes ft;
    Mp6FiStats fi;
    double hz = 0.0, budgetMs;
    long long periodNs = 0;
    int winW = 0, winH = 0, renderW = 0, renderH = 0;
    double heapUsed = 0.0;
    int i, n;

    CS_BUCKET(game, &game);
    CS_BUCKET_DIFF(endFrame, overlay, &submit); /* see cs_bucket_diff() */
    CS_BUCKET(overlay, &overlay);
    CS_BUCKET(seal, &seal);
    cs_frame_times(&ft);
    mp6_fi_stats_get(&fi);
    mp6_tick_config_get(&hz, &periodNs);
    mp6_render_res_get(&winW, &winH, &renderW, &renderH);
    budgetMs = (periodNs > 0) ? (double)periodNs / 1e6 : 16.667;

    n = mp6_diag_heap_count();
    for (i = 0; i < n; i++) {
        Mp6DiagHeap h;
        if (mp6_diag_heap(i, &h) && h.initialized) heapUsed += (double)h.used;
    }

    cs_hud_ms("Frame", ft.avgMs, budgetMs);
    cs_hud_ms("Game", game.avgMs, budgetMs);
    cs_hud_ms("Draw", submit.avgMs, budgetMs);
    cs_hud_ms("FI", fi.costSamples
                        ? (double)fi.costBuildSumNs / (double)fi.costSamples / 1e6
                        : seal.avgMs,
              budgetMs);
    /* NEVER a proxy dressed as GPU time: aurora's gpu_prof is entirely #ifdef
     * TRACY_ENABLE and TRACY is off here, so there is no GPU number to show.
     * `stat gpu` says the same thing at length. */
    cs_hud_text("GPU", "n/a (see stat gpu)");
    cs_hud_text("Mem", "%.0f MB  RSS %.0f MB", heapUsed / 1048576.0,
                (double)mp6_host_rss_bytes() / 1048576.0);
    if (winW > 0 && renderW > 0) {
        cs_hud_text("RenderRes", "%.1f%% (%dx%d)",
                    100.0 * (double)renderW / (double)winW, renderW, renderH);
    }
    cs_hud_text("Draws", "%u", s_drawIndexLast);
    cs_hud_text("Prims", "%u", s_vertsLast);
    cs_row_begin('x', 0);
    cs_cell("Overlay");
    cs_cell("%.2f ms excl", overlay.avgMs);
    cs_row_end();
}

static void cs_panel_scene(void)
{
    const CsAuroraStats *as = aurora_get_stats();
    CsBucket game, submit, overlay, seal, viPost;
    CsCycleRow rows[8];
    Mp6FiStats fi;
    int nrows = 0, i;
    long ticks = s_phaseCount;

    mp6_fi_stats_get(&fi);
    CS_BUCKET(game, &game);
    CS_BUCKET_DIFF(endFrame, overlay, &submit);
    CS_BUCKET(overlay, &overlay);
    CS_BUCKET(seal, &seal);
    CS_BUCKET(viPost, &viPost);

    cs_title("Scene Rendering [STATGROUP_SceneRendering]");

    cs_group("Cycle counters (flat) -- %d ticks retained", s_phaseCount);
    /* GX submit is the ONLY row with a real inclusive/exclusive split in this
     * port: the overlay bracket nests inside the endframe bracket, so
     * inclusive is the whole present block and exclusive is that minus the
     * console's own compositing. Every other bucket is a leaf, and its two
     * columns are equal because they genuinely are, not as a placeholder. */
    rows[nrows].name = "GX submit (aurora end_frame)";
    rows[nrows].calls = ticks;
    rows[nrows].inclAvg = submit.avgMs + overlay.avgMs;
    rows[nrows].inclMax = submit.maxMs + overlay.maxMs;
    rows[nrows].exclAvg = submit.avgMs;
    rows[nrows].exclMax = submit.maxMs;
    nrows++;
    rows[nrows].name = "game tick (logic + GX emit)";
    rows[nrows].calls = ticks;
    rows[nrows].inclAvg = rows[nrows].exclAvg = game.avgMs;
    rows[nrows].inclMax = rows[nrows].exclMax = game.maxMs;
    nrows++;
    rows[nrows].name = "console overlay [excluded]";
    rows[nrows].calls = ticks;
    rows[nrows].inclAvg = rows[nrows].exclAvg = overlay.avgMs;
    rows[nrows].inclMax = rows[nrows].exclMax = overlay.maxMs;
    nrows++;
    rows[nrows].name = "FI seal (stream walk + pair)";
    rows[nrows].calls = ticks;
    rows[nrows].inclAvg = rows[nrows].exclAvg = seal.avgMs;
    rows[nrows].inclMax = rows[nrows].exclMax = seal.maxMs;
    nrows++;
    rows[nrows].name = "vi-post (input, widescreen)";
    rows[nrows].calls = ticks;
    rows[nrows].inclAvg = rows[nrows].exclAvg = viPost.avgMs;
    rows[nrows].inclMax = rows[nrows].exclMax = viPost.maxMs;
    nrows++;
    rows[nrows].name = "FI replay build (port-side)";
    rows[nrows].calls = fi.costSamples;
    rows[nrows].inclAvg = rows[nrows].exclAvg = fi.costSamples
        ? (double)fi.costBuildSumNs / (double)fi.costSamples / 1e6 : 0.0;
    rows[nrows].inclMax = rows[nrows].exclMax = (double)fi.costBuildMaxNs / 1e6;
    nrows++;
    rows[nrows].name = "FI replay present (aurora)";
    rows[nrows].calls = fi.costSamples;
    rows[nrows].inclAvg = rows[nrows].exclAvg = fi.costSamples
        ? (double)fi.costPresentSumNs / (double)fi.costSamples / 1e6 : 0.0;
    rows[nrows].inclMax = rows[nrows].exclMax = (double)fi.costPresentMaxNs / 1e6;
    nrows++;
    cs_cycle_emit(rows, nrows, nrows);

    cs_group("Counters");
    cs_cols(CS_COUNTER_COLS);
    cs_acc_row(&s_ctrLast[CS_C_DRAWS], "mesh draw calls (port index)", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_AURORA_DRAWS], "aurora draw calls", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_AURORA_MERGED], "aurora merged draws", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_VERTS], "vertices / frame", "%.0f");
    for (i = 0; i < 8; i++) {
        char label[48];
        if (s_ctrLast[CS_C_PRIM0 + i].n == 0 && s_primLast[i] == 0) continue;
        snprintf(label, sizeof(label), "  prim %s", kPrimNames[i]);
        cs_acc_row(&s_ctrLast[CS_C_PRIM0 + i], label, "%.0f");
    }
    cs_acc_row(&s_ctrLast[CS_C_EFB_COPIES], "EFB copies / frame", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_TEX_BINDS], "texture binds / frame", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_DL_CALLS], "display-list calls", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_DL_BYTES], "display-list bytes replayed", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_DL_REC_BYTES], "display-list bytes recorded", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_TEX_UPLOAD], "texture upload bytes", "%.0f");
    cs_acc_row(&s_ctrLast[CS_C_PIPE_QUEUED], "pipelines queued", "%.0f");
    {
        CsFrameTimes ft;
        cs_frame_times(&ft);
        cs_row_begin('.', 0);
        cs_cell("present interval ms");
        cs_cell("%.3f", ft.avgMs); cs_cell("%.3f", ft.maxMs); cs_cell("%.3f", ft.minMs);
        cs_row_end();
    }

    cs_group("Last complete frame");
    cs_cols("Counter\tValue");
    if (as != NULL) {
        cs_row_begin('.', 0); cs_cell("pipelines queued / created");
        cs_cell("%u / %u", as->queuedPipelines, as->createdPipelines); cs_row_end();
        cs_row_begin('.', 0); cs_cell("buffer bytes vert/index/uniform/storage");
        cs_cell("%u / %u / %u / %u", as->lastVertSize, as->lastIndexSize,
                as->lastUniformSize, as->lastStorageSize); cs_row_end();
    }
    cs_row_begin(fi.walkOk ? '.' : 'r', 0);
    cs_cell("FI retained stream");
    cs_cell("%u B in %u chunk(s), walk %s, %u pos matrices",
            fi.streamBytes, fi.chunkCount, fi.walkOk ? "ok" : "FAIL", fi.posCount);
    cs_row_end();

    cs_window_note();
    cs_note("Inclusive includes the nested overlay bracket; Exclusive is submit alone.");
    cs_note("The two FI rows' CallCount is frame_interp.c's OWN census window (it owns");
    cs_note("that counter and this file never keeps a second one); every other row's is");
    cs_note("the retained tick count above.");
    cs_note("GX submit is a CPU-side wall proxy, NOT GPU time -- see `stat gpu`.");
}

#endif /* MP6_HEADLESS_BUILD */

/* ONE record buffer for every panel, on purpose. Overlays stack now, so this
 * function is called once per latched panel per refresh -- but each caller
 * lays its block out into DOM (mp6::ui::build_stat_records) before asking for
 * the next one, so the blocks never need to coexist. A per-panel 16 KB buffer
 * would be eight allocations to avoid a discipline the single call site
 * already has. */
const char *mp6_console_panel_text(int panel)
{
    cs_reset();
    cs_rates_refresh();
    switch (panel) {
    case MP6_CONSOLE_PANEL_UNIT:   cs_panel_unit();   break;
    case MP6_CONSOLE_PANEL_FPS:    cs_panel_fps();    break;
    case MP6_CONSOLE_PANEL_SCENE:  cs_panel_scene();  break;
    case MP6_CONSOLE_PANEL_GAME:   cs_panel_game();   break;
    case MP6_CONSOLE_PANEL_GPU:    cs_panel_gpu();    break;
    case MP6_CONSOLE_PANEL_MEMORY: cs_panel_memory(); break;
    case MP6_CONSOLE_PANEL_AUDIO:  cs_panel_audio();  break;
    case MP6_CONSOLE_PANEL_BOARD:  cs_panel_board();  break;
    default: break;
    }
    return s_text;
}

/* =======================================================================
 * 6. Savestate.
 * ======================================================================= */

void mp6_console_savestate_reset(void)
{
    /* The TU is carved out, so a restore did not clobber these -- but the
     * retained timestamps describe PRE-restore frames, exactly the reason
     * platform/gx/frame_interp.c drops its retained streams at this same
     * point. An interval measured across the load itself is not a frame time. */
    memset(s_presentNs, 0, sizeof(s_presentNs));
    memset(s_presentReplay, 0, sizeof(s_presentReplay));
    s_presentHead = 0;
    s_presentCount = 0;
    memset(s_phase, 0, sizeof(s_phase));
    s_phaseHead = 0;
    s_phaseCount = 0;
    /* Process slice DURATIONS are wall time from the same clock, so a window
     * straddling the load would report the restore itself as a process. The
     * row IDENTITIES (entry pointer -> symbol) are not timestamps and survive:
     * dropping them would only force dbghelp to resolve them all again. */
    {
        int i;
        for (i = 0; i < s_procN; i++) {
            s_proc[i].calls = 0; s_proc[i].sumNs = 0; s_proc[i].maxNs = 0;
            s_proc[i].lastCalls = 0; s_proc[i].lastSumNs = 0; s_proc[i].lastMaxNs = 0;
        }
    }
    /* Same reasoning one level up: the game counters are windowed, and a
     * window straddling the load would average a pre-restore process list
     * against a post-restore one. The event delta's baseline goes too --
     * the bus's cumulative totals do not rewind, but nothing here may assume
     * that. */
    memset(s_gCur, 0, sizeof(s_gCur));
    memset(s_gLast, 0, sizeof(s_gLast));
    s_gEventsPrev = 0;
    s_gEventsSeen = 0;
    s_windowTicks = 0;
    s_windowTicksLast = 0;
    s_rateWindowNs = 0;
    s_rateWindowTick = 0;
    s_rateWindowPresents = s_presentsTotal;
    s_rateWindowReplays = s_replaysTotal;
    s_rateTicksPerSec = 0.0;
    s_ratePresentsPerSec = 0.0;
    s_rateReplaysPerSec = 0.0;
    mp6_console_log("[savestate] restore -- sampler rings dropped (pre-restore frames)");
}
