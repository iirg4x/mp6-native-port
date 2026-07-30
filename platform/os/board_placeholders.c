/* Native boundary placeholders for shared-board code that is still absent
 * from the clean, pinned marioparty6 main branch.
 *
 * These are deliberately kept out of the decomp tree: they are integration
 * seams, not recovered implementations.  Every entry logs its first call so
 * a placeholder can never masquerade as working board behavior.  Delete each
 * definition as soon as the same symbol lands on decomp main.
 *
 * The e32fa5e pin (Treetop board recovery: capevent.c, capselect.c,
 * capsule.c, config.c, scroll.c, single.c, snpc.c) retired 60 of the 72
 * entries this file used to carry.  45 of those the linker named directly as
 * duplicate symbols.  The other 15 -- CapAutoThrow, CapColCheck,
 * CapColMdlIdGet, CapPlayerThrow, CapSelectMasuAdd{Front,Back},
 * CapSelectMasuLinkCheck, InitScrollCol, MapSprCreate, MapViewExec,
 * PauseGuideMain, ScrollExec, ev_CapCoinAdd, ev_CapEffCreate,
 * ev_CapEffGridSet -- came back as `static` in their recovering TU, so they
 * never collided; they went because nothing in the decomp or in this port
 * references them any more, which makes the no-op unreachable rather than
 * merely redundant.  A placeholder that cannot be called is worse than
 * useless: it reports nothing and hides that the seam is closed.
 */
#include "dolphin.h"
#include "game/process.h"    /* HuPrcEnd -- see the return-contract note below */
#include "game/board/model.h" /* MB_MODEL_NONE -- shape 4 below hinges on this
                               * value being the decomp's OWN sentinel and not
                               * a -1 that merely looks like it, so it is
                               * included rather than re-spelled */
#include "mp6_board_compat.h" /* mp6_board_config_seam_done */
#include "mp6_shim_log.h"
#include "mp6_diag_probe.h"   /* the enumerable seam registry, below */

#include <stdio.h>

