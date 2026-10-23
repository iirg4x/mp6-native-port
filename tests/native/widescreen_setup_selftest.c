#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "game/mgdata.h"
#include "game/charman.h"
#include "game/board/main.h"
#include "game/flag.h"

GW_PLAYER_CONF GwPlayerConf[GW_PLAYER_MAX];
GW_PLAYER GwPlayer[GW_PLAYER_MAX];
GW_SYSTEM GwSystem;
static int boardInitializations, storyInitializations, clearedFlag;
void _ClearFlag(u32 flag) { clearedFlag = flag; }
void mbSaveInit(s32 board)
{
    GwSystem.boardNo = board;
    _ClearFlag(FLAG_BOARD_SAVEINIT);
}
void mbSaveStoryInit(s32 character, s32 pack, s32 difficulty)
{
    assert(character == GwPlayerConf[1].charNo && !pack && difficulty == 1);
    ++storyInitializations;
}
void mbSavePartyInit(BOOL teams, BOOL bonus, s32 pack, s32 turns,
                    s32 a, s32 b, s32 c, s32 d)
{
    assert(!teams && bonus && !pack && turns == 20 && !a && !b && !c && !d);
    ++boardInitializations;
}
MGDATA MgDataTbl[40];
static int unlocks[100], unlockCalls;
void GWMgUnlockSet(int game)
{
    assert(game >= GW_MGNO_BASE && game < GW_MGNO_BASE+100);
    ++unlocks[game-GW_MGNO_BASE];
    ++unlockCalls;
}
static int loads[CHARNO_MAX];
s16 GwMgNightF;
static const char *qaNight;
static const char *qaCpu, *qaDifficulty, *qaLevels;
static char *qa_getenv(const char *name) {
    if (!strcmp(name,"MP6_WS_QA_NIGHT")) return (char *)qaNight;
    if (!strcmp(name,"MP6_WS_QA_ALL_CPU")) return (char *)qaCpu;
    if (!strcmp(name,"MP6_WS_QA_CPU_LEVELS")) return (char *)qaLevels;
    assert(!strcmp(name,"MP6_WS_QA_CPU_DIFFICULTY"));
    return (char *)qaDifficulty;
}
s32 MgNoGet(s16 overlay) { return overlay >= 1000 && overlay < 1000+MG_TYPE_MAX ? overlay-1000 : -1; }
void CharMotionInit(s16 character) { assert(character >= 0 && character < CHARNO_MAX); ++loads[character]; }
#define getenv qa_getenv
#include "setup_subject.inc"
#undef getenv

