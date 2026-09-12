/* MP6 native port -- developer console CORE. See include/mp6_console.h
 * for the whole contract; this file is the mechanism.
 *
 * Deliberately dependency-light -- <stdio.h>/<stdarg.h>/<string.h> plus the
 * header-only strict parser -- exactly like src/os/mp6_events.c, and in
 * tools/build.py's PLATFORM_SOURCES_COMMON for the same reason: it must LINK
 * HEADLESS so tools/console_selftest.c can drive the real registry, ring,
 * tokenizer and lever table with no window, no GPU and no aurora.
 *
 * It owns every piece of console STATE. src/gx/ui/console.cpp is a pure
 * view over what is decided here, which is why "is the console open" and "does
 * the console own the keyboard" are answerable from a headless test.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mp6_console.h"
#include "mp6_parse.h" /* mp6_parse_i32_strict -- header-only */

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md). Every static below describes the
 * RUNNING process's debug session -- the open latch, the ring positions, the
 * registered function POINTERS -- never deterministic game state. Restoring a
 * capturing process's command-table function pointers into a loading one is
 * the same failure class src/gx/frame_dump.c is carved out for.
 * Registered in tools/build.py HOST_STATE_SECTION_SOURCES; placed AFTER this
 * file's own includes and at preprocessor top level, which
 * verify_host_section_sources() enforces. */
#include "mp6_host_section.h"


#define CON_LINE_MAX   200
#define CON_LOG_LINES  256
#define CON_HIST_LINES  32
#define CON_HIST_MAX   128
#define CON_MAX_CMDS    32
#define CON_MAX_ARGV     8
#define CON_TOKEN_MAX   64

/* =======================================================================
 * 1. Availability + the input bar's state.
 * ======================================================================= */

static int s_available;  /* 0 until the launcher says otherwise -- automation
                          * never reaches the call that sets it */
static int s_bar;        /* MP6_CONSOLE_BAR_* */

static void console_drop_overlays(void); /* defined with the latch array */

void mp6_console_set_available(int available)
{
    s_available = available ? 1 : 0;
    if (!s_available) {
        s_bar = MP6_CONSOLE_BAR_CLOSED; /* never strand the capture on */
        console_drop_overlays();        /* nor leave an overlay over a dead UI */
    }
}

int mp6_console_available(void) { return s_available; }

int mp6_console_bar(void) { return s_available ? s_bar : MP6_CONSOLE_BAR_CLOSED; }

int mp6_console_is_open(void)  { return mp6_console_bar() >= MP6_CONSOLE_BAR_INPUT; }
int mp6_console_log_page(void) { return mp6_console_bar() == MP6_CONSOLE_BAR_LOG; }

void mp6_console_set_bar(int state)
{
    if (!s_available) return;
    if (state < MP6_CONSOLE_BAR_CLOSED || state > MP6_CONSOLE_BAR_LOG) return;
    s_bar = state;
}

void mp6_console_open(void)
{
    if (!s_available) return;
    if (s_bar == MP6_CONSOLE_BAR_CLOSED) s_bar = MP6_CONSOLE_BAR_INPUT;
}

void mp6_console_close(void) { s_bar = MP6_CONSOLE_BAR_CLOSED; }

/* The reference's own progression, and the reason the toggle key is a CYCLE
 * rather than a flip: the bar alone is what you want nine times out of ten,
 * and the scrollback is one more press away instead of a second binding
 * nobody remembers. */
void mp6_console_toggle(void)
{
    if (!s_available) return; /* inert in automation, by construction */
    s_bar = (s_bar >= MP6_CONSOLE_BAR_LOG) ? MP6_CONSOLE_BAR_CLOSED : (s_bar + 1);
}

/* The modifier (shift+`) -- jump straight to the scrollback and straight back,
 * without cycling through CLOSED. */
void mp6_console_toggle_log(void)
{
    if (!s_available) return;
    s_bar = (s_bar == MP6_CONSOLE_BAR_LOG) ? MP6_CONSOLE_BAR_INPUT : MP6_CONSOLE_BAR_LOG;
}

int mp6_console_captures_input(void)
{
    return mp6_console_is_open() ? 1 : 0;
}

