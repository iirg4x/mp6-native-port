/* No minigame or instruction overlay is native yet. Replace board/mgcall.c
 * as a build input, before roulette, wagers, wipes or overlay teardown.
 * board.c treats TRUE as "continue this board"; FALSE sleeps forever while
 * waiting for an overlay return. Do not award coins or unlock unplayed games.
 */
#include <stdio.h>
#include "mp6_events.h"
#include "mp6_minigame.h"

static int skipPending;

int mp6_minigame_take_skipped(void)
{
    int pending = skipPending;
    skipPending = 0;
    return pending;
}

static void skipped(const char *kind)
{
    printf("[MINIGAME] %s skipped: no native minigames available\n", kind);
    fflush(stdout);
    skipPending = 1;
    mp6_event_post("minigame.skipped", 0, kind);
}

void mbMgCallInit(void) {}
int mbev_MgCall(void) { skipped("end of turn"); return 1; }
void mbMgRouletteFocusKill(int kill) { (void)kill; }
void mbev_MgCallKettou(void) { skipped("duel"); }
void mbev_MgCallDonkey(void) { skipped("Donkey Kong"); }
void mbev_MgCallKoopa(void) { skipped("Bowser"); }
void mbev_MgCallSingle(int player) { (void)player; skipped("single player"); }
void mbev_MgCallSingleKoopa(int player, int koopa)
{
    (void)player; (void)koopa; skipped("single player / Bowser");
}
void mbMgCallDataClose(void) {}
void mbev_MgCallTutorial(void) { skipped("tutorial minigame"); }
int mbMgCallVsEffCreate(void) { return 0; }
/* single.c passes an overlay id; the decomp definition omitted it. */
int mbMgCallSingleOnCheck(unsigned short overlay) { (void)overlay; return 0; }
int mbMgRouletteNumGet(int type) { (void)type; return 0; }