int main(void)
{
    const s16 characters[4] = {7,4,2,8};
    for(int type=0;type<MG_TYPE_MAX;++type) {
        memset(loads,0,sizeof(loads));
        MgDataTbl[type].type=type;
        for(int p=0;p<4;++p)
            GwPlayerConf[p]=(GW_PLAYER_CONF){characters[p],3-p,p,0,p!=0};
        GW_PLAYER_CONF native[4];
        memcpy(native,GwPlayerConf,sizeof(native));
        qa_prepare_minigame(1000+type);
        for(int p=0;p<4;++p) {
            int group=type==MG_TYPE_1VS3?(p!=0):type==MG_TYPE_2VS2?p/2:p;
            assert(GwPlayerConf[p].grpNo==group && loads[characters[p]]==1);
            native[p].grpNo=group;
        }
        assert(!memcmp(native,GwPlayerConf,sizeof(native)));
        qaCpu="1";
        qaDifficulty="3";
        qa_prepare_minigame(1000+type);
        for(int p=0;p<4;++p) {
            native[p].type=1;
            native[p].comDif=3;
        }
        assert(!memcmp(native,GwPlayerConf,sizeof(native)));
        qaDifficulty="9";
        qa_prepare_minigame(1000+type);
        assert(!memcmp(native,GwPlayerConf,sizeof(native)));
        qaLevels="3012";
        qa_prepare_minigame(1000+type);
        for(int p=0;p<4;++p) native[p].comDif=qaLevels[p]-'0';
        assert(!memcmp(native,GwPlayerConf,sizeof(native)));
        for(int i=0;i<4;++i) {
            qaLevels=(const char*[]){"301","30122","3019","301x"}[i];
            qa_prepare_minigame(1000+type);
            assert(!memcmp(native,GwPlayerConf,sizeof(native)));
        }
        qaCpu="0";
        qaDifficulty="0";
        qaLevels="3333";
        qa_prepare_minigame(1000+type);
        assert(!memcmp(native,GwPlayerConf,sizeof(native)));
        qaCpu=qaDifficulty=qaLevels=NULL;
    }
    GW_PLAYER_CONF before[4];
    memcpy(before,GwPlayerConf,sizeof(before));
    memset(loads,0,sizeof(loads));
    qa_prepare_minigame(-1);
    assert(!memcmp(before,GwPlayerConf,sizeof(before)));
    for(int i=0;i<CHARNO_MAX;++i) assert(!loads[i]);
    GwMgNightF=1;
    qa_prepare_minigame(1000);
    assert(GwMgNightF==1); /* An absent override preserves the native selection. */
    qaNight="0";
    qa_prepare_minigame(1000);
    assert(GwMgNightF==0);
    qaNight="1";
    qa_prepare_minigame(1000);
    assert(GwMgNightF==1);
    assert(boardInitializations == 0);
    qa_prepare_board(DLL_w03dll);
    assert(boardInitializations == 1 && GwSystem.boardNo == 2);
    assert(clearedFlag == FLAG_BOARD_SAVEINIT);
    for (int p=0;p<GW_PLAYER_MAX;++p) {
        assert(GwPlayer[p].charNo == GwPlayerConf[p].charNo);
        assert(GwPlayer[p].padNo == GwPlayerConf[p].padNo);
        assert(GwPlayer[p].comF && GwPlayer[p].comDif == 1 && !GwPlayer[p].team);
    }
    const s16 boards[] = {DLL_w01dll,DLL_w02dll,DLL_w03dll,DLL_w04dll,
        DLL_w05dll,DLL_w06dll,DLL_s01dll,DLL_s02dll,DLL_s03dll,DLL_w10dll,DLL_w11dll};
    for (int b=0;b<11;++b) {
        int partyBefore=boardInitializations, soloBefore=storyInitializations;
        qa_prepare_board(boards[b]);
        assert(GwSystem.boardNo == b);
        assert(boardInitializations-partyBefore == (b<6 || b==9));
        assert(storyInitializations-soloBefore == (b>=6 && b<=8));
    }
    int resultsBefore=boardInitializations;
    qa_prepare_minigame(DLL_mdpresultdll);
    assert(boardInitializations==resultsBefore+1 && GwSystem.boardNo==0);
    assert(!unlockCalls);
    MgDataTbl[0].ovl=DLL_m602dll;
    MgDataTbl[1].ovl=DLL_m616dll;
    MgDataTbl[2].ovl=DLL_m670dll;
    MgDataTbl[3].ovl=DLL_m601dll; /* Unported: never unlocked by admission setup. */
    MgDataTbl[4].ovl=DLL_NONE;
    const s16 modes[] = {DLL_mdminidll,DLL_mgmfreedll,DLL_mgmbattledll,
        DLL_mgmbingodll,DLL_mgmdecathlondll,DLL_mgmrenshodll,DLL_mgmtournamentdll};
    for (unsigned m=0;m<sizeof(modes)/sizeof(modes[0]);++m)
        qa_prepare_mode(modes[m]);
    assert(unlockCalls==21 && unlocks[0]==7 && unlocks[1]==7 && unlocks[2]==7);
    assert(!unlocks[3]);
    qa_prepare_mode(DLL_mdsingdll);
    qa_prepare_mode(-1);
    assert(unlockCalls==21);
    return 0;
}
