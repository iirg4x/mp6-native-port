/* MP6 native port -- board name/logo telop seam (`mbTelopCreate`).
 *
 * WHAT THIS IS
 * ------------
 * A PLACEHOLDER UPGRADE, not a decomp submission.  `mbTelopCreate` and its
 * two object-manager exec hooks live in `src/board/telop.c` on real
 * hardware, but they are absent from the pinned decomp tree (the file
 * recovers the mbTelopTime, mbTaunt, mbLanguage and mbBoardDataNum families
 * and stops there).  Until they land on decomp main this port carried
 * `mbTelopCreate` as a logged no-op in board_placeholders.c, which is
 * exactly why the board-opening flyover showed no board logo:
 * `src/board/opening.c:256` (`ev_OpeningParty`) creates the "Towering
 * Treetop" overlay with
 *
 *     mbTelopCreate(-1, GwSystem.boardNo + 16, FALSE);
 *
 * right after `mbWipeFadeIn()`/`mbMusPlay()` and before the 180-frame
 * `mbCameraMovePos` flyover, and a no-op swallowed it whole.
 *
 * WHERE THE BEHAVIOR COMES FROM
 * -----------------------------
 * Read out of the user's own retail GP6E01 `main.dol`, at the addresses
 * `config/GP6E01/symbols.txt` already names:
 *
 *     mbTelopCreate     .text:0x802035D0  size 0x164
 *     TelopInitOMExec   .text:0x8020399C  size 0x0C8  (scope:local)
 *     TelopOMExec       .text:0x80203A64  size 0x330  (scope:local)
 *     telopFileTbl      .rodata:0x8021AF30 size 0x06C  (scope:local)
 *
 * so this is a transcription of observed behavior, NOT an invention and NOT
 * a claim of a byte-matching recovery.  Names of the two statics below are
 * this file's own; the shapes (which work fields exist, at which widths)
 * are what the retail code reads and writes.  Delete this file the moment
 * src/board/telop.c grows the real definitions.
 *
 * WHAT IS DELIBERATELY NOT RETAIL-LITERAL
 * ---------------------------------------
 *  * Screen position.  Retail loads the literals 384.0f/240.0f, which are
 *    exactly half of the width/height its own sprite projection uses
 *    (`HuSprDispInit`, .text:0x800110CC, sets `C_MTXOrtho(proj, 0, 480, 0,
 *    768, 0, 10)`) -- i.e. dead centre of the 2D canvas.  This port's
 *    shared sprite canvas is HU_DISP_WIDTH x HU_DISP_HEIGHT wide (the
 *    decomp's `include/game/disp.h`, 576x480, which every other 2D caller
 *    in this build already uses), so the same INTENT is written with the
 *    canvas centre macros.  Hard-coding 384 here would put the logo two
 *    thirds across this port's canvas instead of in the middle.
 *  * espEntry failure.  Retail passes the result straight into
 *    espDrawNoSet, which would index esprite[-1] if the board archive
 *    entry ever failed to load.  Here that case is logged and the telop
 *    object is dropped instead -- a port-side safety seam, and the
 *    diagnostic that says "the sprite did not load" rather than "the logo
 *    silently did not appear".
 *
 * NOT COVERED HERE: `mbTelopCheck()` (src/board/telop.c:68) still reports
 * on telop.c's OWN, now permanently-NULL `telopOMObj` static, so it keeps
 * answering "no telop up" exactly as it did while mbTelopCreate was a
 * no-op.  Its single caller is the SOLO-mode opening
 * (src/board/opening.c:648), not the party path this seam serves; fixing
 * it belongs with the real telop.c recovery, not with a port seam that
 * cannot reach another translation unit's static.
 */
#include "dolphin.h"
#include "dolphin/pad.h"

#include "game/board/audio.h"
#include "game/board/main.h"
#include "game/disp.h"
#include "game/esprite.h"
#include "game/gamework.h"
#include "game/object.h"
#include "game/pad.h"
#include "game/process.h"

#include "mp6_shim_log.h"

#include <stdio.h>