/* RETURN CONTRACTS -- what a no-op has to answer so its caller can leave.
 *
 * A placeholder is not "safe because it does nothing".  Every one of these
 * symbols is called by RECOVERED code that keeps running afterwards, and
 * some of that code cannot make progress -- or cannot even stay alive --
 * unless the callee answers a specific way.  FOUR failure shapes have been
 * found by reading every call site of every seam below in the pinned decomp
 * source:
 *
 *  1. PROCESS BODY THAT RETURNS.  A seam handed to HuPrcChildCreate is a
 *     HuPrc process entry point.  platform/os/process_native.c's
 *     ProcessTrampoline (lines 152-155) treats a process function that RETURNS
 *     instead of calling HuPrcEnd() as fatal: it prints "[FATAL]
 *     process_native: a HuPrc process function returned instead of calling
 *     HuPrcEnd() -- unsupported" and exit(1)s the game.  Two seams are used
 *     this way -- ConfigMain (config.c:140) and CapEffUse (capsule.c:1112) --
 *     so both must end with HuPrcEnd(), which also runs the destructor the
 *     creator installed, and it is those destructors the callers' wait loops
 *     are watching.
 *
 *  2. A RETURN VALUE THE CALLER LOOPS ON.  mbCapUse (capselect.c:164) is the
 *     documented instance: mbCapSelect's `for (;;)` only breaks when the use
 *     is accepted.
 *
 *  3. A FILE-STATIC FLAG ONLY THE SEAM WRITES.  mbConfigExec spins on
 *     configDoneF (config.c:143) and only ConfigMain sets it.  Nothing an
 *     out-of-line placeholder returns can help; that one needs the tiny
 *     bridge in patches/decomp/src/board/config.c.patch.
 *
 *  4. A HANDLE OR AN INDEX THE CALLER DEREFERENCES UNCHECKED.  This shape
 *     is not "the caller takes the wrong branch", it is memory-unsafe, and
 *     NO return value can close it: every value a handle seam can produce
 *     is either the "nothing" sentinel or a live slot number, and there is
 *     no third answer.  The caller is what has to be guarded, in a
 *     patches/decomp hunk carrying a `PORT SEAM GUARD` anchor that names
 *     the seam, this file, and the exact crash.
 *
 *     What makes this shape hard to SEE is that the sentinel usually does
 *     not announce itself at the indexing site.  zig cc's default
 *     -fsanitize=array-bounds only fires on a real ARRAY; the board's big
 *     tables (objManData, mbWinData) are heap POINTERS from mbMalloc, so
 *     index -1 there is a silent read/write in front of the allocation and
 *     the run walks on to fail somewhere else entirely.  Every reachable
 *     instance below is therefore recorded with BOTH the first unchecked
 *     use and the site that actually stops the process.
 *
 *       mbTelopTimeSprCreate -> -1 feeds HuSprGrpData[grpId] (sprman.c),
 *       which is indexed with no bounds test and WRITTEN through.  That one
 *       IS a real array (HUSPR_GROUP[256], sprite.h:111), so it traps at the
 *       first use -- "index -1 out of bounds".  Guarded in
 *       patches/decomp/src/board/pause.c.patch.
 *
 *       mbCapObjCreate -> MB_MODEL_NONE (-1) feeds objManData[modelId]
 *       (object.c:533; objManData is the heap pointer at object.c:20/54, so
 *       nothing traps there) and then mbCapObjKill(-1), which MATCHES the
 *       first idle capsuleObjData[] slot -- all eight are initialised to
 *       MB_MODEL_NONE at capsule.c:2592 -- takes the found-a-live-object
 *       branch at capsule.c:2929 and dereferences that slot's NULL .anim in
 *       HuSprAnimKill (sprman.c:412, `if(--anim->useNum <= 0)`).  A null
 *       member access IS trapped: hard panic, "member access within null
 *       pointer of type 'ANIMDATA'", reproduced 3 runs out of 3 on the plain
 *       retail board path the moment a player lands on a capsule space --
 *       mbev_MasuMove (masu.c:775) -> mbCapMasuExec (capselect.c:934) ->
 *       mbCapCapsuleGet (capselect.c:1100 create, :1132 kill).
 *
 *       mbCapMasuNextGet -> -1 is the same run's UPSTREAM half: it is the
 *       capsuleNo mbCapMasuExec then awards, and every read of a capsuleNo
 *       goes through mbCapValueTypeGet() == `value & 0xFF` (capsule.c:1830),
 *       so -1 becomes 255 and indexes capsuleData[] -- a real 60-entry
 *       static, capsule.c:325-386 -- out of bounds in mbCapUseMesGet
 *       (capsule.c:2443), which both branches of mbCapMasuExec reach
 *       (capselect.c:942 and :1001).
 *
 *     Both of the latter two are guarded in
 *     patches/decomp/src/board/capselect.c.patch.  They are NOT redundant,
 *     but they are also not independent the way this audit first claimed:
 *     mbCapCapsuleGet has exactly ONE caller (capselect.c:934), below the
 *     mbCapMasuNextGet guard, and the tutorial branch that would otherwise
 *     supply a real capsuleNo is dead on the pinned tree (nothing sets
 *     FLAG_BOARD_TUTORIAL, and mbTutorialCall returns -1 while it is clear,
 *     tutorial.c:277).  So the upstream guard is what closes the observed
 *     panic today, and the mbCapObjCreate guard is what stops it re-opening
 *     the moment mbCapMasuNextGet alone is recovered.  Recovering either
 *     seam without the other's guard is exactly the regression the
 *     tools/test_board_seam_contract.py entries exist to catch.
 *
 * WHICH SEAMS THIS AUDIT ACTUALLY HAS TO COVER.  61 placeholders live in
 * this file, but a seam that is never called cannot hurt anything, and the
 * shared-board flow wedges long before most of them are reachable.  The
 * live census comes from the runs' own [BOARD-PLACEHOLDER] lines rather
 * than from reading call graphs -- ten distinct seams have ever fired on
 * the retail Party-Mode board path:
 *
 *   board open / turn loop : mbev_MgCall, mbTelopTimeCreate,
 *                            mbTelopPlayerCreate
 *   landing on a capsule
 *   space                  : mbCapMasuNextGet, mbCapObjCreate
 *   pause -> Options       : ConfigMain, ConfigKill, ConfigSettingRead,
 *                            ConfigSettingWrite, mbTelopTimeSprCreate
 *
 * Of those ten, four are void no-ops with nothing to answer, three are
 * shape 1/3 (the Config trio), and three are shape 4 -- which is why this
 * audit's earlier claim that CapEffThrowCreate was the ONE open hazard was
 * wrong twice over: it missed the shape entirely, even though pause.c.patch
 * had already been written for it, and it declared THIS FILE's own
 * mbCapObjCreate/mbCapMasuNextGet entries "audited and left alone".
 *
 * The census is a floor, not the audit.  Reachability also has a structural
 * ROOT GATE that is worth writing down because it decides most of the rest
 * of the file: mbCapSelect (the capsule wheel) is called from player.c:664
 * only when `capsuleNum != 0`, i.e. only when the player already OWNS a
 * capsule -- and no capsule can be owned while mbCapMasuNextGet declines
 * every one.  Everything hanging off using or throwing a capsule (the wheel,
 * the description windows, the throw/auto-throw effect chain, the bonus-coin
 * chain) is therefore unreachable today and becomes reachable together, the
 * moment that one seam lands.
 *
 * SENTINEL AUDIT BEGIN -- one line per placeholder below whose "nothing"
 * answer is a NEGATIVE index or a NULL handle, because those are the only
 * two values that can collide with a live slot.  Columns are
 * <seam> : <verdict> : <evidence>.  Verdicts:
 *   GUARDED -- shape 4, closed caller-side by the named patches/decomp file.
 *   SAFE    -- the call sites either discard the value or already test it,
 *              or no call site exists in the pinned tree at all.
 *   LATENT  -- shape 4 as well, and NOT closed; unreachable today behind the
 *              root gate above.  The entry says what the guard would be, so
 *              the recovery that makes it reachable has it in hand.
 * tools/test_board_seam_contract.py derives the left column from this file's
 * own return statements and fails if a negative/NULL-returning seam has no
 * line here -- so a new seam cannot be added without a verdict.
 *
 *   mbTelopTimeSprCreate : GUARDED : pause.c.patch -- HuSprGrpData[-1]
 *       store, trapped; see shape 4 above.
 *   mbCapMasuNextGet : GUARDED : capselect.c.patch -- capsuleData[255]
 *       read, trapped; see shape 4 above.
 *   mbCapObjCreate : GUARDED : capselect.c.patch -- objManData[-1] silent,
 *       then the NULL ANIMDATA panic; see shape 4 above.
 *   mbCapSelectComGet : SAFE : capselect.c:616 reads -1 as "the COM
 *       declines", presses B and ends the wheel.  It is the caller's own
 *       no-content branch, not an index.
 *   mbev_CapCoinDisp : SAFE : its one call site discards the result
 *       (capevent.c:2527).
 *   mbev_CapEffCoinAdd : SAFE : capevent.c:2485 tests `coinNo >= 0` before
 *       using it to offset obj->data.
 *   mbev_CapEffGlowAdd : SAFE : both call sites discard the result
 *       (capsule.c:1417, :1675).
 *   mbev_CapPlayerComSelRandomGet : SAFE : no call site in the pinned tree.
 *       Its only readers, mbev_CapPlayerComSelGet/SameGet (capevent.c:2532,
 *       :2537), return it straight to callers that are not recovered yet --
 *       re-audit when they are, because -1 would be a GwPlayer[] index.
 *   mbCapDescWinCreate : LATENT : -1 goes into work->winId[1] and straight
 *       into mbWinPosGet (capselect.c:425-426), which is
 *       `&mbWinData[winNo]` with no test (window.c:427-429) -- a heap
 *       pointer, so silent -- and then into mbWinKill at capselect.c:507,
 *       which reads winP->proc off that same out-of-bounds slot and calls
 *       HuPrcKill on it (window.c:372-377).  This file's own idiom is
 *       already there to copy: capselect.c:517 and :555 test winId >= 0
 *       before touching it, and :324 seeds every slot with -1.  Guard =
 *       `if (work->winId[1] >= 0)` around the mbWinPosGet at :426 and the
 *       same test at :507.
 *   mbev_CapEffCoinCreate : LATENT : NULL, and the OMOBJ* tables it feeds
 *       hold the same collision the capsule objects did.  ev_CapCoinAdd
 *       dereferences obj->data at capevent.c:2512 (reached only with a
 *       non-zero coin count, which mbCapBonusCoinNumGet's 0 currently
 *       denies at capevent.c:1725), and mbev_CapEffCoinKill(NULL) MATCHES
 *       the first empty ev_CapEffCoinOMObj[] slot and poisons it with
 *       (OMOBJ *)-1 (capevent.c:3414-3419).  Guard = a NULL test at the two
 *       create sites (capevent.c:1764, capsule.c:1606).
 *   mbev_CapEffExplodeCreate : LATENT : NULL, and this one ends in a real
 *       array-bounds trap rather than a silent write.  capselect.c:258-266
 *       hands the NULL to mbev_CapEffExplodeAnimGet and then
 *       mbev_CapEffExplodeKill; each scans ev_CapEffExplodeOMObj[8]
 *       (capevent.c:53) for the pointer, matches the first NULL slot, and
 *       the kill stores (OMOBJ *)-1 into it (capevent.c:3360-3365).  After
 *       eight discards no slot is NULL any more, the scan leaves i == 8 and
 *       capevent.c:3378 indexes the array one past its end -- a real array,
 *       so trapped.  Guard = a NULL test around capselect.c:258-266.
 *   mbev_CapEffGlowCreate : LATENT : NULL into capsuleThrowGlowOMObj
 *       (capsule.c:1595); every reader is itself a seam today
 *       (mbev_CapEffGlowAdd), so nothing dereferences it YET.  Re-audit with
 *       those seams, not on its own.
 *   mbev_CapEffMasuHitCreate : LATENT : NULL into
 *       mbev_CapEffMasuHitTransformSet (capsule.c:1601-1604), which does
 *       `omObjGetDataAs(obj, ...)` on it unguarded (capevent.c:3749).
 *       Guard = the masuId test already wrapping it, extended with a NULL
 *       test on the object.
 *   mbev_CapEffRayCreate : LATENT : identical shape to MasuHitCreate --
 *       capsule.c:1597-1599 into mbev_CapEffRayTransformSet's
 *       omObjGetDataAs at capevent.c:3712.  Guard = the same NULL test on
 *       the object, alongside the masuId test already there.
 * SENTINEL AUDIT END
 *
 * The reachable seams audited CLEAN: mbev_MgCall's TRUE keeps board.c:425
 * out of HuPrcSleep(-1); the telop creates and the Config trio return
 * nothing at all (the Config trio's own hazards are shapes 1 and 3 above).
 *
 * ONE HAZARD IS STILL NOT CLOSED, and unlike shape 4 it cannot be closed by
 * guarding the caller either: CapEffThrowCreate.  capsule.c:1326-1328 is
 * `do { HuPrcVSleep(); } while (!CapEffThrowCheck(&pos, &work->maxTime));`
 * and CapEffThrowCheck returns FALSE while capEffThrowMdlId is
 * MB_MODEL_NONE (capsule.c:1479-1481).  capEffThrowMdlId is file-static in
 * capsule.c and CapEffThrowCreate is its only writer, so a human player's
 * capsule throw wedges CapPlayerThrow forever.  Unlike configDoneF, the
 * loop does not just want a flag: on the TRUE path CapEffThrowCheck hands
 * back the throw's landing position AND work->maxTime, which the rest of
 * CapPlayerThrow divides by.  Manufacturing those is inventing gameplay,
 * not reporting a no-content case, so this stays open and logged.  It has
 * not fired yet: reaching it needs a capsule in a player's inventory, and
 * mbCapMasuNextGet is still the seam that would have granted one. */

