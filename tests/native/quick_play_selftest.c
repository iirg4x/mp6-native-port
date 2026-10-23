/* Quick Party store and per-tick speed policy (src/os/quick_play.c), linked
 * against stand-ins for the game symbols it reads. */
#include "mp6_quick_play.h"

#include "dolphin.h"
#include "game/object.h"
#include "game/gamework.h"
#include "messdir_enum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

OMOVL omcurovl = DLL_NONE;
long mp6_tick_count;
GW_PLAYER GwPlayer[GW_PLAYER_MAX];
static BOOL pauseOpen;
BOOL mbPauseProcCheck(void) { return pauseOpen; }

static const char *envValue;
char *mp6_test_getenv(const char *name)
{
    return strcmp(name, "MP6_QUICK_PLAY") == 0 ? (char *)envValue : NULL;
}

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int main(void)
{
    /* Off by default: every scene at normal speed, intro kept. */
    omcurovl = DLL_w01dll;
    CHECK(mp6_quick_play_speed() == MP6_QUICK_PLAY_OFF);
    CHECK(!mp6_quick_play_skip_intro());
    CHECK(!mp6_quick_play_scene());
    CHECK(mp6_quick_play_tick_factor() == 1);

    /* The published setting, clamped. */
    mp6_quick_play_set_speed(9);
    CHECK(mp6_quick_play_speed() == MP6_QUICK_PLAY_MAX);
    mp6_quick_play_set_speed(3);
    CHECK(mp6_quick_play_skip_intro());

    /* Boards and results are sped up; minigames, instructions, menus are not. */
    CHECK(mp6_quick_play_scene() && mp6_quick_play_tick_factor() == 3);
    omcurovl = DLL_w11dll;
    CHECK(mp6_quick_play_tick_factor() == 3);
    omcurovl = DLL_resultdll;
    CHECK(mp6_quick_play_tick_factor() == 3);
    omcurovl = DLL_m601dll;
    CHECK(!mp6_quick_play_scene() && mp6_quick_play_tick_factor() == 1);
    omcurovl = DLL_instdll;
    CHECK(mp6_quick_play_tick_factor() == 1);
    omcurovl = DLL_mdpartydll;
    CHECK(mp6_quick_play_tick_factor() == 1);

    /* A human wait holds normal speed for the tick it is polled and the next,
     * then full speed resumes on its own. */
    omcurovl = DLL_w01dll;
    mp6_quick_play_hold("dice block");
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 3);

    /* The board pause menu keeps menu speed. */
    pauseOpen = TRUE;
    CHECK(mp6_quick_play_tick_factor() == 1);
    pauseOpen = FALSE;
    CHECK(mp6_quick_play_tick_factor() == 3);

    /* In-game scale makes up what extra ticks do not reach: a device at the
     * full multiple gets none, one at 1.5x of 3x gets 2x. Forever (-1) and
     * "now" (0) pass through; press-A pauses move on at once. */
    CHECK(mp6_quick_play_tick_factor() == 3);
    mp6_quick_play_set_achieved(3.0);
    CHECK(mp6_quick_play_game_scale() == 1.0 && mp6_quick_play_scale_ticks(60) == 60);
    mp6_quick_play_set_achieved(1.5);
    CHECK(mp6_quick_play_game_scale() == 2.0);
    CHECK(mp6_quick_play_scale_sleep(60) == 30 && mp6_quick_play_scale_ticks(1) == 1);
    CHECK(mp6_quick_play_scale_ticks(-1) == -1 && mp6_quick_play_scale_ticks(0) == 0);
    mp6_quick_play_set_achieved(1.0);
    CHECK(mp6_quick_play_game_scale() == 3.0 && mp6_quick_play_scale_ticks(60) == 20);
    /* Results screens run sped up by extra ticks only: their scripts sleep
     * through per-tick object animations, which nothing shortens. */
    omcurovl = DLL_mdpresultdll;
    CHECK(mp6_quick_play_tick_factor() == 3 && mp6_quick_play_game_scale() == 3.0);
    CHECK(mp6_quick_play_scale_sleep(510) == 510 && mp6_quick_play_scale_ticks(60) == 60);
    /* ... and print their pages at the game's speed, then wait like a
     * prompt player: their scripts time animations by those pages. */
    CHECK(!mp6_quick_play_instant_text() && mp6_quick_play_keywait_ticks() == 20);
    omcurovl = DLL_w01dll;
    CHECK(mp6_quick_play_instant_text());
    CHECK(mp6_quick_play_tick_factor() == 3 && mp6_quick_play_scale_sleep(60) == 20);
    CHECK(mp6_quick_play_keywait_ticks() == 1);
    mp6_quick_play_hold("dice block");
    CHECK(mp6_quick_play_tick_factor() == 1 && mp6_quick_play_scale_sleep(60) == 60);
    CHECK(mp6_quick_play_tick_factor() == 1);

    /* A board countdown is a timed challenge: normal speed while it runs
     * if anyone is a person, and an all-computer party keeps its speed. */
    for (int i = 0; i < GW_PLAYER_MAX; i++) GwPlayer[i].comF = TRUE;
    mp6_quick_play_countdown();
    CHECK(mp6_quick_play_tick_factor() == 3);
    GwPlayer[2].comF = FALSE;
    mp6_quick_play_countdown();
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 3);
    /* On a computer's event only the countdown itself slows down ... */
    mp6_quick_play_event_begin(0);
    mp6_quick_play_event_watch("shell game");
    mp6_quick_play_countdown();
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 3);
    mp6_quick_play_event_end();
    /* ... on a person's event the rest of the event stays at normal speed,
     * through nested hook brackets, until the outermost one ends. */
    mp6_quick_play_event_begin(2);
    CHECK(mp6_quick_play_tick_factor() == 3);
    mp6_quick_play_event_watch("shell game");
    mp6_quick_play_event_begin(1);
    mp6_quick_play_event_end();
    for (int i = 0; i < 5; i++) CHECK(mp6_quick_play_tick_factor() == 1);
    mp6_quick_play_event_end();
    CHECK(mp6_quick_play_tick_factor() == 3);
    mp6_quick_play_event_begin(2);
    mp6_quick_play_countdown();
    CHECK(mp6_quick_play_tick_factor() == 1);
    for (int i = 0; i < 5; i++) CHECK(mp6_quick_play_tick_factor() == 1);
    mp6_quick_play_event_end();
    CHECK(mp6_quick_play_tick_factor() == 3);
    /* Lines that state the stakes wait for A; others are explain-only. */
    CHECK(mp6_quick_play_must_read(MESSNUM(MESS_KOOPA_MASU, 8)));
    CHECK(!mp6_quick_play_must_read(MESSNUM(MESS_KOOPA_MASU, 7)));

    /* Event help panels (mash A, pick) hold normal speed; general controls
     * (dice, path, "Start: skip") do not. */
    CHECK(mp6_quick_play_action_help(MESSNUM(MESS_TERESA_MASU, 17)));
    CHECK(mp6_quick_play_action_help(MESSNUM(MESS_BOARD_W02, 14)));
    CHECK(!mp6_quick_play_action_help(MESSNUM(MESS_BOARD_OPE, 12)));
    mp6_quick_play_action_prompt();
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 1);
    CHECK(mp6_quick_play_tick_factor() == 3);

    /* Off the board a countdown (a minigame's) changes nothing. */
    omcurovl = DLL_m601dll;
    mp6_quick_play_countdown();
    CHECK(mp6_quick_play_tick_factor() == 1 && mp6_quick_play_game_scale() == 1.0);
    CHECK(!mp6_quick_play_env_override());

    printf("quick play selftest ok\n");
    return 0;
}
