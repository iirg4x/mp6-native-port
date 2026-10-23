/* MP6 native port -- the "Quick Party" mod (include/mp6_quick_play.h has the
 * contract). Reads the running overlay to tell boards and results screens
 * from minigames and menus; everything else is a plain store. */

#include "mp6_quick_play.h"

#include "dolphin.h"
#include "game/object.h"
#include "game/gamework.h"
#include "game/board/pause.h"
#include "messdir_enum.h"

#include <stdio.h>
#include <stdlib.h>

/* SAVESTATE CARVE-OUT: the published setting, the environment latch and the
 * per-tick hold window describe the running process, not the captured game.
 * Must sit AFTER this TU's own includes and at preprocessor TOP LEVEL. See
 * mp6_host_section.h. */
#include "mp6_host_section.h"

extern long mp6_tick_count; /* the tick the trace lines report */

static int g_setting = MP6_QUICK_PLAY_OFF;
/* -2 = environment not read yet; -1 = MP6_QUICK_PLAY unset; else 1..max. */
static int g_env = -2;
/* Ticks left at normal speed after the last mp6_quick_play_hold(), and what
 * asked for it (for the trace). */
static int g_holdTicks;
static const char *g_holdWhat;
/* MP6_QUICK_PLAY_TRACE: -1 = not read yet. */
static int g_trace = -1;
static const char *g_tracedWhat;
/* Last reported scene state, for the one log line per change. */
static int g_loggedScene = -1;
/* This tick's speed (mp6_quick_play_tick_factor()); 1 = normal. */
static int g_scale = 1;
/* Multiple of the base tick rate the device actually reaches while sped up
 * (mp6_quick_play_set_achieved), and the in-game scale that makes up the
 * rest: g_scale / achieved, never below 1. */
static double g_achieved = 1.0;
static double g_gameScale = 1.0;
/* The board event under way (mp6_quick_play_event_begin): its player, or
 * -1 outside one, and what keeps the rest of it at normal speed
 * (mp6_quick_play_event_watch), or NULL. Brackets nest -- an event that
 * walks players runs the space hooks again -- and the outermost one owns
 * the event. */
static int g_eventDepth;
static int g_eventPlayer = -1;
static const char *g_eventWatch;

static int quick_play_clamp(int speed)
{
    if (speed < MP6_QUICK_PLAY_OFF) return MP6_QUICK_PLAY_OFF;
    if (speed > MP6_QUICK_PLAY_MAX) return MP6_QUICK_PLAY_MAX;
    return speed;
}

static int quick_play_env(void)
{
    if (g_env == -2) {
        const char *value = getenv("MP6_QUICK_PLAY");
        if (value == NULL || value[0] == '\0') {
            g_env = -1;
        } else {
            g_env = quick_play_clamp(atoi(value));
        }
    }
    return g_env;
}

int mp6_quick_play_speed(void)
{
    const int env = quick_play_env();
    return env >= 0 ? env : g_setting;
}

void mp6_quick_play_set_speed(int speed)
{
    speed = quick_play_clamp(speed);
    if (speed != g_setting) {
        if (speed == MP6_QUICK_PLAY_OFF) printf("[MODS] Quick Party: off\n");
        else printf("[MODS] Quick Party: %dx\n", speed);
        fflush(stdout);
    }
    g_setting = speed;
}

int mp6_quick_play_env_override(void)
{
    return quick_play_env() >= 0;
}

static int quick_play_board_overlay(OMOVL ovl)
{
    return (ovl >= DLL_w01dll && ovl <= DLL_w06dll) || ovl == DLL_w10dll || ovl == DLL_w11dll;
}

/* Boards and the screens that tally them. Minigames (m6xx), their
 * instructions (instdll), mode menus, file select and the mic/story modes
 * stay at normal speed. */
static int quick_play_scene_overlay(OMOVL ovl)
{
    return quick_play_board_overlay(ovl) || ovl == DLL_resultdll || ovl == DLL_partyresultdll ||
           ovl == DLL_storyresultdll || ovl == DLL_mdpresultdll;
}

int mp6_quick_play_scene(void)
{
    const int scene = mp6_quick_play_speed() > MP6_QUICK_PLAY_OFF && quick_play_scene_overlay(omcurovl);
    if (scene != g_loggedScene) {
        g_loggedScene = scene;
        printf("[QUICK-PLAY] %s (overlay %d)\n", scene ? "sped-up scene" : "normal-speed scene",
               (int)omcurovl);
        fflush(stdout);
    }
    return scene;
}