/* =======================================================================
 * 2. Stat overlay latches.
 *
 * One latch per panel, all in one array -- the generalization of what the
 * `stat unit` corner HUD already was. Gated on availability for the same
 * reason the bar is: automation must not be able to draw one, and none may
 * survive mp6_launcher_ui_teardown(). They do NOT capture input -- an overlay
 * is furniture, not a surface.
 * ======================================================================= */

static const char *const kPanelNames[MP6_CONSOLE_PANEL_COUNT] = {
    "none", "unit", "fps", "scenerendering", "game", "gpu", "memory", "audio", "board"
};

/* Index 0 (`none`) is never latched: it is the word that CLEARS the others. */
static unsigned char s_overlay[MP6_CONSOLE_PANEL_COUNT];

static int panel_valid(int panel)
{
    return panel > MP6_CONSOLE_PANEL_NONE && panel < MP6_CONSOLE_PANEL_COUNT;
}

static void console_drop_overlays(void)
{
    memset(s_overlay, 0, sizeof(s_overlay));
}

int mp6_console_overlay(int panel)
{
    if (!s_available || !panel_valid(panel)) return 0;
    return s_overlay[panel] ? 1 : 0;
}

void mp6_console_set_overlay(int panel, int on)
{
    if (!s_available || !panel_valid(panel)) return;
    s_overlay[panel] = on ? 1u : 0u;
}

int mp6_console_toggle_overlay(int panel)
{
    if (!s_available || !panel_valid(panel)) return 0;
    s_overlay[panel] = s_overlay[panel] ? 0u : 1u;
    return s_overlay[panel] ? 1 : 0;
}

int mp6_console_overlay_count(void)
{
    int i, n = 0;
    if (!s_available) return 0;
    for (i = 1; i < MP6_CONSOLE_PANEL_COUNT; i++) if (s_overlay[i]) n++;
    return n;
}

void mp6_console_clear_overlays(void) { console_drop_overlays(); }

/* THE one arming predicate: `the bar is up OR ANY overlay is latched`, never
 * `open`. Every stat panel is now a persistent overlay that survives the bar
 * closing, and they all read the same phase buckets, GX census and present
 * ring the panels inside the console used to -- so a closed bar must not
 * silently disarm sampling for an overlay the user is still looking at.
 *
 * It lives HERE rather than in console_stats.c (where the rings it gates are)
 * because it reads nothing from the sampler: it is a pure function of the two
 * latch sets above. Keeping it beside them is what lets tools/console_selftest.c
 * prove the bar/overlay independence against the real code, and what keeps
 * aurora_bridge.c's phase clock, the hot GX hook sites' relaxed load and the
 * present ring all agreeing about when sampling is live -- one function, one
 * answer. */
int mp6_console_stats_armed(void)
{
    return (mp6_console_is_open() || mp6_console_overlay_count() > 0) ? 1 : 0;
}

const char *mp6_console_panel_name(int panel)
{
    if (panel < 0 || panel >= MP6_CONSOLE_PANEL_COUNT) return "?";
    return kPanelNames[panel];
}

int mp6_console_panel_from_name(const char *name)
{
    int i;
    if (name == NULL) return -1;
    for (i = 0; i < MP6_CONSOLE_PANEL_COUNT; i++) {
        if (strcmp(kPanelNames[i], name) == 0) return i;
    }
    return -1;
}

/* =======================================================================
 * 3. Output ring.
 *
 * A fixed ring, never an allocation: the console logs from the frame
 * boundary and from command handlers, and a debug instrument that can fail
 * on malloc is worse than one with a bounded history.
 * ======================================================================= */

static char s_log[CON_LOG_LINES][CON_LINE_MAX];
static int  s_logHead;   /* next slot to write */
static int  s_logCount;  /* retained */
static int  s_logTotal;  /* ever written -- the view's generation counter */

void mp6_console_clear_log(void)
{
    s_logHead = 0;
    s_logCount = 0;
    s_logTotal++; /* still a generation change: the view must repaint */
}

