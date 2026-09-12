/* Missing modules must return somewhere playable. This runs as a child of
 * the real overlay manager, after its prolog has finished assigning omcurdll.
 * Normal board minigames are skipped earlier by minigame_stub.c. */
#include "game/object.h"
#include "mp6_events.h"
#include "mp6_minigame.h"

static int returnPending;
int mp6_unavailable_overlay_take_returned(void)
{
    int pending = returnPending;
    returnPending = 0;
    return pending;
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
        return 1;
    default:
        return 0;
    }
}

void mp6_unavailable_overlay_return(void)
{
    int offset;
    returnPending = 1;
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