/* telopFileTbl, .rodata:0x8021AF30 -- 27 board-archive data ids.  0..15 are
 * the in-board event telops; 16..26 are the per-board name/logo ANIMs, the
 * same board-archive files src/board/pause.c's own logoFileTbl already
 * names for the pause screen (board 0 == 0x44 == "Towering Treetop"). */
static const u32 mp6_telopFileTbl[27] = {
    0x0005004D, 0x0005004E, 0x0005004F, 0x00050050,
    0x00050051, 0x00050052, 0x00050053, 0x00050054,
    0x00050055, 0x00050056, 0x00050058, 0x00050057,
    0x00050057, 0x00050057, 0x0005005A, 0x00050059,
    0x00050044, 0x00050045, 0x00050046, 0x00050047,
    0x00050048, 0x00050049, 0x0005004C, 0x0005004B,
    0x0005004A, 0x00050044, 0x00050044,
};

/* First BOARD-NAME telop.  Retail branches on `telopNo >= 16` three times
 * (sound effects are for the event telops only; the board-name ones get
 * longer fades), so it gets a name here instead of a bare 16. */
#define MP6_TELOP_BOARD_NAME_FIRST 16

/* Retail's own literals: object priority, the one model/sprite slot it
 * carries, and the sprite draw layer. */
#define MP6_TELOP_OM_PRIO 262
#define MP6_TELOP_DRAW_NO 32

/* mbAudFXPlay ids used by the event telops (board-name telops are silent --
 * the opening plays its own music instead). */
#define MP6_TELOP_SE_IN  1011
#define MP6_TELOP_SE_OUT 1012

enum {
    MP6_TELOP_STATE_IN = 0,   /* scale/alpha ramp up */
    MP6_TELOP_STATE_HOLD = 1, /* on screen; waits out `delay`, then A/COM */
    MP6_TELOP_STATE_OUT = 2   /* scale up while fading out, then self-kill */
};

typedef struct Mp6TelopWork_s {
    u8 killF : 1;
    u8 state : 3;
    s8 playerNo;
    s8 telopNo;
    s8 delay;
    s16 timer;
    s16 timerMax;
} MP6_TELOP_WORK;

/* telop.c's `telopOMObj` is a file static there and unreachable from here;
 * this is the same live-object handle for the seam's own lifetime tracking
 * (mbTelopCreate's blocking `stat` wait, and the exec hook's self-kill). */
static OMOBJ *mp6_telopOMObj;

static void Mp6TelopOMExec(OMOBJ *obj);

static void Mp6TelopInitOMExec(OMOBJ *obj)
{
    MP6_TELOP_WORK *work = omObjGetWork(obj, MP6_TELOP_WORK);
    s16 sprId = obj->mdlId[0];

    work->state = MP6_TELOP_STATE_IN;
    work->timer = 0;
    work->timerMax = (work->telopNo >= MP6_TELOP_BOARD_NAME_FIRST) ? 60 : 15;
    espPosSet(sprId, HU_DISP_CENTERX, HU_DISP_CENTERY);
    espTPLvlSet(sprId, 0.0f);
    espScaleSet(sprId, 0.0f, 0.0f);
    espDispOn(sprId);
    obj->objFunc = Mp6TelopOMExec;
}

/* Retail's TelopOMExec: a three-state ramp driven off the object manager,
 * one step per board frame. */