void mp6_console_log(const char *fmt, ...)
{
    va_list ap;
    char *dst;
    if (fmt == NULL) return;
    dst = s_log[s_logHead];
    va_start(ap, fmt);
    vsnprintf(dst, sizeof(s_log[0]), fmt, ap);
    va_end(ap);
    s_logHead = (s_logHead + 1) % CON_LOG_LINES;
    if (s_logCount < CON_LOG_LINES) s_logCount++;
    s_logTotal++;
}

int mp6_console_log_total(void) { return s_logTotal; }
int mp6_console_log_count(void) { return s_logCount; }

const char *mp6_console_log_line(int back)
{
    int idx;
    if (back < 0 || back >= s_logCount) return NULL;
    idx = s_logHead - 1 - back;
    while (idx < 0) idx += CON_LOG_LINES;
    return s_log[idx];
}

/* =======================================================================
 * 4. History.
 * ======================================================================= */

static char s_hist[CON_HIST_LINES][CON_HIST_MAX];
static int  s_histHead;
static int  s_histCount;

static void console_history_push(const char *line)
{
    if (line == NULL || line[0] == '\0') return;
    if (s_histCount > 0) {
        const char *prev = s_hist[(s_histHead + CON_HIST_LINES - 1) % CON_HIST_LINES];
        if (strcmp(prev, line) == 0) return; /* no runs of the same command */
    }
    snprintf(s_hist[s_histHead], CON_HIST_MAX, "%s", line);
    s_histHead = (s_histHead + 1) % CON_HIST_LINES;
    if (s_histCount < CON_HIST_LINES) s_histCount++;
}

int mp6_console_history_count(void) { return s_histCount; }

const char *mp6_console_history(int back)
{
    int idx;
    if (back < 0 || back >= s_histCount) return NULL;
    idx = s_histHead - 1 - back;
    while (idx < 0) idx += CON_HIST_LINES;
    return s_hist[idx];
}

/* =======================================================================
 * 5. Lever (cvar) table.
 *
 * The table is STATIC and complete at compile time, so the toggles panel can
 * list a lever the process has not consulted yet. What is discovered at
 * runtime is only each lever's ENV-latched value, reported by its own getter
 * the first time it runs -- which is exactly what lets the panel print "env=1
 * override=0 (console wins)" instead of one number with no provenance.
 * ======================================================================= */

typedef struct {
    const char *name;
    const char *env;
    const char *help;
    int minValue;
    int maxValue;
} ConCvarDesc;

static const ConCvarDesc kCvars[MP6_CVAR_COUNT] = {
    { "fi_diag", "MP6_FI_DIAG",
      "Unlocked-FPS diagnostics: 1 periodic, 2 per-seal stamp, 3 per-replay detail", 0, 3 },
    { "tickratelog", "MP6_TICK_RATE_LOG",
      "measured tick rate + lateness + per-tick phase averages, 1 line/5s", 0, 1 },
    { "presentratelog", "MP6_PRESENT_RATE_LOG",
      "presents/s vs ticks, 1 line/5s", 0, 1 },
    { "audiotimeline", "MP6_AUDIO_TIMELINE",
      "[SETL] SFX voice lifecycle keyed by game tick", 0, 1 },
    { "framescope", "MP6_FRAMESCOPE",
      "capture frame N's whole GX configuration trace (0 = off)", 0, 1000000000 },
    { "framedump", "MP6_FRAME_DUMP",
      "per-present GPU readback burst (0 disables an env-armed burst)", 0, 1 },
    { "alloccensus", "MP6_ALLOC_CENSUS_START_TICK",
      "all-heap census + symbolized alloc trace from this tick (-1 = off)", -1, 1000000000 },
    { "drawcount", "MP6_DIAG_DRAWCOUNT",
      "previous frame's draw-call count, 1 line/s", 0, 1 },
    { "skipdrawlo", "MP6_SKIP_DRAWS",
      "draw-bisect range low index (-1 = off)", -1, 1000000 },
    { "skipdrawhi", "MP6_SKIP_DRAWS",
      "draw-bisect range high index (-1 = off)", -1, 1000000 },
    { "arrayprobe", "MP6_ARRAYPROBE",
      "every GXSetArray bind/grow and display-list recording bracket", 0, 1 },
};