/* Call-count instrumentation.  MP6_LOG_ONCE alone answers "did this seam
 * fire at all", which is not enough to RANK the seams by gameplay impact:
 * a one-shot board-opening no-op and a per-frame no-op both print exactly
 * one line.  So each seam also carries its own counter and re-reports at
 * decade boundaries (1, 10, 100, 1000, ...).  That is O(1) per call, needs
 * no exit hook (the windowed runs are killed, so atexit is not reliable),
 * and bounds the log at ~log10(N) lines per seam. */
/* ...and each seam ALSO records itself in one flat table, so the set is
 * enumerable.  The counters themselves are function statics created by the
 * macro below -- there is no table anywhere -- which means the only surface
 * they have ever had is the decade-boundary line above, and a seam that fires
 * 40 times a second looks identical to one that fired twice at board open
 * until the third decade lands.  Ranking seams by gameplay impact needs the
 * whole set at once.
 *
 * The registry is filled HERE, in the one helper every BOARD_PLACEHOLDER_ONCE
 * site already calls, and nowhere else: this file's whole purpose is to be
 * deleted seam by seam as decomp main lands each symbol (see the header), and
 * an edit spread across 200 macro sites would fight that.  It stores the
 * caller's own `count` POINTER rather than a copy, so the table never has to
 * be kept in sync -- the value a reader sees is the live counter.
 *
 * The printf, its wording, and the decade throttle are unchanged, byte for
 * byte: existing logs and the board-seam contract test read against them. */
