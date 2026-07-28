/* Native boundary placeholders for shared-board code that is still absent
 * from the clean, pinned marioparty6 main branch.
 *
 * These are deliberately kept out of the decomp tree: they are integration
 * seams, not recovered implementations.  Every entry logs its first call so
 * a placeholder can never masquerade as working board behavior.  Delete each
 * definition as soon as the same symbol lands on decomp main.
 */
#include "dolphin.h"
#include "mp6_shim_log.h"

#include <stdio.h>

/* Call-count instrumentation.  MP6_LOG_ONCE alone answers "did this seam
 * fire at all", which is not enough to RANK the seams by gameplay impact:
 * a one-shot board-opening no-op and a per-frame no-op both print exactly
 * one line.  So each seam also carries its own counter and re-reports at
 * decade boundaries (1, 10, 100, 1000, ...).  That is O(1) per call, needs
 * no exit hook (the windowed runs are killed, so atexit is not reliable),
 * and bounds the log at ~log10(N) lines per seam. */
static void mp6_board_placeholder_hit(const char *symbol, const char *result,
                                      unsigned long *count, unsigned long *next)
{
    if (++*count >= *next) {
        printf("[BOARD-PLACEHOLDER-COUNT] %s -> %s : %lu call(s)\n",
               symbol, result, *count);
        fflush(stdout);
        *next = (*next < 1000000UL) ? (*next * 10UL) : (*next + 1000000UL);
    }
}

#define BOARD_PLACEHOLDER_ONCE(symbol, result)                                \
    do {                                                                       \
        static unsigned long mp6_phCount__ = 0;                                \
        static unsigned long mp6_phNext__ = 1;                                 \
        MP6_LOG_ONCE("BOARD-PLACEHOLDER", symbol " -> " result);               \
        mp6_board_placeholder_hit(symbol, result, &mp6_phCount__,              \
                                  &mp6_phNext__);                              \
    } while (0)

/* Board loop / turn flow. */
void mbTelopTimeChangeCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbTelopTimeChangeCreate", "no-op");
}

s32 mbev_SingleMgEnd(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbev_SingleMgEnd", "return TRUE (finish event)");
    return TRUE;
}

void mbev_CapKettouEndCall(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbev_CapKettouEndCall", "no-op");
}

void mbev_CapDonkeyEndCall(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbev_CapDonkeyEndCall", "no-op");
}

void mbev_CapKoopaEndCall(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbev_CapKoopaEndCall", "no-op");
}

void mbev_Last5(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_Last5", "no-op");
}

void mbTelopLastTurnCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbTelopLastTurnCreate", "no-op");
}

s32 mbSingleCall(s32 mode, s32 arg)
{
    (void)mode;
    (void)arg;
    BOARD_PLACEHOLDER_ONCE("mbSingleCall", "return -1 (no selection)");
    return -1;
}

s32 mbev_MgCall(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_MgCall", "return TRUE (do not suspend board loop)");
    return TRUE;
}

void mbCapInit(void)
{
    BOARD_PLACEHOLDER_ONCE("mbCapInit", "no-op");
}

void mbSingleInit(void)
{
    BOARD_PLACEHOLDER_ONCE("mbSingleInit", "no-op");
}

/* Capsule events.  Private decomp structs are represented by ABI-equivalent
 * opaque pointers at this native boundary. */
void mbev_CapBiriQMetalShock(void *work)
{
    (void)work;
    BOARD_PLACEHOLDER_ONCE("mbev_CapBiriQMetalShock", "no-op");
}

BOOL mbCapEffUseCreate(s32 playerNo, s32 capsuleNo)
{
    (void)playerNo;
    (void)capsuleNo;
    BOARD_PLACEHOLDER_ONCE("mbCapEffUseCreate", "return FALSE (effect unavailable)");
    return FALSE;
}