static int s_cvarHasOverride[MP6_CVAR_COUNT];
static int s_cvarOverride[MP6_CVAR_COUNT];
static int s_cvarEnvSeen[MP6_CVAR_COUNT];
static int s_cvarEnvValue[MP6_CVAR_COUNT];

static int cvar_valid(int id) { return id >= 0 && id < MP6_CVAR_COUNT; }

int mp6_console_cvar_get(int id, int envValue)
{
    if (!cvar_valid(id)) return envValue;
    s_cvarEnvSeen[id] = 1;
    s_cvarEnvValue[id] = envValue;
    return s_cvarHasOverride[id] ? s_cvarOverride[id] : envValue;
}

int mp6_console_cvar_set(int id, int value)
{
    if (!cvar_valid(id)) return 0;
    if (value < kCvars[id].minValue || value > kCvars[id].maxValue) return 0;
    s_cvarHasOverride[id] = 1;
    s_cvarOverride[id] = value;
    return 1;
}

int mp6_console_cvar_clear(int id)
{
    if (!cvar_valid(id)) return 0;
    s_cvarHasOverride[id] = 0;
    s_cvarOverride[id] = 0;
    return 1;
}

int mp6_console_cvar_id_from_name(const char *name)
{
    int i;
    if (name == NULL) return -1;
    for (i = 0; i < MP6_CVAR_COUNT; i++) {
        if (strcmp(kCvars[i].name, name) == 0) return i;
    }
    return -1;
}

const char *mp6_console_cvar_name(int id) { return cvar_valid(id) ? kCvars[id].name : "?"; }
const char *mp6_console_cvar_env(int id)  { return cvar_valid(id) ? kCvars[id].env : "?"; }
const char *mp6_console_cvar_help(int id) { return cvar_valid(id) ? kCvars[id].help : ""; }
int mp6_console_cvar_min(int id)          { return cvar_valid(id) ? kCvars[id].minValue : 0; }
int mp6_console_cvar_max(int id)          { return cvar_valid(id) ? kCvars[id].maxValue : 0; }
int mp6_console_cvar_has_override(int id) { return cvar_valid(id) ? s_cvarHasOverride[id] : 0; }
int mp6_console_cvar_override(int id)     { return cvar_valid(id) ? s_cvarOverride[id] : 0; }
int mp6_console_cvar_env_seen(int id)     { return cvar_valid(id) ? s_cvarEnvSeen[id] : 0; }
int mp6_console_cvar_env_value(int id)    { return cvar_valid(id) ? s_cvarEnvValue[id] : 0; }

/* =======================================================================
 * 6. Command registry + tokenizer.
 * ======================================================================= */

typedef struct {
    char name[CON_TOKEN_MAX];
    const char *help;
    Mp6ConsoleCmdFn fn;
} ConCmd;

static ConCmd s_cmds[CON_MAX_CMDS];
static int    s_cmdCount;
static int    s_builtinsRegistered;

static void console_register_builtins(void);

static void console_init_once(void)
{
    if (s_builtinsRegistered) return;
    s_builtinsRegistered = 1; /* set FIRST: the builtins call back into
                               * mp6_console_register, which calls this */
    console_register_builtins();
}

int mp6_console_register(const char *name, const char *help, Mp6ConsoleCmdFn fn)
{
    int i;
    console_init_once();
    if (name == NULL || name[0] == '\0' || fn == NULL) return 0;
    if (strlen(name) >= CON_TOKEN_MAX) return 0;
    for (i = 0; i < s_cmdCount; i++) {
        if (strcmp(s_cmds[i].name, name) == 0) { /* re-registration replaces */
            s_cmds[i].help = (help != NULL) ? help : "";
            s_cmds[i].fn = fn;
            return 1;
        }
    }
    if (s_cmdCount >= CON_MAX_CMDS) return 0;
    snprintf(s_cmds[s_cmdCount].name, CON_TOKEN_MAX, "%s", name);
    s_cmds[s_cmdCount].help = (help != NULL) ? help : "";
    s_cmds[s_cmdCount].fn = fn;
    s_cmdCount++;
    return 1;
}

int mp6_console_command_count(void)
{
    console_init_once();
    return s_cmdCount;
}