#define MP6_BOARD_SEAM_MAX MP6_DIAG_SEAM_MAX

static struct {
    const char          *symbol;
    const char          *result;
    const unsigned long *count;
} mp6_boardSeams[MP6_BOARD_SEAM_MAX];
static int mp6_boardSeamCount;

/* First call for a given site registers it.  `count` is the per-site static's
 * address, which is stable for the process lifetime, so identity by pointer is
 * exact and needs no string compare on the hot path. */
static void mp6_board_seam_record(const char *symbol, const char *result,
                                  const unsigned long *count)
{
    int i;
    for (i = 0; i < mp6_boardSeamCount; i++) {
        if (mp6_boardSeams[i].count == count) return;
    }
    if (mp6_boardSeamCount >= MP6_BOARD_SEAM_MAX) return;
    mp6_boardSeams[mp6_boardSeamCount].symbol = symbol;
    mp6_boardSeams[mp6_boardSeamCount].result = result;
    mp6_boardSeams[mp6_boardSeamCount].count = count;
    mp6_boardSeamCount++;
}

int mp6_diag_seam_count(void)
{
    return mp6_boardSeamCount;
}

int mp6_diag_seam(int index, const char **symbol, const char **result,
                  unsigned long *count)
{
    if (index < 0 || index >= mp6_boardSeamCount) return 0;
    if (symbol != NULL) *symbol = mp6_boardSeams[index].symbol;
    if (result != NULL) *result = mp6_boardSeams[index].result;
    if (count != NULL) *count = *mp6_boardSeams[index].count;
    return 1;
}

