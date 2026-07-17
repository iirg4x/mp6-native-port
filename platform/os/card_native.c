/* MP6 native port -- memory-card slot A via Aurora's CARD backend
 * (aurora lib/dolphin/card.cpp + lib/card/*: GCI-folder and raw-image
 * backends, BAT/directory/SRAM, async variants).
 *
 * The one real ABI seam: the GC SDK's CARDInit takes (void) -- the game
 * calls it exactly once at game/card.c:15 -- while aurora's CARDInit is an
 * extension taking (gameCode, makerCode) to tag/filter .gci files. This TU
 * interposes the game's call (dolphin_compat.h renames CARDInit ->
 * mp6_CARDInit for every decomp TU) and supplies the disc identity
 * "GP6E" / "01" plus the storage base path.
 *
 * Storage: <cwd>/saves, which aurora's GCI-folder backend (its default
 * mode) expands to saves/<REGION>/Card A/ per slot. Region resolves to USA
 * (aurora::g_gameName is normally filled by ITS dvd layer, which this port
 * replaces; the zeroed fallback hits the switch default -- "USA" -- which
 * is correct for GP6E01). Directories are pre-created here because the
 * backend is only promised an existing base path.
 *
 * Headless builds keep the honest no-card behavior end to end: this
 * interposer compiles to a no-op and shims_manual.c's no-card gate
 * (#ifdef MP6_HEADLESS_BUILD) answers CARD_RESULT_NOCARD.
 */
#include "mp6_shim_log.h"

#ifdef MP6_HEADLESS_BUILD

void mp6_CARDInit(void)
{
    MP6_LOG_ONCE("CARD", "mp6_CARDInit (headless: no-op, no-card gate active)");
}

#else /* aurora build: wire the real backend */

#include "host.h" /* mp6_host_save_dir / mp6_host_mkdir */
#include <stdio.h>

/* aurora lib/dolphin/card.cpp exports (extern "C" there). NOT taken from
 * the decomp's dolphin/card.h: CARDInit's signature is aurora's extension,
 * and pulling that header here would re-import the (void) prototype this
 * interposer exists to bridge. dolphin_compat.h (force-included into every
 * common-flavor TU, this one included) renames CARDInit to mp6_CARDInit for
 * the DECOMP's benefit -- undo that here so the forward below reaches
 * aurora's real symbol instead of recursing into ourselves. */
#undef CARDInit
extern void CARDSetBasePath(const char *path, const int chan);
extern void CARDInit(const char *game, const char *maker);

void mp6_CARDInit(void)
{
    static int done = 0;
    char saveDir[256];
    char sub[320];
    if (done) return; /* aurora's CARDInit self-guards too; keep the log clean */
    done = 1;

    /* Base path from the host seam -- mp6_host_save_dir returns the
     * cwd-relative "saves" on win32 (see host.h for why that resolution
     * must not change). The defensive fallback below cannot fire on win32
     * (the backend only fails on a too-small buffer). */
    if (mp6_host_save_dir(saveDir, sizeof(saveDir)) != 0) {
        snprintf(saveDir, sizeof(saveDir), "saves");
    }

    /* GCI-folder layout: saves/USA/Card A (slot B unused but harmless).
     * Directory-creation results deliberately ignored (existing dirs are
     * the common case). Separator is a macro: on win32 it must stay "\\"
     * (existing save trees were created with it); on posix '\\' is an
     * ordinary filename character, so android needs the real '/'. */
#ifdef _WIN32
#define MP6_CARD_SEP "\\"
#else
#define MP6_CARD_SEP "/"
#endif
    mp6_host_mkdir(saveDir);
    snprintf(sub, sizeof(sub), "%s" MP6_CARD_SEP "USA", saveDir);
    mp6_host_mkdir(sub);
    snprintf(sub, sizeof(sub), "%s" MP6_CARD_SEP "USA" MP6_CARD_SEP "Card A", saveDir);
    mp6_host_mkdir(sub);

    CARDSetBasePath(saveDir, -1);
    CARDInit("GP6E", "01");
    printf("[CARD] slot A wired to aurora GCI-folder backend at saves/USA/Card A\n");
    {
        /* self-check: print exactly what the game's HuCardSlotCheck will see */
        extern int CARDProbeEx(int chan, int *memSize, int *sectorSize);
        int ms = -999, ss = -999;
        int r = CARDProbeEx(0, &ms, &ss);
        printf("[CARD] self-probe slot A: result=%d memSize=%d sectorSize=%d\n", r, ms, ss);
    }
    fflush(stdout);
}

#endif /* MP6_HEADLESS_BUILD */