void mbev_CapBonusCoinCall(s32 playerNo, s32 capsuleNo, s32 coinNum, BOOL wait)
{
    (void)playerNo;
    (void)capsuleNo;
    (void)coinNum;
    (void)wait;
    BOARD_PLACEHOLDER_ONCE("mbev_CapBonusCoinCall", "no-op");
}

void mbCapListRead(void)
{
    BOARD_PLACEHOLDER_ONCE("mbCapListRead", "no-op");
}

void ev_CapCoinAdd(void *obj, s32 playerNo, s32 coinNum, BOOL high,
    void (*hook)(void))
{
    (void)obj;
    (void)playerNo;
    (void)coinNum;
    (void)high;
    (void)hook;
    BOARD_PLACEHOLDER_ONCE("ev_CapCoinAdd", "no-op");
}

s32 mbev_CapPlayerSquishVoiceSet(s32 *playerNo, s32 masuId, BOOL voice)
{
    (void)playerNo;
    (void)masuId;
    (void)voice;
    BOARD_PLACEHOLDER_ONCE("mbev_CapPlayerSquishVoiceSet", "return -1 (no target)");
    return -1;
}

BOOL mbev_CapCullCheck(s32 playerNo, s32 masuId)
{
    (void)playerNo;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCullCheck", "return FALSE (not culled)");
    return FALSE;
}

s32 mbev_CapPlayerComSelSameGet(s32 playerNo, s32 selection, BOOL same)
{
    (void)playerNo;
    (void)selection;
    (void)same;
    BOARD_PLACEHOLDER_ONCE("mbev_CapPlayerComSelSameGet", "return -1 (no target)");
    return -1;
}

void mbev_CapEffRingOMExec(void *obj)
{
    (void)obj;
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffRingOMExec", "no-op");
}

s16 ev_CapEffCreate(void *anim, s16 max)
{
    (void)anim;
    (void)max;
    BOARD_PLACEHOLDER_ONCE("ev_CapEffCreate", "return -1 (no model)");
    return -1;
}

void mbev_CapEffElectricOMExec(void *obj)
{
    (void)obj;
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffElectricOMExec", "no-op");
}

void ev_CapEffGridSet(s16 modelId, s32 xNum, s32 yNum, s32 zNum)
{
    (void)modelId;
    (void)xNum;
    (void)yNum;
    (void)zNum;
    BOARD_PLACEHOLDER_ONCE("ev_CapEffGridSet", "no-op");
}

s32 mbCapObjColorCreate(s32 capsuleNo, BOOL create)
{
    (void)capsuleNo;
    (void)create;
    BOARD_PLACEHOLDER_ONCE("mbCapObjColorCreate", "return -1 (no object)");
    return -1;
}

void mbCapMasuObjCreate(s32 masuId)
{
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("mbCapMasuObjCreate", "no-op");
}

void CapColMdlIdGet(void)
{
    BOARD_PLACEHOLDER_ONCE("CapColMdlIdGet", "no-op");
}

BOOL CapColCheck(Vec *posA, Vec *posB, Vec *out)
{
    (void)posA;
    (void)posB;
    (void)out;
    BOARD_PLACEHOLDER_ONCE("CapColCheck", "return FALSE (no collision)");
    return FALSE;
}

void CapPlayerThrow(void)
{
    BOARD_PLACEHOLDER_ONCE("CapPlayerThrow", "no-op");
}

void CapAutoThrow(void *work)
{
    (void)work;
    BOARD_PLACEHOLDER_ONCE("CapAutoThrow", "no-op");
}

s32 mbCapComChanceGet(s32 capsuleNo, s32 playerNo, s32 mode)
{
    (void)capsuleNo;
    (void)playerNo;
    (void)mode;
    BOARD_PLACEHOLDER_ONCE("mbCapComChanceGet", "return 0 (no preference)");
    return 0;
}

s16 mbCapMasuDispTypeGet(s16 masuId)
{
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("mbCapMasuDispTypeGet", "return 0 (default display type)");
    return 0;
}