static void Mp6TelopOMExec(OMOBJ *obj)
{
    MP6_TELOP_WORK *work = omObjGetWork(obj, MP6_TELOP_WORK);
    s16 sprId = obj->mdlId[0];
    float t;

    if (work->killF || mbExitCheck()) {
        espKill(sprId);
        mp6_telopOMObj = NULL;
        omDelObjEx(HuPrcCurrentGet(), obj);
        return;
    }

    switch (work->state) {
    case MP6_TELOP_STATE_IN:
        if (++work->timer >= work->timerMax) {
            work->state = MP6_TELOP_STATE_HOLD;
        }
        t = (float)work->timer / (float)work->timerMax;
        espTPLvlSet(sprId, t);
        espScaleSet(sprId, t, t);
        break;

    case MP6_TELOP_STATE_HOLD:
        /* `delay` is the automatic hold: set for a COM owner or for the
         * ownerless (playerNo < 0) board-name telop, which is exactly the
         * board-opening case -- nobody has to press anything for the logo
         * to leave. */
        if (work->delay != 0) {
            work->delay--;
            break;
        }
        if (work->playerNo >= 0
            && !(HuPadBtnDown[GwPlayer[work->playerNo].padNo] & PAD_BUTTON_A)
            && !GwPlayer[work->playerNo].comF) {
            break;
        }
        work->state = MP6_TELOP_STATE_OUT;
        work->timer = 0;
        if (work->telopNo >= MP6_TELOP_BOARD_NAME_FIRST) {
            work->timerMax = (work->playerNo < 0) ? 60 : 30;
        } else {
            work->timerMax = (work->playerNo < 0) ? 30 : 15;
            mbAudFXPlay(MP6_TELOP_SE_OUT);
        }
        break;

    case MP6_TELOP_STATE_OUT:
        if (++work->timer >= work->timerMax) {
            work->killF = TRUE;
        }
        t = (float)work->timer / (float)work->timerMax;
        espTPLvlSet(sprId, 1.0f - t);
        espScaleSet(sprId, 1.0f + t, 1.0f + t);
        break;

    default:
        break;
    }
}

void mbTelopCreate(s32 playerNo, s32 telopNo, BOOL stat)
{
    MP6_TELOP_WORK *work;
    OMOBJ *obj;
    s16 sprId;

    if (telopNo < 0 || telopNo >= (s32)(sizeof(mp6_telopFileTbl) / sizeof(mp6_telopFileTbl[0]))) {
        printf("[BOARD-TELOP] mbTelopCreate: telopNo %d out of range -- skipped\n",
               (int)telopNo);
        fflush(stdout);
        return;
    }

    obj = omAddObjEx(mbObjMan, MP6_TELOP_OM_PRIO, 1, 0, OM_GRP_NONE,
                     Mp6TelopInitOMExec);
    if (obj == NULL) {
        printf("[BOARD-TELOP] mbTelopCreate: no free OM object for telop %d\n",
               (int)telopNo);
        fflush(stdout);
        return;
    }
    mp6_telopOMObj = obj;
    omSetStatBit(obj, OM_STAT_MODELPAUSE);

    work = omObjGetWork(obj, MP6_TELOP_WORK);
    work->killF = FALSE;
    work->state = MP6_TELOP_STATE_IN;
    work->playerNo = (s8)playerNo;
    work->telopNo = (s8)telopNo;
    work->delay = (playerNo < 0 || GwPlayer[playerNo].comF) ? 30 : 0;

    sprId = espEntry(mbBoardDataNumGet(mp6_telopFileTbl[telopNo]), 100, 0);
    if (sprId < 0) {
        /* See the header note: retail would hand -1 straight to
         * espDrawNoSet.  Report it and drop the object instead. */
        printf("[BOARD-TELOP] mbTelopCreate: espEntry FAILED for telop %d "
               "(data 0x%08X) -- no logo this opening\n",
               (int)telopNo, (unsigned)mbBoardDataNumGet(mp6_telopFileTbl[telopNo]));
        fflush(stdout);
        mp6_telopOMObj = NULL;
        omDelObjEx(mbObjMan, obj);
        return;
    }
    obj->mdlId[0] = sprId;
    espDrawNoSet(sprId, MP6_TELOP_DRAW_NO);

    MP6_LOG_ONCE("BOARD-TELOP",
                 "mbTelopCreate -> real sprite (board name/logo telop)");
    printf("[BOARD-TELOP] telop=%d data=0x%08X spr=%d delay=%d\n",
           (int)telopNo, (unsigned)mbBoardDataNumGet(mp6_telopFileTbl[telopNo]),
           (int)sprId, (int)work->delay);
    fflush(stdout);

    if (telopNo < MP6_TELOP_BOARD_NAME_FIRST) {
        mbAudFXPlay(MP6_TELOP_SE_IN);
    }
    if (stat) {
        while (mp6_telopOMObj != NULL) {
            HuPrcVSleep();
        }
    }
}