static void mp6_board_placeholder_hit(const char *symbol, const char *result,
                                      unsigned long *count, unsigned long *next)
{
    ++*count;
    if (*count == 1UL) {
        mp6_board_seam_record(symbol, result, count);
    }
    if (*count >= *next) {
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

void mbev_Last5(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_Last5", "no-op");
}

void mbTelopLastTurnCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbTelopLastTurnCreate", "no-op");
}

s32 mbev_MgCall(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_MgCall", "return TRUE (do not suspend board loop)");
    return TRUE;
}

/* Capsule events.  Private decomp structs are represented by ABI-equivalent
 * opaque pointers at this native boundary. */
void mbev_CapBiriQMetalShock(void *work)
{
    (void)work;
    BOARD_PLACEHOLDER_ONCE("mbev_CapBiriQMetalShock", "no-op");
}

void mbev_CapTeresaFadeCreate(s16 modelId)
{
    (void)modelId;
    BOARD_PLACEHOLDER_ONCE("mbev_CapTeresaFadeCreate", "no-op");
}

/* Minigame dispatch from the board turn loop. */
void mbev_MgCallSingleKoopa(s32 playerNo, BOOL koopa)
{
    (void)playerNo;
    (void)koopa;
    BOARD_PLACEHOLDER_ONCE("mbev_MgCallSingleKoopa", "no-op");
}

/* Telops.
 *
 * mbTelopCreate is NOT here: it graduated from "no-op" to a real behavioral
 * seam in platform/os/board_telop.c (see that file's header for what it is
 * and what it is not).  A no-op there is what made the board name/logo
 * overlay never appear during the board-opening flyover.  telop.c is
 * byte-identical between the f1fc94d and e32fa5e pins and still does not
 * define mbTelopCreate, so that seam stays open. */

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

void mbTelopTimeCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbTelopTimeCreate", "no-op");
}

void mbTelopPlayerCreate(s32 playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbTelopPlayerCreate", "no-op");
}

/* Shop. */
void ev_Shop(void *work)
{
    (void)work;
    BOARD_PLACEHOLDER_ONCE("ev_Shop", "no-op");
}

/* ------------------------------------------------------------------------
 * Seams OPENED by the e32fa5e recovery.
 *
 * Recovering a stub exposes its own callees.  The seven Treetop TUs land as
 * real bodies that call 49 routines the decomp declares but has not
 * recovered yet -- some as file-local `static` prototypes with no definition,
 * some as plain externs.  Either way the linker reports them undefined, and
 * the net placeholder count moves 72 -> 61, not 72 -> 12.
 *
 * Every signature below is taken from the decomp's OWN declaration of the
 * symbol (the static prototype at the head of its TU, or the extern in the
 * caller) -- nothing here is transcribed from a retail binary.  Struct
 * pointers are void* at this boundary, as above: the decomp types are
 * TU-private and all pointers share one ABI.  Return values are the
 * neutral "did nothing, carry on" answer for each contract, spelled out in
 * the log string so a placeholder can never be mistaken for real behavior.
 * ---------------------------------------------------------------------- */

/* capsule.c -- throw/trail/camera effect helpers. */
void CapEffCrackCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("CapEffCrackCreate", "no-op");
}

void CapEffThrowCreate(int playerNo, float *x, float *y, float *z, float yOfs,
    int masuId)
{
    (void)playerNo;
    (void)x;
    (void)y;
    (void)z;
    (void)yOfs;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("CapEffThrowCreate", "no-op");
}

int CapEffThrowMasu(int masuId, int capsuleNo, int playerNo, BOOL bonusF)
{
    (void)masuId;
    (void)capsuleNo;
    (void)playerNo;
    (void)bonusF;
    BOARD_PLACEHOLDER_ONCE("CapEffThrowMasu", "return 0 (no bonus coins)");
    return 0;
}

void CapEffThrowMasuCreate(int masuId, int capsuleNo)
{
    (void)masuId;
    (void)capsuleNo;
    BOARD_PLACEHOLDER_ONCE("CapEffThrowMasuCreate", "no-op");
}

void CapEffTrailAdd(void *pos, int capsuleNo)
{
    (void)pos;
    (void)capsuleNo;
    BOARD_PLACEHOLDER_ONCE("CapEffTrailAdd", "no-op");
}

void CapEffTrailCreate(int capsuleNo)
{
    (void)capsuleNo;
    BOARD_PLACEHOLDER_ONCE("CapEffTrailCreate", "no-op");
}

/* HuPrc process body (capsule.c:1112).  Returning from it is fatal in this
 * port (process_native.c:152-155), and its two waiters --
 * `while (mbCapEffUseModeGet(work->playerNo) >= 0)` at capevent.c:1517 and
 * capmove.c:25 -- are watching capsuleUseEffMode[playerNo], which the
 * destructor mbCapEffUseCreate installed (CapEffUseKill, capsule.c:811-820)
 * sets to -1.  Ending the process immediately therefore IS the caller's own
 * "the use effect is over" path; it just happens on the first tick. */