const char *mp6_console_command_name(int index)
{
    console_init_once();
    if (index < 0 || index >= s_cmdCount) return NULL;
    return s_cmds[index].name;
}

const char *mp6_console_command_help(int index)
{
    console_init_once();
    if (index < 0 || index >= s_cmdCount) return NULL;
    return s_cmds[index].help;
}

/* Whitespace-split into at most CON_MAX_ARGV tokens; the final token keeps
 * whatever is left of the line so `echo a b c` reports one string. Tokens
 * longer than CON_TOKEN_MAX-1 are truncated rather than rejected -- a
 * truncated argument fails its own parse loudly, which is a better message
 * than "line too long". */
static int console_tokenize(const char *line, char tokens[CON_MAX_ARGV][CON_TOKEN_MAX])
{
    int argc = 0;
    const char *p = line;
    while (*p != '\0' && argc < CON_MAX_ARGV) {
        size_t n;
        const char *start;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') p++;
        n = (size_t)(p - start);
        if (n >= CON_TOKEN_MAX) n = CON_TOKEN_MAX - 1;
        memcpy(tokens[argc], start, n);
        tokens[argc][n] = '\0';
        argc++;
    }
    return argc;
}

int mp6_console_exec(const char *line)
{
    char tokens[CON_MAX_ARGV][CON_TOKEN_MAX];
    const char *argv[CON_MAX_ARGV];
    int argc, i;

    console_init_once();
    if (line == NULL) return 0;
    argc = console_tokenize(line, tokens);
    if (argc == 0) return 0;
    console_history_push(line);
    for (i = 0; i < argc; i++) argv[i] = tokens[i];

    for (i = 0; i < s_cmdCount; i++) {
        if (strcmp(s_cmds[i].name, tokens[0]) == 0) {
            s_cmds[i].fn(argc, argv);
            return 1;
        }
    }
    mp6_console_log("unknown command '%s' -- try 'help'", tokens[0]);
    return 0;
}

/* =======================================================================
 * 6b. Autocomplete.
 *
 * The popup above the input bar is a VIEW of this function and holds no list
 * of its own -- which is what makes the completion rules testable headless,
 * and what stops a second, drifting copy of "what commands exist" from
 * appearing in an RmlUi TU. See mp6_console_complete()'s contract in
 * include/mp6_console.h.
 *
 * Case-insensitive, without <ctype.h>: this file's include set is asserted by
 * tools/test_console_contract.py precisely so that "dependency-light" cannot
 * erode one header at a time, and a five-character ASCII fold does not earn
 * an exception. The registry is ASCII by construction (a command name comes
 * from a string literal in this tree).
 * ======================================================================= */

static char console_fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int console_has_prefix(const char *word, const char *prefix, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (word[i] == '\0') return 0;
        if (console_fold(word[i]) != console_fold(prefix[i])) return 0;
    }
    return 1;
}

