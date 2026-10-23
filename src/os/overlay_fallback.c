/* Missing modules must return somewhere playable. This runs as a child of
 * the real overlay manager, after its prolog has finished assigning omcurdll.
 *
 * Board minigames use the game's own flow (board/mgcall.c -> instdll ->
 * minigame -> resultdll/instdll -> board), so a missing minigame called from
 * a board stands in for exactly that one game: it ends at once with nobody
 * scoring and returns one step, as MgSeq's close does for a real minigame.
 * The next overlay (results, or the duel/Bowser/DK hand-back through instdll)
 * then runs unchanged, keeping the board's turn save, graphs and battle pot.
 * Elsewhere (Mini-Game mode, Star Bank, direct test loads) it unwinds to the
 * nearest resumable menu in the history. */
#include "game/object.h"
#include "game/data.h"
#include "game/gamework.h"
#include "game/mgdata.h"
#include "mp6_events.h"
#include "mp6_minigame.h"

/* The notice for the player. The stub announces it on the black screen where
 * the game would have started, holds while the UI shows it, then returns.
 * Announcing after the return queued it behind instdll, the results and the
 * board reload, so it popped up over the next turn. */
static int noticePending;
static int noticeOpen;
int mp6_unavailable_overlay_take_returned(void)
{
    int pending = noticePending;
    noticePending = MP6_UNAVAILABLE_NONE;
    if (pending != MP6_UNAVAILABLE_NONE) noticeOpen = 1;
    return pending;
}

void mp6_unavailable_notice_closed(void)
{
    noticeOpen = 0;
}

static int supported(OMOVL overlay)
{
    switch (overlay) {
    /* Boot is one-shot initialization, not a resumable menu. */
    case DLL_selmenuDLL:
    case DLL_fileseldll:
    case DLL_mdseldll:
    case DLL_mdpartydll:
    case DLL_w01dll:
    case DLL_w03dll:
    case DLL_mdminidll:
    case DLL_mgmfreedll:
        return 1;
    default:
        return 0;
    }
}

static int board_overlay(OMOVL overlay)
{
    return (overlay >= DLL_w01dll && overlay <= DLL_w06dll) || overlay == DLL_w10dll ||
           overlay == DLL_w11dll || (overlay >= DLL_s01dll && overlay <= DLL_s03dll);
}

/* Called from a board: some history entry above the current one is a board
 * (the board stays in the history while instdll and the game run). */
static int called_from_board(void)
{
    for (int offset = 1; offset <= omovlhisidx && offset < 16; ++offset) {
        const OMOVLHIS *entry = omOvlHisGet(offset);
        if (entry && board_overlay(entry->ovl)) return 1;
    }
    return 0;
}

/* The result a game reports when nobody scores. omGameSysInit clears these
 * fields when a real minigame starts; a missing module never gets there, so
 * without this the previous minigame's result would be paid out again. All
 * zero is "nobody won" for results, duels (no winner) and Donkey Kong (no
 * coins). Bowser minigames count a bonus <= 0 as caught by Bowser, so there
 * every player escapes instead of everyone being punished. */
static void neutral_result(int minigame)
{
    const s16 survived = MgDataTbl[minigame].type == MG_TYPE_KUPA ? 1 : 0;
    for (int player = 0; player < GW_PLAYER_MAX; ++player) {
        GwPlayer[player].mgCoin = 0;
        GwPlayer[player].mgCoinBonus = survived;
        GwPlayer[player].mgScore = 0;
    }
}

void mp6_unavailable_overlay_announce(void)
{
    int minigame = MgNoGet(omcurovl);
    noticePending = minigame >= 0 && called_from_board() ? MP6_UNAVAILABLE_BOARD_MINIGAME
                                                         : MP6_UNAVAILABLE_RETURNED;
    noticeOpen = 0;
}

int mp6_unavailable_notice_pending(void)
{
    return noticePending != MP6_UNAVAILABLE_NONE;
}

int mp6_unavailable_notice_open(void)
{
    return noticeOpen;
}

void mp6_unavailable_overlay_return(void)
{
    int offset;
    int minigame = MgNoGet(omcurovl);
    /* Whatever the UI did not pick up must not surface later. */
    noticePending = MP6_UNAVAILABLE_NONE;
    noticeOpen = 0;
    /* Instructions preload the selected game's archive without the OVL tag.
     * A real minigame closes that archive after setup; a missing module never
     * reaches that owner. Release only its archive before returning, otherwise
     * browsing several unavailable games eventually exhausts HEAP_DVD. */
    if (minigame >= 0) HuDataDirClose(MgDataTbl[minigame].dataDir);
    if (minigame >= 0 && called_from_board()) {
        neutral_result(minigame);
        mp6_event_post("minigame.unavailable", omcurovl, "board");
        omOvlReturnEx(1, TRUE);
        return;
    }
    /* Skip other missing modules in the history (e.g. instruction -> game). */
    for (offset = 1; offset <= omovlhisidx && offset < 16; ++offset) {
        const OMOVLHIS *entry = omOvlHisGet(offset);
        if (entry && supported(entry->ovl)) {
            mp6_event_post("overlay.skipped", entry->ovl, "return");
            omOvlReturnEx(offset, TRUE);
            return;
        }
    }
    /* No usable history, including direct test launches. Reset the root so
     * a later Back cannot re-enter the missing module. */
    omovlhisidx = 0;
    omOvlHisChg(0, DLL_mdseldll, 0, 0);
    mp6_event_post("overlay.skipped", DLL_mdseldll, "mode select");
    omOvlGotoEx(DLL_mdseldll, TRUE, 0, 0);
}