void CapEffUse(void)
{
    BOARD_PLACEHOLDER_ONCE("CapEffUse",
        "end the process at once -- CapEffUseKill sets capsuleUseEffMode=-1, "
        "which is what capevent.c:1517/capmove.c:25 wait for");
    HuPrcEnd();
}

void CapThrowCameraCalc(float t, float *x, float *y, float *z, void *out,
    int num)
{
    (void)t;
    (void)x;
    (void)y;
    (void)z;
    (void)out;
    (void)num;
    BOARD_PLACEHOLDER_ONCE("CapThrowCameraCalc", "no-op");
}

/* config.c -- the in-board options menu. */
void ConfigKill(void)
{
    BOARD_PLACEHOLDER_ONCE("ConfigKill", "no-op");
}

/* HuPrc process body (config.c:140) AND the sole writer of the flag its
 * creator waits on.  mbConfigExec is
 *
 *     configProc = HuPrcChildCreate(ConfigMain, ...);     config.c:140
 *     HuPrcDestructorSet2(configProc, ConfigKill);        config.c:142
 *     while (!configDoneF) { HuPrcVSleep(); }             config.c:143
 *     ConfigSettingWrite();
 *     return configResult;                                config.c:147
 *
 * and configDoneF is a file-static whose only assignments in the recovered
 * source are mbConfigExec's own `= FALSE` at config.c:132 and whatever the
 * unrecovered ConfigMain does.  A no-op here therefore parks pause.c:271's
 * `while (cancelF == 0 && result != 0)` loop forever the moment the player
 * opens pause -> Options: the board keeps rendering and never comes back.
 * (Returning without HuPrcEnd() is separately fatal -- process_native.c:152-155.)
 *
 * The graceful answer is the one mbConfigExec already wrote down for
 * itself: finish with configDoneF TRUE and configResult FALSE, i.e. "the
 * options screen is done and the player did not ask to quit", which is
 * exactly what pause.c:278 reads back as cancelF.  configResult is already
 * FALSE from config.c:133, so the bridge only has to raise the flag. */
void ConfigMain(void)
{
    BOARD_PLACEHOLDER_ONCE("ConfigMain",
        "close the Options screen at once -- configDoneF=TRUE so mbConfigExec "
        "(config.c:143) can return configResult=FALSE (do not quit the board)");
    mp6_board_config_seam_done();
    HuPrcEnd();
}

void ConfigSettingRead(void)
{
    BOARD_PLACEHOLDER_ONCE("ConfigSettingRead", "no-op");
}

void ConfigSettingWrite(void)
{
    BOARD_PLACEHOLDER_ONCE("ConfigSettingWrite", "no-op");
}

/* single.c -- solo-mode board setup and minigame dispatch. */
void SingleEffInit(void)
{
    BOARD_PLACEHOLDER_ONCE("SingleEffInit", "no-op");
}

void SingleFlagFlush(void)
{
    BOARD_PLACEHOLDER_ONCE("SingleFlagFlush", "no-op");
}

void SingleLast5(void)
{
    BOARD_PLACEHOLDER_ONCE("SingleLast5", "no-op");
}

void SingleMasuOrderInit(void)
{
    BOARD_PLACEHOLDER_ONCE("SingleMasuOrderInit", "no-op");
}

void SingleMgSaveInit(void)
{
    BOARD_PLACEHOLDER_ONCE("SingleMgSaveInit", "no-op");
}

void SingleMicCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("SingleMicCreate", "no-op");
}

void SingleMicListener(u16 *response)
{
    (void)response;
    BOARD_PLACEHOLDER_ONCE("SingleMicListener", "no-op");
}

void ev_SingleMg(int playerNo, int masuId)
{
    (void)playerNo;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("ev_SingleMg", "no-op");
}

void ev_SingleMgEnd(int playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("ev_SingleMgEnd", "no-op");
}

void ev_SingleKoopaMg(int playerNo, int masuId)
{
    (void)playerNo;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("ev_SingleKoopaMg", "no-op");
}

void ev_SingleKoopaMgEnd(int playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("ev_SingleKoopaMgEnd", "no-op");
}

void ev_SingleMKoopaMg(int playerNo, int masuId)
{
    (void)playerNo;
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("ev_SingleMKoopaMg", "no-op");
}

void ev_SingleMKoopaMgEnd(int playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("ev_SingleMKoopaMgEnd", "no-op");
}

/* capevent.c -- capsule event dispatch and its effect objects. */
void ev_CapCall(void *work, BOOL waitF)
{
    (void)work;
    (void)waitF;
    BOARD_PLACEHOLDER_ONCE("ev_CapCall", "no-op");
}

void ev_CapEffDraw(void *modelP, void *mtx)
{
    (void)modelP;
    (void)mtx;
    BOARD_PLACEHOLDER_ONCE("ev_CapEffDraw", "no-op");
}