int mp6_console_complete(const char *line, const char **out, int max, int *tokenStart)
{
    size_t len, start, firstStart, firstEnd, secondStart, prefixLen;
    const char *prefix;
    char verb[CON_TOKEN_MAX];
    int total = 0, i, kind;

    if (tokenStart != NULL) *tokenStart = 0;
    if (max < 0) max = 0;
    console_init_once();
    if (line == NULL) return 0;
    len = strlen(line);

    /* The token under the caret is whatever follows the last separator. */
    start = len;
    while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '\t') start--;
    if (tokenStart != NULL) *tokenStart = (int)start;

    firstStart = 0;
    while (line[firstStart] == ' ' || line[firstStart] == '\t') firstStart++;
    firstEnd = firstStart;
    while (line[firstEnd] != '\0' && line[firstEnd] != ' ' && line[firstEnd] != '\t') firstEnd++;
    secondStart = firstEnd;
    while (line[secondStart] == ' ' || line[secondStart] == '\t') secondStart++;

    if (start <= firstEnd) {
        kind = 0; /* still typing the command word itself */
    } else if (start != secondStart) {
        /* A third or later token: nothing in this registry describes those
         * (a lever VALUE is an integer, not a name), and offering the second
         * token's list again would be a lie about what would happen. */
        kind = -1;
    } else {
        size_t n = firstEnd - firstStart;
        if (n >= CON_TOKEN_MAX) n = CON_TOKEN_MAX - 1;
        memcpy(verb, line + firstStart, n);
        verb[n] = '\0';
        if (strcmp(verb, "stat") == 0) {
            kind = 1;
        } else if (strcmp(verb, "get") == 0 || strcmp(verb, "set") == 0 ||
                   strcmp(verb, "unset") == 0) {
            kind = 2;
        } else if (strcmp(verb, "help") == 0) {
            kind = 0; /* `help <command>` completes against the same list */
        } else {
            kind = -1;
        }
    }

    prefix = line + start;
    prefixLen = len - start;
    switch (kind) {
    case 0:
        for (i = 0; i < s_cmdCount; i++) {
            if (!console_has_prefix(s_cmds[i].name, prefix, prefixLen)) continue;
            if (out != NULL && total < max) out[total] = s_cmds[i].name;
            total++;
        }
        break;
    case 1:
        for (i = 0; i < MP6_CONSOLE_PANEL_COUNT; i++) {
            if (!console_has_prefix(kPanelNames[i], prefix, prefixLen)) continue;
            if (out != NULL && total < max) out[total] = kPanelNames[i];
            total++;
        }
        break;
    case 2:
        for (i = 0; i < MP6_CVAR_COUNT; i++) {
            if (!console_has_prefix(kCvars[i].name, prefix, prefixLen)) continue;
            if (out != NULL && total < max) out[total] = kCvars[i].name;
            total++;
        }
        break;
    default:
        break;
    }
    return total;
}

/* =======================================================================
 * 7. Built-in commands.
 * ======================================================================= */

static void cmd_help(int argc, const char *const *argv)
{
    int i;
    if (argc >= 2) {
        for (i = 0; i < s_cmdCount; i++) {
            if (strcmp(s_cmds[i].name, argv[1]) == 0) {
                mp6_console_log("%s -- %s", s_cmds[i].name, s_cmds[i].help);
                return;
            }
        }
        mp6_console_log("no such command '%s'", argv[1]);
        return;
    }
    mp6_console_log("commands:");
    for (i = 0; i < s_cmdCount; i++) {
        mp6_console_log("  %-10s %s", s_cmds[i].name, s_cmds[i].help);
    }
}

static void console_log_panels(void)
{
    int i;
    mp6_console_log("  stat none           drop every overlay");
    for (i = 1; i < MP6_CONSOLE_PANEL_COUNT; i++) {
        mp6_console_log("  stat %-14s%s", kPanelNames[i],
                        mp6_console_overlay(i) ? " <- ON" : "");
    }
    mp6_console_log("  overlays stay up when this bar closes -- that is the point.");
}

static void cmd_stat(int argc, const char *const *argv)
{
    int panel;
    if (argc < 2) {
        mp6_console_log("stat: %d overlay(s) up", mp6_console_overlay_count());
        console_log_panels();
        return;
    }
    panel = mp6_console_panel_from_name(argv[1]);
    if (panel < 0) {
        mp6_console_log("stat: unknown panel '%s'", argv[1]);
        console_log_panels();
        return;
    }
    if (panel == MP6_CONSOLE_PANEL_NONE) {
        mp6_console_clear_overlays();
        mp6_console_log("stat none -- every overlay dropped");
        return;
    }
    mp6_console_log("stat %s %s", kPanelNames[panel],
                    mp6_console_toggle_overlay(panel) ? "ON" : "off");
}

static void cmd_panels(int argc, const char *const *argv)
{
    (void)argc; (void)argv;
    console_log_panels();
}

static void console_log_cvar(int id)
{
    char envText[32];
    if (s_cvarEnvSeen[id]) {
        snprintf(envText, sizeof(envText), "%d", s_cvarEnvValue[id]);
    } else {
        snprintf(envText, sizeof(envText), "unread");
    }
    mp6_console_log("  %-15s env(%s)=%s override=%s  <- %s",
                    kCvars[id].name, kCvars[id].env, envText,
                    s_cvarHasOverride[id] ? "set" : "-",
                    s_cvarHasOverride[id] ? "console" : "env");
}

