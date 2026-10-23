#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/object.h"
#include "game/gamework.h"
#include "game/mgdata.h"
#include "mp6_minigame.h"

s32 omovlhisidx;
OMOVL omcurovl;
MGDATA MgDataTbl[2];
GW_PLAYER GwPlayer[GW_PLAYER_MAX];
static int closedArchive, closeCount;
static int returned, destination, changed, events, unavailable;
s32 MgNoGet(s16 overlay)
{
    return overlay == DLL_m617dll ? 1 : -1;
}
void HuDataDirClose(int dataNum)
{
    assert(!returned && destination == DLL_NONE);
    closedArchive = dataNum;
    closeCount++;
}
static OMOVLHIS history[16];
OMOVLHIS *omOvlHisGet(s32 offset)
{
    assert(offset >= 0 && offset <= omovlhisidx && offset < 16);
    return &history[omovlhisidx - offset];
}
void omOvlReturnEx(s16 offset, s16 unlink)
{
    assert(unlink == TRUE);
    returned = offset;
    destination = history[omovlhisidx - offset].ovl;
}
void omOvlHisChg(s32 offset, OMOVL overlay, s32 event, s32 status)
{
    assert(omovlhisidx == 0 && offset == 0 && event == 0 && status == 0);
    history[0].ovl = overlay;
    changed++;
}
void omOvlGotoEx(OMOVL overlay, s16 unlink, s32 event, s32 status)
{
    assert(unlink && event == 0 && status == 0);
    destination = overlay;
}
void mp6_event_post(const char *key, long number, const char *value)
{
    assert(value && number >= 0);
    if (strcmp(key, "minigame.unavailable") == 0) {
        assert(strcmp(value, "board") == 0 && number == omcurovl);
        unavailable++;
        return;
    }
    assert(strcmp(key, "overlay.skipped") == 0);
    events++;
}
static void reset(int depth)
{
    omovlhisidx = depth;
    returned = changed = events = unavailable = 0;
    destination = DLL_NONE;
    omcurovl = DLL_mdbankdll;
    closedArchive = -1;
    closeCount = 0;
    for (int i = 0; i < 16; ++i) history[i].ovl = DLL_m601dll;
}
int main(void)
{
    assert(!mp6_unavailable_overlay_take_returned());
    reset(2);
    history[0].ovl = DLL_mdseldll;
    history[1].ovl = DLL_w01dll;
    history[1].evtno = 9; history[1].stat = 42;
    /* The stub announces on its black screen; the UI takes the notice once
     * and holds it open until it closes, then the stub returns. */
    mp6_unavailable_overlay_announce();
    assert(mp6_unavailable_notice_pending() && !mp6_unavailable_notice_open());
    assert(mp6_unavailable_overlay_take_returned() == MP6_UNAVAILABLE_RETURNED);
    assert(!mp6_unavailable_overlay_take_returned());
    assert(!mp6_unavailable_notice_pending() && mp6_unavailable_notice_open());
    mp6_unavailable_notice_closed();
    assert(!mp6_unavailable_notice_open());
    mp6_unavailable_overlay_return();
    assert(!mp6_unavailable_overlay_take_returned());
    assert(returned == 1 && destination == DLL_w01dll && events == 1);
    assert(history[1].evtno == 9 && history[1].stat == 42);
    reset(3); history[1].ovl = DLL_mdpartydll;
    mp6_unavailable_overlay_return();
    assert(returned == 2 && destination == DLL_mdpartydll);
    /* Resumable minigame menus stay local. Endurance's pending result state
     * must not consume a fabricated result from a missing game. */
    reset(3); history[0].ovl = DLL_mdseldll;
    history[1].ovl = DLL_mgmfreedll; history[2].ovl = DLL_instdll;
    mp6_unavailable_overlay_return();
    assert(returned == 2 && destination == DLL_mgmfreedll);
    assert(closeCount == 0); /* Missing modes do not own a minigame archive. */
    reset(3); history[1].ovl = DLL_mgmfreedll; history[2].ovl = DLL_instdll;
    omcurovl = DLL_m617dll;
    MgDataTbl[1].dataDir = 0x210000;
    mp6_unavailable_overlay_return();
    assert(returned == 2 && destination == DLL_mgmfreedll);
    assert(closeCount == 1 && closedArchive == 0x210000);
    /* An already-closed archive is a safe no-op in HuDataDirClose; the
     * fallback must still identify the same owner on every repeated visit. */
    for (int visit = 0; visit < 32; ++visit) {
        reset(3); history[1].ovl = DLL_mgmfreedll; history[2].ovl = DLL_instdll;
        omcurovl = DLL_m617dll;
        mp6_unavailable_overlay_return();
        assert(closeCount == 1 && closedArchive == 0x210000);
    }
    reset(3); history[0].ovl = DLL_mdminidll;
    history[1].ovl = DLL_mgmrenshodll; history[2].ovl = DLL_instdll;
    mp6_unavailable_overlay_return();
    assert(returned == 3 && destination == DLL_mdminidll);
    for (int depth = -1; depth < 16; ++depth) {
        reset(depth);
        mp6_unavailable_overlay_return();
        assert(!returned && changed == 1 && events == 1);
        assert(destination == DLL_mdseldll && history[0].ovl == DLL_mdseldll);
    }
    /* A missing minigame called from any board ends at once with nobody
     * scoring and returns a single step, like MgSeq's close: results (or the
     * instdll hand-back of duel/DK/Bowser games) still run, then the board. */
    static const OMOVL boards[] = { DLL_w01dll, DLL_w02dll, DLL_w03dll, DLL_w04dll,
                                    DLL_w05dll, DLL_w06dll, DLL_w10dll, DLL_w11dll,
                                    DLL_s01dll, DLL_s02dll, DLL_s03dll };
    for (unsigned b = 0; b < sizeof(boards) / sizeof(boards[0]); ++b) {
        for (int type = MG_TYPE_4P; type <= MG_TYPE_DONKEY; ++type) {
            reset(3); history[0].ovl = DLL_mdpartydll; history[1].ovl = boards[b];
            history[2].ovl = type == MG_TYPE_KETTOU || type == MG_TYPE_KUPA ||
                             type == MG_TYPE_DONKEY ? DLL_instdll : DLL_resultdll;
            omcurovl = DLL_m617dll;
            MgDataTbl[1].type = type;
            memset(GwPlayer, 0x5a, sizeof(GwPlayer));
            for (int p = 0; p < GW_PLAYER_MAX; ++p) {
                GwPlayer[p].mgCoin = 10 + p;      /* a previous game's result */
                GwPlayer[p].mgCoinBonus = p - 1;
                GwPlayer[p].mgScore = 1000;
            }
            GW_PLAYER before[GW_PLAYER_MAX];
            memcpy(before, GwPlayer, sizeof(before));
            mp6_unavailable_overlay_announce();
            assert(mp6_unavailable_overlay_take_returned() == MP6_UNAVAILABLE_BOARD_MINIGAME);
            mp6_unavailable_overlay_return();
            assert(returned == 1 && destination == history[2].ovl);
            assert(unavailable == 1 && events == 0 && changed == 0);
            assert(closeCount == 1 && closedArchive == 0x210000);
            /* Returning drops the hold even if the UI never closed it. */
            assert(!mp6_unavailable_notice_open());
            for (int p = 0; p < GW_PLAYER_MAX; ++p) {
                assert(GwPlayer[p].mgCoin == 0 && GwPlayer[p].mgScore == 0);
                assert(GwPlayer[p].mgCoinBonus == (type == MG_TYPE_KUPA ? 1 : 0));
                /* Coins, stars, battle stakes and graphs stay with the game. */
                before[p].mgCoin = GwPlayer[p].mgCoin;
                before[p].mgCoinBonus = GwPlayer[p].mgCoinBonus;
                before[p].mgScore = GwPlayer[p].mgScore;
            }
            assert(memcmp(before, GwPlayer, sizeof(before)) == 0);
        }
    }
    /* Mini-Game mode keeps unwinding to its menu, never via results. */
    reset(3); history[1].ovl = DLL_mgmfreedll; history[2].ovl = DLL_instdll;
    omcurovl = DLL_m617dll;
    mp6_unavailable_overlay_announce();
    assert(mp6_unavailable_overlay_take_returned() == MP6_UNAVAILABLE_RETURNED);
    mp6_unavailable_overlay_return();
    assert(unavailable == 0 && destination == DLL_mgmfreedll);
    /* Without a UI nothing takes the notice; returning drops it, so it can
     * never surface later over another screen. */
    reset(3); history[1].ovl = DLL_mgmfreedll; history[2].ovl = DLL_instdll;
    omcurovl = DLL_m617dll;
    mp6_unavailable_overlay_announce();
    mp6_unavailable_overlay_return();
    assert(!mp6_unavailable_notice_pending() && !mp6_unavailable_overlay_take_returned());
    /* A missing non-minigame module under a board is not a board minigame. */
    reset(2); history[0].ovl = DLL_mdseldll; history[1].ovl = DLL_w02dll;
    mp6_unavailable_overlay_return();
    assert(unavailable == 0 && returned == 2 && destination == DLL_mdseldll);
    puts("missing overlay return: PASS");
}