void ev_CapWorkInit(void *work, int bgId)
{
    (void)work;
    (void)bgId;
    BOARD_PLACEHOLDER_ONCE("ev_CapWorkInit", "no-op");
}

int mbCapBonusCoinNumGet(int playerNo, int capsuleNo)
{
    (void)playerNo;
    (void)capsuleNo;
    BOARD_PLACEHOLDER_ONCE("mbCapBonusCoinNumGet", "return 0 (no bonus coins)");
    return 0;
}

s16 mbev_CapCoinDisp(int playerNo, int coinNum, BOOL winMotF, BOOL waitF)
{
    (void)playerNo;
    (void)coinNum;
    (void)winMotF;
    (void)waitF;
    BOARD_PLACEHOLDER_ONCE("mbev_CapCoinDisp", "return -1 (no display)");
    return -1;
}

int mbev_CapEffCoinAdd(void *obj, void *pos, void *vel, float scale,
    float gravity, int time, int arg)
{
    (void)obj;
    (void)pos;
    (void)vel;
    (void)scale;
    (void)gravity;
    (void)time;
    (void)arg;
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffCoinAdd", "return -1 (no particle)");
    return -1;
}

void *mbev_CapEffCoinCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffCoinCreate", "return NULL (no effect object)");
    return NULL;
}

void mbev_CapEffOpenCreate(int playerNo, int masuId, BOOL unk08, BOOL unk0C,
    BOOL unk10)
{
    (void)playerNo;
    (void)masuId;
    (void)unk08;
    (void)unk0C;
    (void)unk10;
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffOpenCreate", "no-op");
}

int mbev_CapPlayerComSelRandomGet(int playerNo, int selection, int *playerList,
    int playerNum)
{
    (void)playerNo;
    (void)selection;
    (void)playerList;
    (void)playerNum;
    BOARD_PLACEHOLDER_ONCE("mbev_CapPlayerComSelRandomGet", "return -1 (no target)");
    return -1;
}

BOOL mbev_CapPointCullCheck(void *pos)
{
    (void)pos;
    BOARD_PLACEHOLDER_ONCE("mbev_CapPointCullCheck", "return FALSE (not culled)");
    return FALSE;
}

/* The per-capsule dispatch table.  mbev_CapCall reads
 * `ev_CapsuleData[capsuleNo].unk08` and calls it only `if (event != NULL)`,
 * and tests `.unk18 != 0` behind its own guard, so an all-zero table is the
 * correct placeholder: every capsule's own event is skipped rather than
 * dispatched through a wild pointer.  Sized to match capsule.c's own
 * capsuleData[] (60 entries), which shares this capsuleNo index domain and
 * covers mbCapValidCheck's 0x35 upper bound with margin.  Layout mirrors
 * capevent.c's EVCAPSULEDATA field-for-field so the recovered accessors read
 * the offsets they expect under this port's 8-byte pointers. */
typedef struct Mp6EvCapsuleData {
    void (*main)(void);
    void (*unk04)(void);
    int unk08;
    int unk0C;
    int unk10;
    int bgDataNum;
    int unk18;
} MP6_EVCAPSULEDATA;

MP6_EVCAPSULEDATA ev_CapsuleData[60];

/* capselect.c -- capsule selection UI and its committed use. */
/* SHAPE 4, verdict LATENT (see the SENTINEL AUDIT table in the header): -1
 * lands in work->winId[1] and is indexed straight into mbWinData[] by
 * mbWinPosGet, then by mbWinKill.  Unreachable while no player can own a
 * capsule; the guard the table names has to land with whatever recovery
 * changes that. */
s16 mbCapDescWinCreate(int capsuleNo)
{
    (void)capsuleNo;
    BOARD_PLACEHOLDER_ONCE("mbCapDescWinCreate", "return -1 (no window)");
    return -1;
}

/* SHAPE 4 (see the header): the capsule this seam declines to name is
 * awarded unchecked by mbCapMasuExec, and -1 & 0xFF == 255 indexes the
 * 60-entry capsuleData[] out of bounds.  This is the guard that closes the
 * observed 3/3 board panic, because mbCapCapsuleGet -- where the
 * mbCapObjCreate hazard lives -- is only reached below it.  Guarded
 * caller-side in patches/decomp/src/board/capselect.c.patch. */
int mbCapMasuNextGet(int playerNo)
{
    (void)playerNo;
    BOARD_PLACEHOLDER_ONCE("mbCapMasuNextGet",
        "return -1 (no capsule) -- mbCapMasuExec's award path is guarded on "
        "this by patches/decomp/src/board/capselect.c.patch");
    return -1;
}