static void cmd_toggles(int argc, const char *const *argv)
{
    int i;
    (void)argc; (void)argv;
    mp6_console_log("runtime levers (console setter beats the env latch):");
    for (i = 0; i < MP6_CVAR_COUNT; i++) console_log_cvar(i);
    mp6_console_log("  env-only (read inside a decomp patch, not settable here):");
    mp6_console_log("    MP6_OM_CENSUS  MP6_DRAW_CENSUS  MP6_DRAW_CENSUS_OBJ");
}

static void cmd_get(int argc, const char *const *argv)
{
    int id;
    if (argc < 2) { mp6_console_log("usage: get <lever>"); return; }
    id = mp6_console_cvar_id_from_name(argv[1]);
    if (id < 0) { mp6_console_log("no such lever '%s' -- try 'toggles'", argv[1]); return; }
    console_log_cvar(id);
    mp6_console_log("  %s", kCvars[id].help);
}

static void cmd_set(int argc, const char *const *argv)
{
    int id, value;
    if (argc < 3) { mp6_console_log("usage: set <lever> <int>"); return; }
    id = mp6_console_cvar_id_from_name(argv[1]);
    if (id < 0) { mp6_console_log("no such lever '%s' -- try 'toggles'", argv[1]); return; }
    if (!mp6_parse_i32_strict(argv[2], kCvars[id].minValue, kCvars[id].maxValue, &value)) {
        mp6_console_log("set %s: '%s' is not an integer in [%d, %d]",
                        kCvars[id].name, argv[2], kCvars[id].minValue, kCvars[id].maxValue);
        return;
    }
    mp6_console_cvar_set(id, value);
    console_log_cvar(id);
}

static void cmd_unset(int argc, const char *const *argv)
{
    int id;
    if (argc < 2) { mp6_console_log("usage: unset <lever>"); return; }
    id = mp6_console_cvar_id_from_name(argv[1]);
    if (id < 0) { mp6_console_log("no such lever '%s' -- try 'toggles'", argv[1]); return; }
    mp6_console_cvar_clear(id);
    console_log_cvar(id);
}

static void cmd_clear(int argc, const char *const *argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "log") != 0 && strcmp(argv[1], "stats") != 0)) {
        mp6_console_log("usage: clear [log|stats]");
        return;
    }
    if (argc == 1 || strcmp(argv[1], "log") == 0) mp6_console_clear_log();
    if (argc == 1 || strcmp(argv[1], "stats") == 0) mp6_console_clear_overlays();
}

static void cmd_echo(int argc, const char *const *argv)
{
    int i;
    char buf[CON_LINE_MAX];
    size_t used = 0;
    buf[0] = '\0';
    for (i = 1; i < argc; i++) {
        int n = snprintf(buf + used, sizeof(buf) - used, "%s%s", (i > 1) ? " " : "", argv[i]);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= sizeof(buf)) { used = sizeof(buf) - 1; break; }
    }
    mp6_console_log("%s", buf);
}

static void console_register_builtins(void)
{
    mp6_console_register("help", "help [command] -- list commands, or explain one", cmd_help);
    mp6_console_register("stat", "stat <panel>|none -- toggle a persistent overlay (see 'panels')", cmd_stat);
    mp6_console_register("panels", "panels -- list the stat overlays and which are up", cmd_panels);
    mp6_console_register("toggles", "toggles -- every runtime lever, its env value and its override", cmd_toggles);
    mp6_console_register("get", "get <lever> -- one lever's value and provenance", cmd_get);
    mp6_console_register("set", "set <lever> <int> -- override a lever at runtime", cmd_set);
    mp6_console_register("unset", "unset <lever> -- drop the override, env wins again", cmd_unset);
    mp6_console_register("clear", "clear [log|stats] -- clear console output and stat panels", cmd_clear);
    mp6_console_register("echo", "echo <text> -- print text into the ring", cmd_echo);
}

/* mp6_console_savestate_reset() is deliberately NOT here: the only console
 * state a restore must drop is the sampler's monotonic-timestamp rings, so it
 * is defined next to them in src/gx/console/console_stats.c. Everything
 * this file owns -- availability, the bar state, the overlay latches, the
 * command table, the lever overrides -- describes the live debug session and
 * survives a load. */