void CapSelectMasuAddFront(s16 *masuFlag, s16 masuId, s16 max)
{
    (void)masuFlag;
    (void)masuId;
    (void)max;
    BOARD_PLACEHOLDER_ONCE("CapSelectMasuAddFront", "no-op");
}

void CapSelectMasuAddBack(s16 *masuFlag, s16 masuId, s16 max)
{
    (void)masuFlag;
    (void)masuId;
    (void)max;
    BOARD_PLACEHOLDER_ONCE("CapSelectMasuAddBack", "no-op");
}

void CapSelectMasuLinkCheck(s16 *masuFlag, s16 masuId)
{
    (void)masuFlag;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("CapSelectMasuLinkCheck", "no-op");
}

/* Pause, guide, and shared board-object effects. */
void PauseGuideMain(void)
{
    BOARD_PLACEHOLDER_ONCE("PauseGuideMain", "no-op");
}

void mbObjBiriQCreate(s16 modelId)
{
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbObjBiriQCreate", "no-op");
}

void mbObjBiriQColorSet(s16 modelId, BOOL set, float alpha, GXColor color)
{
    (void)modelId;
    (void)set;
    (void)alpha;
    (void)color;
    BOARD_PLACEHOLDER_ONCE("mbObjBiriQColorSet", "no-op");
}

void mbObjFadeCreate(s32 modelId, Vec *pos)
{
    (void)modelId;
    (void)pos;
    BOARD_PLACEHOLDER_ONCE("mbObjFadeCreate", "no-op");
}

void mbObjFadeTexColorSet(s32 modelId, s32 r, s32 g, s32 b, float a)
{
    (void)modelId;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    BOARD_PLACEHOLDER_ONCE("mbObjFadeTexColorSet", "no-op");
}

void mbObjFadeTexRotSet(s32 modelId, Vec *pos, Vec *rot)
{
    (void)modelId;
    (void)pos;
    (void)rot;
    BOARD_PLACEHOLDER_ONCE("mbObjFadeTexRotSet", "no-op");
}

void mbObjFadeKill(s32 modelId)
{
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbObjFadeKill", "no-op");
}

/* Capsule dispatch from the movement/player translation units. */
void mbCapMasuExec(s32 playerNo, s32 id)
{
    (void)playerNo;
    (void)id;
    BOARD_PLACEHOLDER_ONCE("mbCapMasuExec", "no-op");
}

s32 mbev_CapCall(s32 playerNo, s32 capsuleNo, BOOL move, BOOL stop)
{
    (void)playerNo;
    (void)capsuleNo;
    (void)move;
    (void)stop;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCall", "return 0 (event skipped)");
    return 0;
}

void mbev_CapCallDonkey(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCallDonkey", "no-op");
}

void mbev_CapCallKoopa(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCallKoopa", "no-op");
}

void mbev_CapCallKettou(s32 playerNo, s16 masuId, BOOL stop)
{
    (void)playerNo;
    (void)masuId;
    (void)stop;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCallKettou", "no-op");
}

void mbev_CapCallMiracle(s32 playerNo, s32 masuId)
{
    (void)playerNo;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCallMiracle", "no-op");
}

void mbev_SingleMg(s32 playerNo, s16 masuId)
{
    (void)playerNo;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("mbev_SingleMg", "no-op");
}

void mbev_MgCallSingleKoopa(s32 playerNo, BOOL koopa)
{
    (void)playerNo;
    (void)koopa;
    BOARD_PLACEHOLDER_ONCE("mbev_MgCallSingleKoopa", "no-op");
}

/* Telops and pause configuration.
 *
 * mbTelopCreate is NOT here any more: it graduated from "no-op" to a real
 * behavioral seam in platform/os/board_telop.c (see that file's header for
 * what it is and what it is not).  A no-op there is what made the board
 * name/logo overlay never appear during the board-opening flyover. */

s16 mbTelopTimeSprCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbTelopTimeSprCreate", "return -1 (no sprite)");
    return -1;
}

