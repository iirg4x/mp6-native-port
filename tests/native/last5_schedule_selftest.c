#include <stdio.h>
#include <string.h>
#include "game/board/main.h"
#include "game/flag.h"
GW_SYSTEM GwSystem;
static int last5, ceremony, telop;
BOOL _CheckFlag(u32 flag) { return flag==FLAG_BOARD_LAST5 && last5; }
void _SetFlag(u32 flag) { if (flag==FLAG_BOARD_LAST5) last5=1; }
void mbev_Last5(void) { ceremony++; }
void mbTelopLastTurnCreate(void) { telop++; }
void mbSingleCall(int id,int unused) { }
#include "board_subject.inc"
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n",__LINE__,#c); return 1; } } while(0)
int main(void) {
    for (int turns=10;turns<=50;turns+=5) {
        memset(&GwSystem,0,sizeof(GwSystem));
        GwSystem.turnMax=turns; GwSystem.partyF=1;
        last5=ceremony=telop=0;
        for (int round=1;round<=turns;round++) {
            GwSystem.turnNo=round;
            GwSystem.timeTurn=(round-1)%3;
            GwSystem.curTime=((round-1)/3)%2;
            for (int player=0;player<4;player++) {
                GwSystem.turnPlayerNo=player;
                check_last5(0);
                CHECK(ceremony==(round>=turns-4));
            }
        }
        CHECK(ceremony==1 && telop==4);
        last5=ceremony=0; GwSystem.turnNo=turns-4; GwSystem.turnPlayerNo=0;
        check_last5(1); CHECK(ceremony==0); /* interrupted turn cannot replay */
    }
    puts("Last Five Turns scheduling: PASS");
    return 0;
}