int mp6_quick_play_skip_intro(void)
{
    return mp6_quick_play_speed() > MP6_QUICK_PLAY_OFF;
}

void mp6_quick_play_hold(const char *what)
{
    /* This tick's remaining pacing and the next one: hold sites poll every
     * tick, so the window only closes once the game stops waiting. */
    g_holdTicks = 2;
    g_holdWhat = what;
}

static int quick_play_human(int playerNo)
{
    return playerNo >= 0 && playerNo < GW_PLAYER_MAX && !GwPlayer[playerNo].comF;
}

void mp6_quick_play_event_begin(int playerNo)
{
    if (g_eventDepth++ == 0) {
        g_eventPlayer = playerNo;
        g_eventWatch = NULL;
    }
}

static void quick_play_event_clear(void)
{
    g_eventDepth = 0;
    g_eventPlayer = -1;
    g_eventWatch = NULL;
}

void mp6_quick_play_event_end(void)
{
    if (g_eventDepth > 0 && --g_eventDepth == 0) {
        quick_play_event_clear();
    }
}

void mp6_quick_play_event_watch(const char *what)
{
    if (g_eventWatch == NULL && quick_play_human(g_eventPlayer)) {
        g_eventWatch = what;
    }
}

int mp6_quick_play_human_present(void)
{
    int playerNo;
    for (playerNo = 0; playerNo < GW_PLAYER_MAX; playerNo++) {
        if (quick_play_human(playerNo)) return 1;
    }
    return 0;
}

void mp6_quick_play_countdown(void)
{
    if (!mp6_quick_play_scene() || !quick_play_board_overlay(omcurovl)) return;
    if (mp6_quick_play_human_present()) {
        mp6_quick_play_hold("countdown");
        mp6_quick_play_event_watch("timed challenge");
    }
}

