/* MP6 native port -- explicit placeholder bridge for src/board/board.c's
 * mbSaveInit/mbSavePartyInit entry points, discovered as the real
 * undefined-symbol closure when wiring the fully-recovered mdpartydll
 * overlay in.
 *
 * WHY THIS FILE EXISTS, NOT gen_shims.py: these two symbols are NOT
 * Nintendo SDK surface (dolphin/msm) -- they are ordinary MP6 GAME code,
 * defined in `src/board/board.c` (board.c:734 and :775). On real
 * hardware mdpartydll.rel called them across the REL<->DOL boundary via
 * `src/game/kerent.c`'s kernel jump table (`_kerjmp_mbSaveInit` etc.);
 * this port skips kerent.c entirely (GAME_SKIP_LIST in tools/build.py --
 * "100% MWCC PPC inline asm, meaningless for a monolithic native link"),
 * so a plain extern call is what's left, and it needs a plain extern
 * definition somewhere.
 *
 * WHY A STUB, NOT THE REAL board.c BODY: board.c is a SEPARATE, still
 * actively-worked decompilation effort (main-DOL code, not a REL overlay --
 * configure.py keeps the whole file NonMatching; STATUS.md reports
 * individual functions including mbSaveInit itself as byte-exact
 * already, but siblings in the same TU -- mbObjectSetup/mbMain/
 * mbNextTime -- are not, and the file carries real internal dependencies
 * of its own, e.g. mbSaveInit calls mbPlayerHandicapGet/
 * mbMasuPlayerPrizeReset, themselves more board.c surface). Pulling any
 * of that in is integrating a DIFFERENT, much larger, ongoing recovery
 * effort -- explicitly out of scope for M14, whose mandate is mdpartydll
 * only ("do NOT try to recover downstream overlays -- that's the decomp
 * worker's job"). board.c is not in tools/build.py's game_sources() (it
 * only walks src/game/*.c, never src/board/) or any REL_SOURCES entry;
 * integrating it for real is future work for whichever effort eventually
 * owns board.c.
 *
 * WHY A STUB IS SAFE HERE: mdparty.c's own two callers (fn_1_718,
 * fn_1_E30 -- both reached only at the END of a full player-count /
 * character / rule / difficulty / board-select UI flow the player
 * actually drives through mdpartydll's OWN recovered, rendered screens)
 * already write every GwSystem/GwPlayer/GwPlayerConf field mdparty.c
 * itself reads, directly, before either call. The real mbSaveInit/
 * mbSavePartyInit additionally touch a handful of GwSystem fields
 * (partyF, storyComDif, curTime, boardWork[], a few flag bits) and
 * GwPlayer[].star/coin/... resets that only board.c's OWN other
 * functions (mbMain, the per-tile "masu" logic, ...) would ever read --
 * and none of that is compiled into this port either. The very next
 * step after these two calls is `fn_1_109C`'s
 * `omOvlGotoEx(boardDll[GwSystem.boardNo], ...)` -- itself still an
 * undecompiled w0Xdll overlay, already caught by dll_bridge.c's existing
 * OSLink catch-all stub. So this placeholder's blast radius is zero
 * observable behavior in this port today; it exists purely so the link
 * succeeds and mdpartydll's OWN substantial, real, recovered UI flow can
 * run and be exercised end to end, the same way an undecompiled overlay
 * gets a black-screen stub instead of blocking the whole executable.
 *
 * Exactly 2 symbols -- the complete board.c surface mdpartydll's
 * recovered source calls (verified: grep across mdparty.c + stage.c for
 * any other `mb`-prefixed extern turns up nothing else). Delete this
 * file the moment board.c itself is integrated into the port and let the
 * real definitions take over -- the two signatures above must then match
 * board.c's real ones exactly (mbSavePartyInit's board.c-side parameter
 * name is itself a stale/misleading "boardNo" -- the real first argument
 * at mdparty.c's call site is GwSystem.tagF, not a board number; kept as
 * the type-correct, unnamed-per-header extern shape here).
 */
#include "dolphin.h"
#include "mp6_shim_log.h"

#include <stdio.h>

void mbSaveInit(s32 boardNo)
{
    printf("[STUB] mbSaveInit(boardNo=%d) -- src/board/board.c is not yet integrated into this "
           "port (see platform/os/board_stub.c) -- no-op.\n", (int)boardNo);
    fflush(stdout);
}

void mbSavePartyInit(s32 tagF, s32 bonusStarF, s32 mgPack, s32 turnMax,
                      s32 handicapP1, s32 handicapP2, s32 handicapP3, s32 handicapP4)
{
    printf("[STUB] mbSavePartyInit(tagF=%d, bonusStarF=%d, mgPack=%d, turnMax=%d, "
           "handicap=%d/%d/%d/%d) -- src/board/board.c is not yet integrated into this port "
           "(a separate, not-yet-integrated effort) -- no-op.\n",
           (int)tagF, (int)bonusStarF, (int)mgPack, (int)turnMax,
           (int)handicapP1, (int)handicapP2, (int)handicapP3, (int)handicapP4);
    fflush(stdout);
}