/* SHAPE 4 (see the header), and the only seam so far that panicked rather
 * than merely misbehaved: MB_MODEL_NONE is both "no object" and the idle
 * value of every capsuleObjData[] slot, so mbCapObjKill(-1) matches slot 0
 * and reaches a NULL ANIMDATA.  Spelled MB_MODEL_NONE rather than -1 because
 * the collision IS the bug -- the value is the right one to return, it is the
 * caller that cannot take it.  Guarded caller-side in
 * patches/decomp/src/board/capselect.c.patch; that guard is defensive today
 * (the mbCapMasuNextGet guard keeps mbCapCapsuleGet unreached) and becomes
 * load-bearing the moment mbCapMasuNextGet alone is recovered. */
int mbCapObjCreate(int capsuleNo, BOOL flag)
{
    (void)capsuleNo;
    (void)flag;
    BOARD_PLACEHOLDER_ONCE("mbCapObjCreate",
        "return MB_MODEL_NONE (no object) -- mbCapCapsuleGet is guarded on "
        "this by patches/decomp/src/board/capselect.c.patch");
    return MB_MODEL_NONE;
}

int mbCapSelectComGet(int playerNo, int *capsuleTbl, int capsuleNum)
{
    (void)playerNo;
    (void)capsuleTbl;
    (void)capsuleNum;
    BOARD_PLACEHOLDER_ONCE("mbCapSelectComGet", "return -1 (no selection)");
    return -1;
}

/* mbCapSelect's only exit for a HUMAN selection (capselect.c:143-179):
 *
 *     for (;;) {
 *         ...
 *         CapSelect(work);                       // sets ev_CapSelectValue
 *         if (ev_CapSelectValue[playerNo] >= 0) {
 *             if (mbCapUse(playerNo, ev_CapSelectValue[playerNo])) {
 *                 ev_CapSelectValue[playerNo] = -7;
 *                 ...
 *                 break;                         // capselect.c:176
 *             }
 *         } else { ... }
 *         HuPrcVSleep();                         // capselect.c:180
 *     }
 *
 * Pressing A on a capsule makes CapSelect store a real capsule number
 * (capselect.c:444), so the >= 0 branch is taken and a FALSE from mbCapUse
 * skips the break -- the wheel closes and immediately re-opens, every turn,
 * forever.  It yields, so the board stays live and this reads as a stuck
 * capsule menu rather than a freeze, which is exactly how the same shape in
 * DiceRun's `if (result <= 0) goto repeat;` (player.c:741) was first found.
 *
 * TRUE is the caller's own no-content path, not an invention: it sets
 * ev_CapSelectValue to -7, the same value CapSelectPadExec produces for a B
 * press (capselect.c:667) and for a COM that declines (capselect.c:616-617), and
 * -7 is what DiceRun's `case -7:` (player.c:733) turns into capsuleSkipF so
 * the turn moves on to the dice.  The capsule is simply not consumed, which
 * is the truth about what this placeholder did. */
int mbCapUse(int playerNo, int capsuleNo)
{
    (void)playerNo;
    (void)capsuleNo;
    BOARD_PLACEHOLDER_ONCE("mbCapUse",
        "return TRUE (accept and discard the selection) -- FALSE re-opens the "
        "capsule wheel forever at capselect.c:164");
    return TRUE;
}

void mbev_CapEffDustExplodeAdd(void *obj, void *pos)
{
    (void)obj;
    (void)pos;
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffDustExplodeAdd", "no-op");
}

void *mbev_CapEffExplodeCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffExplodeCreate", "return NULL (no effect object)");
    return NULL;
}

/* capsule.c -- glow/ray/hit effect objects and player motion sync. */
int mbev_CapEffGlowAdd(void *obj, void *pos, void *vel, int time, float scale,
    float gravity, float unk, void *color)
{
    (void)obj;
    (void)pos;
    (void)vel;
    (void)time;
    (void)scale;
    (void)gravity;
    (void)unk;
    (void)color;
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffGlowAdd", "return -1 (no particle)");
    return -1;
}

void *mbev_CapEffGlowCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffGlowCreate", "return NULL (no effect object)");
    return NULL;
}

void *mbev_CapEffMasuHitCreate(void)
{
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffMasuHitCreate", "return NULL (no effect object)");
    return NULL;
}

void *mbev_CapEffRayCreate(float scale, float speed)
{
    (void)scale;
    (void)speed;
    BOARD_PLACEHOLDER_ONCE("mbev_CapEffRayCreate", "return NULL (no effect object)");
    return NULL;
}

BOOL mbev_CapMasuMoveCheck(int masuId)
{
    (void)masuId;
    BOARD_PLACEHOLDER_ONCE("mbev_CapMasuMoveCheck", "return FALSE (space does not move)");
    return FALSE;
}

void mbev_CapPlayerMotShiftWait(int playerNo, int motNo, u32 attr, BOOL waitF)
{
    (void)playerNo;
    (void)motNo;
    (void)attr;
    (void)waitF;
    BOARD_PLACEHOLDER_ONCE("mbev_CapPlayerMotShiftWait", "no-op");
}
