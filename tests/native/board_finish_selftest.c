#include <stdio.h>
#include <string.h>
#include "game/board/main.h"
#include "game/flag.h"
GW_SYSTEM GwSystem;
static int nextOvl, exits, sleeps, changes, tutorial, move_done;
BOOL _CheckFlag(u32 flag) { return flag==FLAG_BOARD_TUTORIAL && tutorial; }
void _ClearFlag(u32 flag) { if (flag==FLAG_BOARD_MOVE_DONE) move_done=0; }
void _SetFlag(u32 flag) { if (flag==FLAG_BOARD_MOVE_DONE) move_done=1; }
void mbExitReq(void) { exits++; }
void HuPrcSleep(s32 ticks) { if (ticks==-1) sleeps++; }
void mbChangeTimeSet(void) { changes++; }
#include "board_subject.inc"
#define CHECK(c) do { if (!(c)) {printf("FAIL %d: %s\n",__LINE__,#c);return 1;} } while(0)
static void reset(int turn, int day_turn) {
    memset(&GwSystem,0,sizeof(GwSystem));
    GwSystem.partyF=1; GwSystem.turnMax=20; GwSystem.turnNo=turn;
    GwSystem.timeTurnMax=3; GwSystem.timeTurn=day_turn;
    exits=sleeps=changes=tutorial=0; move_done=1;
}
int main(void) {
    reset(20,2); mbNextTime(); CHECK(!exits && !sleeps && !changes);
    reset(19,3); mbNextTime(); CHECK(exits==1 && sleeps==1 && changes==1 && move_done);
    reset(21,2); mbNextTime(); CHECK(exits==1 && sleeps==1 && !changes && !move_done);
    reset(21,3); mbNextTime(); CHECK(exits==1 && sleeps==1 && !changes && !move_done);
    reset(21,2); tutorial=1; mbNextTime(); CHECK(!exits && !sleeps);
    reset(21,2); GwSystem.partyF=0; mbNextTime(); CHECK(!exits && !sleeps);
    puts("board completion after minigame stub: PASS");
    return 0;
}