void mbTelopTimeSprRotSet(s16 id, float rot)
{
    (void)id;
    (void)rot;
    BOARD_PLACEHOLDER_ONCE("mbTelopTimeSprRotSet", "no-op");
}

BOOL mbConfigExec(s32 playerNo, s32 modelId)
{
    (void)playerNo;
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbConfigExec", "return FALSE (not cancelled)");
    return FALSE;
}

/* Metal/BiriQ material hooks. */
void mbObjMetalCreate(s16 modelId)
{
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbObjMetalCreate", "no-op");
}

void mbObjMetalTPLvlSet(s16 modelId, float level)
{
    (void)modelId;
    (void)level;
    BOARD_PLACEHOLDER_ONCE("mbObjMetalTPLvlSet", "no-op");
}

void mbObjMetalColorSet(s16 modelId, GXColor shadow, GXColor hilite)
{
    (void)modelId;
    (void)shadow;
    (void)hilite;
    BOARD_PLACEHOLDER_ONCE("mbObjMetalColorSet", "no-op");
}

void mbObjMetalKill(s16 modelId)
{
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbObjMetalKill", "no-op");
}

BOOL mbObjBiriQKill(s16 modelId)
{
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbObjBiriQKill", "return TRUE (already absent)");
    return TRUE;
}

void mbTelopTimeCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbTelopTimeCreate", "no-op");
}

void mbTelopPlayerCreate(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbTelopPlayerCreate", "no-op");
}

void mbev_CapKillerMoveCall(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbev_CapKillerMoveCall", "no-op");
}

s32 mbCapSelect(void)
{
    BOARD_PLACEHOLDER_ONCE("mbCapSelect", "return -1 (cancel selection)");
    return -1;
}

void mbev_CapCallTrap(s32 playerNo, s16 masuId, s16 masuIdNext)
{
    (void)playerNo;
    (void)masuId;
    (void)masuIdNext;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCallTrap", "no-op");
}

/* Map/scroll, shop, save, star, and W01 Teresa seams. */
void InitScrollCol(void)
{
    BOARD_PLACEHOLDER_ONCE("InitScrollCol", "no-op");
}

BOOL MapViewExec(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("MapViewExec", "return FALSE (exit map view)");
    return FALSE;
}

BOOL ScrollExec(s32 playerNo, s16 starMasuId)
{
    (void)playerNo;
    (void)starMasuId;
    BOARD_PLACEHOLDER_ONCE("ScrollExec", "return FALSE (exit scroll view)");
    return FALSE;
}

void MapSprCreate(s32 type, s32 id, s32 layer)
{
    (void)type;
    (void)id;
    (void)layer;
    BOARD_PLACEHOLDER_ONCE("MapSprCreate", "no-op");
}

void ev_Shop(void *work)
{
    (void)work;
    BOARD_PLACEHOLDER_ONCE("ev_Shop", "no-op");
}

void mbSingleSaveFlush(s32 value)
{
    (void)value;
    BOARD_PLACEHOLDER_ONCE("mbSingleSaveFlush", "no-op");
}

void mbev_StarScroll(Vec *startPos, Vec *endPos, int time)
{
    (void)startPos;
    (void)endPos;
    (void)time;
    BOARD_PLACEHOLDER_ONCE("mbev_StarScroll", "no-op");
}

void mbObjStarTevStageSet(void *drawObj, void *material, s32 *tevNo,
    s32 *texNo)
{
    (void)drawObj;
    (void)material;
    (void)tevNo;
    (void)texNo;
    BOARD_PLACEHOLDER_ONCE("mbObjStarTevStageSet", "no-op");
}

void mbev_CapTeresaFadeCreate(s16 modelId)
{
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbev_CapTeresaFadeCreate", "no-op");
}

void mbev_CapCallTeresa(s32 playerNo, s16 masuId)
{
    (void)playerNo;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCallTeresa", "no-op");
}