static void quick_play_trace(const char *what)
{
    if (g_trace < 0) {
        const char *value = getenv("MP6_QUICK_PLAY_TRACE");
        g_trace = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    if (g_trace && what != g_tracedWhat) {
        g_tracedWhat = what;
        if (what) printf("[QUICK-PLAY] normal speed: waiting on %s (tick %ld)\n", what, mp6_tick_count);
        else printf("[QUICK-PLAY] full speed (tick %ld)\n", mp6_tick_count);
        fflush(stdout);
    }
}

static int quick_play_factor(void)
{
    const char *waitingOn = NULL;
    if (g_holdTicks > 0) {
        g_holdTicks--;
        waitingOn = g_holdWhat;
    }
    if (!mp6_quick_play_scene()) return 1;
    if (waitingOn == NULL && g_eventWatch != NULL) {
        /* Left the board mid-event (a savestate, a minigame): drop it. */
        if (quick_play_board_overlay(omcurovl)) waitingOn = g_eventWatch;
        else quick_play_event_clear();
    }
    /* The board pause menu (and its controller/settings pages) runs in the
     * board overlay; it is a menu, so it keeps menu speed. */
    if (waitingOn == NULL && quick_play_board_overlay(omcurovl) && mbPauseProcCheck()) {
        waitingOn = "pause menu";
    }
    quick_play_trace(waitingOn);
    return waitingOn ? 1 : mp6_quick_play_speed();
}

/* MP6_QUICK_PLAY_PARTS (diagnostic, default all): which in-game speed-ups
 * apply -- 1 process sleeps, 4 player and camera moves. */
static int g_parts = -1;

int mp6_quick_play_parts(void)
{
    if (g_parts < 0) {
        const char *value = getenv("MP6_QUICK_PLAY_PARTS");
        g_parts = (value != NULL && value[0] != 0) ? atoi(value) : 5;
    }
    return g_parts;
}

static void quick_play_update_game_scale(void)
{
    double scale = 1.0;
    if (g_scale > 1) {
        scale = (double)g_scale / (g_achieved > 1.0 ? g_achieved : 1.0);
        if (scale < 1.0) scale = 1.0;
        if (scale > (double)g_scale) scale = (double)g_scale;
    }
    g_gameScale = scale;
}

/* MP6_QUICK_PLAY_TICKS=0 (diagnostic): no extra ticks, so the in-game layer
 * carries the whole speed-up -- what the slowest device gets. */
static int g_ticksAllowed = -1;

int mp6_quick_play_tick_factor(void)
{
    if (g_ticksAllowed < 0) {
        const char *value = getenv("MP6_QUICK_PLAY_TICKS");
        g_ticksAllowed = !(value != NULL && value[0] == '0');
    }
    g_scale = quick_play_factor();
    if (!g_ticksAllowed) g_achieved = 1.0;
    quick_play_update_game_scale();
    return g_ticksAllowed ? g_scale : 1;
}

void mp6_quick_play_set_achieved(double multiple)
{
    g_achieved = multiple > 0.0 ? multiple : 1.0;
    quick_play_update_game_scale();
}

int mp6_quick_play_time_scale(void)
{
    return g_scale;
}

double mp6_quick_play_game_scale(void)
{
    return g_gameScale;
}

/* The in-game layer only shortens waits on boards, where the moves and
 * camera paths those waits cover are shortened by the same scale. A results
 * screen's script sleeps while its objects animate one step per tick (the
 * party results' 2nd-place fall, its bonus-star medals); shortening only the
 * sleep lets the script move on mid-animation and leave a model stranded. */
static int quick_play_scale(int ticks)
{
    int scaled;
    if (g_gameScale <= 1.0 || ticks <= 0 || !quick_play_board_overlay(omcurovl)) return ticks;
    scaled = (int)((double)ticks / g_gameScale + 0.5);
    return scaled < 1 ? 1 : scaled;
}

int mp6_quick_play_scale_sleep(int ticks)
{
    return (mp6_quick_play_parts() & 1) ? quick_play_scale(ticks) : ticks;
}

int mp6_quick_play_scale_ticks(int ticks)
{
    return (mp6_quick_play_parts() & 4) ? quick_play_scale(ticks) : ticks;
}

int mp6_quick_play_instant_text(void)
{
    /* Boards only: a results screen's script times its animations by its
     * pages (the party results start a bonus-star medal's 60-tick exit,
     * then show the next page and reuse the medal object), so pages there
     * print at the game's own speed and the extra ticks make them quick. */
    return mp6_quick_play_scene() && quick_play_board_overlay(omcurovl);
}

int mp6_quick_play_keywait_ticks(void)
{
    /* Explain-only lines move on as soon as they are printed (instantly on
     * boards, see game/window.c); one tick keeps the page from being skipped
     * unseen by the very press that opened it. Elsewhere a page waits as long
     * as a player pressing A right away. */
    return mp6_quick_play_instant_text() ? 1 : 20;
}

/* Lines that state what is at stake. The game does not mark them, so they
 * are listed here; everything else on a board is explain-only. */
static const u32 quick_play_stakes_mes[] = {
    MESSNUM(MESS_KOOPA_MASU, 8), /* Bowser: what losing his minigame costs */
    MESSNUM(MESS_KOOPA_MASU, 9), /* the same, team battle */
    MESSNUM(MESS_BOARD_W03, 32), /* Faire Square shell game: the prize */
};

int mp6_quick_play_must_read(unsigned int messNum)
{
    unsigned int i;
    for (i = 0; i < sizeof(quick_play_stakes_mes) / sizeof(quick_play_stakes_mes[0]); i++) {
        if (quick_play_stakes_mes[i] == messNum) return 1;
    }
    return 0;
}

/* Message directories of board events: each board's own and the shared
 * event spaces'. Help panels from these say what to press in the event;
 * general controls (dice, paths, map, skip) come from MESS_BOARD_OPE. */
static const u16 quick_play_event_dirs[] = {
    MESS_BOARD_W01, MESS_BOARD_W02, MESS_BOARD_W03, MESS_BOARD_W04,
    MESS_BOARD_W05, MESS_BOARD_W06, MESS_TERESA_MASU, MESS_MIRACLE_MASU,
    MESS_DONKEY_MASU, MESS_KOOPA_MASU,
};

int mp6_quick_play_action_help(unsigned int messNum)
{
    unsigned int i;
    for (i = 0; i < sizeof(quick_play_event_dirs) / sizeof(quick_play_event_dirs[0]); i++) {
        if (quick_play_event_dirs[i] == (messNum >> 16)) return 1;
    }
    return 0;
}

void mp6_quick_play_action_prompt(void)
{
    if (mp6_quick_play_scene() && quick_play_board_overlay(omcurovl) && mp6_quick_play_human_present()) {
        mp6_quick_play_hold("action prompt");
    }
}
