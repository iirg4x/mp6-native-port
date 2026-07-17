/* MP6 native port -- compat prelude force-included for
 * src/REL/selmenuDll/selmenu.c ONLY (see tools/build.py's REL_SOURCES
 * entry for it), on top of the global dolphin_compat.h.
 *
 * selmenu.c dereferences `se->comp` where `se` is declared `MSMSE *` --
 * the top-level include/msm.h's struct (msmSe_s, with a `comp` field).
 * But selmenu.c reaches game/msm.h first (via its own #include
 * "game/audio.h"), which -- sharing an include guard with the top-level
 * msm.h (see dolphin_compat.h) -- wins the race, so the only
 * `msmSeGetIndexPtr` prototype actually visible returns game/msm.h's OWN,
 * independent `MSM_SE` (game/msm_data.h), which has no `comp` field at
 * all ("no member named 'comp' in struct MSMSe_s").
 *
 * Defining this per-file (rather than in the global dolphin_compat.h)
 * matters: platform/null/shims_generated.c lives outside include/game/,
 * so ITS OWN "msm.h" reference resolves straight to the top-level header,
 * whose real `typedef struct msmSe_s {...} MSMSE;` would then collide
 * with a blanket compat definition of the same name. selmenu.c is the
 * ONLY user of the identifier `MSMSE` anywhere in this slice (verified:
 * grep -rl), so scoping the fix to just this one TU is both sufficient
 * and avoids that collision entirely.
 *
 * The struct fields are copied verbatim from include/msm.h's msmSe_s.
 * Real field values don't matter (msmSeGetIndexPtr is a null shim
 * returning 0 in both build modes) -- only the type needs to exist and
 * parse.
 */
#ifndef MP6_SELMENU_COMPAT_H
#define MP6_SELMENU_COMPAT_H

#include "dolphin_compat.h"
#include <dolphin/types.h>

typedef struct {
    u16 groupId;
    u16 fxId;
    s8 vol;
    s8 pan;
    s16 pitchBend;
    u8 span;
    u8 reverb;
    u8 chorus;
    u8 doppler;
    s8 comp;
    u8 pad[3];
} MP6MsmSeCompat;
#define MSMSE MP6MsmSeCompat

#endif /* MP6_SELMENU_COMPAT_H */
