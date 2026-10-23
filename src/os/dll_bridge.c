/* MP6 native port -- REL "loader" bridge for the recovered menu/board DLLs,
 * PLUS the DVD entry points every OTHER path (real game data: fonts,
 * message tables, HSF models, sprites, ...) goes through.
 *
 * On real hardware, game/objdll.c's omDLLLink() reads a .rel FILE from DVD
 * (HuDvdDataReadDirect -> DVDOpen/DVDReadAsyncPrio) into memory, then calls
 * OSLink() to runtime-relocate it against the already-running DOL and
 * patch its OSModuleHeader.prolog/epilog fields from section-relative
 * offsets into absolute addresses, before finally calling
 * ((OMDLLPROLOG)module->prolog)().
 *
 * This port links the decompiled overlays' C sources DIRECTLY into the
 * game image instead of building real .rel binaries -- there is nothing
 * left to relocate, the linker already resolved everything. So this file
 * fakes just enough of that dance for game/objdll.c (unmodified decomp
 * code) to keep working as-is:
 *
 *   - DVDOpen() recognizes the known "dll/xxx.rel" paths (see
 *     include/ovl_table.h) and hands back a tiny synthetic file whose
 *     "length" is sizeof(OSModuleHeader) -- nothing else, since OSLink
 *     will be a no-op relocation (no sections to walk).
 *   - DVDReadAsyncPrio()/DVDReadPrio(), when the target file is one of
 *     those, fabricate a zeroed OSModuleHeader tagged with a sentinel ID
 *     identifying which DLL it is, instead of reading real disc bytes.
 *   - OSLink() recognizes the sentinel ID and writes the REAL, statically-
 *     linked (renamed -- see tools/build.py's per-TU -D_prolog=.../
 *     -D_epilog=... flags, which dodge the multiple-definition clash of
 *     every DLL otherwise defining `_prolog`/`_epilog`) function pointers
 *     into the header's .prolog/.epilog fields, exactly mimicking what a
 *     real relocation would have produced -- then returns TRUE.
 *
 * Every OTHER path resolves for real, over the extracted disc tree
 * (GP6E01/{sys/fst.bin,files/}) -- see src/dvd/dvd_files.c, which
 * owns the actual FST parsing/host-file I/O. This file checks the
 * synthetic paths FIRST, unconditionally, in every DVD entry point below;
 * only once a path/entrynum genuinely isn't one of those does it fall
 * through to mp6_dvd_*(). A path that's neither a synthetic DLL nor a real
 * FST entry still honestly returns "not found" -- game/dvd.c's
 * HuDvdDataReadWait callers hit their own OSPanic("File Open Error") when
 * that happens, which our hand-written OSPanic (src/null/
 * shims_manual.c) prints clearly and exits on, rather than hanging or
 * crashing obscurely.
 */
#include "dolphin.h"
#include "game/object.h"
#include "game/audio.h"
#include "game/process.h"
#include "game/mgdata.h"
#include "game/charman.h"
#include "game/board/main.h" /* forced board loads: party setup */
#include "game/flag.h"
#include "mp6_shim_log.h"
#include "mp6_dvd_files.h"
#include "mp6_boot.h"
#include "mp6_events.h" /* dll.bound game event, see mp6_dll_bridge_report_bound */
#include "mp6_minigame.h"
#include "mp6_gameplay_trace.h" /* MP6_TEST_FRAND_SEED */

/* Aurora's scene pipeline sets (include/aurora/gfx.h; headless: no-op stub
 * in src/null/shims_manual.c). Declared here because that header's C half
 * needs <stdbool.h>, the same seam console_stats.c uses. */
extern void aurora_set_pipeline_scene(uint32_t scene);

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

typedef void (*VoidFunc)(void);

/* The decompiled DLLs' renamed prolog/epilog (see tools/build.py) and
 * ctors/dtors arrays. On real hardware these are linker-synthesized
 * static-initializer tables and empty for this plain-C code, but a real REL
 * is also loaded fresh on every visit. A native overlay stays linked, so each
 * module's prolog loop (`for (ctor = _ctors; *ctor != 0; ctor++)`) runs its
 * src/rel_state reset (include/mp6_rel_state.h) to restore that. */
extern int bootDll_prolog(void);
extern void bootDll_epilog(void);
extern void mp6_boot_reset(void);
const VoidFunc bootDll_ctors[] = { mp6_boot_reset, 0 };
const VoidFunc bootDll_dtors[] = { 0 };

extern int selmenuDll_prolog(void);
extern void selmenuDll_epilog(void);
extern void mp6_selmenu_reset(void);
const VoidFunc selmenuDll_ctors[] = { mp6_selmenu_reset, 0 };
const VoidFunc selmenuDll_dtors[] = { 0 };

extern int fileselDll_prolog(void);
extern void fileselDll_epilog(void);
extern void mp6_filesel_reset(void);
const VoidFunc fileselDll_ctors[] = { mp6_filesel_reset, 0 };
const VoidFunc fileselDll_dtors[] = { 0 };

/* mdseldll -- mode select, the overlay file-select unconditionally
 * proceeds to (omOvlCallEx(0x5d, ...) = ovl_table.h index 93). Compiled
 * from the decomp's src/REL/mdseldll/mdsel.c and bound for real, exactly
 * like the other overlays; same -D rename scheme (tools/build.py
 * REL_SOURCES) for the shared _prolog/_epilog/_ctors/_dtors/ObjectSetup
 * names. */
extern int mdselDll_prolog(void);
extern void mdselDll_epilog(void);
extern void mp6_mdsel_reset(void);
const VoidFunc mdselDll_ctors[] = { mp6_mdsel_reset, 0 };
const VoidFunc mdselDll_dtors[] = { 0 };

/* mdpartydll -- party-mode setup, the overlay mode select's own "Party
 * Mode" confirm proceeds to (mdsel.c: omOvlCall(DLL_mdpartydll, 0, 0) =
 * ovl_table.h index 91). Compiled from the decomp's
 * src/REL/mdpartydll/{mdparty.c,stage.c} and bound for real, exactly like
 * the other overlays; same -D rename scheme (tools/build.py REL_SOURCES)
 * for the shared _prolog/_epilog/_ctors/_dtors/ObjectSetup names. See
 * this port's own board_stub.c placeholder. */
extern int mdpartyDll_prolog(void);
extern void mdpartyDll_epilog(void);
extern void mp6_mdparty_reset(void);
const VoidFunc mdpartyDll_ctors[] = { mp6_mdparty_reset, 0 };
const VoidFunc mdpartyDll_dtors[] = { 0 };

/* Recovered party-results lifecycle; no unavailable-overlay return. */
extern int mdpresultDll_prolog(void);
extern void mdpresultDll_epilog(void);
extern void mp6_mdpresult_reset(void);
const VoidFunc mdpresultDll_ctors[] = { mp6_mdpresult_reset, 0 };
const VoidFunc mdpresultDll_dtors[] = { 0 };

/* w01Dll -- Towering Treetop. The recovered world01.c is linked into the
 * native image and retains the real REL lifecycle: its prolog walks this
 * module-local ctor table and calls its renamed ObjectSetup, which registers
 * MB1_Create/MB1_Kill with the shared board runtime. */
extern int w01Dll_prolog(void);
extern void w01Dll_epilog(void);
extern void w01Dll_ObjectSetup(void);
extern void mp6_w01_reset(void);
const VoidFunc w01Dll_ctors[] = { mp6_w01_reset, 0 };
const VoidFunc w01Dll_dtors[] = { 0 };

/* Keep the recovered minigame/menu-mode lifecycle in one inventory. The
 * fields used by the runtime table are {path, prolog, epilog, name, id}; the
 * prefix and ctor initializer are only needed to emit the linker-facing REL
 * symbols. A row therefore cannot acquire a path, binding, or report-name
 * mismatch by being edited in only one of those three places. */
extern void mp6_mdmini_reset_data(void);
extern void mp6_m629_reset(void);
extern void mp6_m627_reset(void);
extern void mp6_m664_reset(void);
extern void mp6_m658_reset(void);
extern void mp6_m636_reset(void);
extern void mp6_m643_reset(void);
extern void mp6_m649_reset(void);
extern void mp6_m667_reset(void);
extern void mp6_m646_reset(void);
extern void mp6_m675_reset(void);
extern void mp6_mgmrensho_reset(void);
extern void mp6_m662_reset(void);
extern void mp6_m631_reset(void);
extern void mp6_m625_reset(void);
extern void mp6_m644_reset(void);
extern void mp6_m680_reset(void);
extern void mp6_m609_reset(void);
extern void mp6_m619_reset(void);
extern void mp6_m647_reset(void);
extern void mp6_m660_reset(void);
extern void mp6_m617_reset(void);
extern void mp6_m620_reset(void);
extern void mp6_m642_reset(void);
extern void mp6_m665_reset(void);
extern void mp6_m666_reset(void);
extern void mp6_m613_reset(void);
extern void mp6_m626_reset(void);
extern void mp6_m674_reset(void);
extern void mp6_m676_reset(void);
extern void mp6_mgmtournament_reset(void);
extern void mp6_ttwars_reset(void);
extern void mp6_w03_reset(void);
extern void mp6_w04_reset(void);
extern void mp6_m616_reset(void);
extern void mp6_m670_reset(void);
extern void mp6_m602_reset(void);
extern void mp6_m608_reset(void);
extern void mp6_m612_reset(void);
extern void mp6_m618_reset(void);
extern void mp6_m621_reset(void);
extern void mp6_m630_reset(void);
extern void mp6_m632_reset(void);
extern void mp6_m635_reset(void);
extern void mp6_m637_reset(void);
extern void mp6_m638_reset(void);
extern void mp6_m640_reset(void);
extern void mp6_m650_reset(void);
extern void mp6_m651_reset(void);
extern void mp6_m656_reset(void);
extern void mp6_m657_reset(void);
extern void mp6_m633_reset(void);
extern void mp6_m659_reset(void);
extern void mp6_m668_reset(void);
#define MP6_M616_CTORS { mp6_m616_reset, 0 }
#define MP6_M670_CTORS { mp6_m670_reset, 0 }
extern void mp6_inst_reset(void);
extern void mp6_mgmfree_reset(void);
#define MP6_INST_CTORS { mp6_inst_reset, 0 }
#define MP6_MGMFREE_CTORS { mp6_mgmfree_reset, 0 }
#define MP6_M602_CTORS { mp6_m602_reset, 0 }
#define MP6_M608_CTORS { mp6_m608_reset, 0 }
#define MP6_M612_CTORS { mp6_m612_reset, 0 }
#define MP6_M618_CTORS { mp6_m618_reset, 0 }
#define MP6_M621_CTORS { mp6_m621_reset, 0 }
#define MP6_M630_CTORS { mp6_m630_reset, 0 }
#define MP6_M632_CTORS { mp6_m632_reset, 0 }
#define MP6_M635_CTORS { mp6_m635_reset, 0 }
#define MP6_M637_CTORS { mp6_m637_reset, 0 }
#define MP6_M638_CTORS { mp6_m638_reset, 0 }
#define MP6_M640_CTORS { mp6_m640_reset, 0 }
#define MP6_M650_CTORS { mp6_m650_reset, 0 }
#define MP6_M651_CTORS { mp6_m651_reset, 0 }
#define MP6_M656_CTORS { mp6_m656_reset, 0 }
#define MP6_M657_CTORS { mp6_m657_reset, 0 }
#define MP6_M633_CTORS { mp6_m633_reset, 0 }
#define MP6_M659_CTORS { mp6_m659_reset, 0 }
#define MP6_M668_CTORS { mp6_m668_reset, 0 }
#define MP6_MD_MINI_CTORS { mp6_mdmini_reset_data, 0 }
#define MP6_M629_CTORS { mp6_m629_reset, 0 }
#define MP6_M627_CTORS { mp6_m627_reset, 0 }
#define MP6_M664_CTORS { mp6_m664_reset, 0 }
#define MP6_M658_CTORS { mp6_m658_reset, 0 }
#define MP6_M636_CTORS { mp6_m636_reset, 0 }
#define MP6_M643_CTORS { mp6_m643_reset, 0 }
#define MP6_M649_CTORS { mp6_m649_reset, 0 }
#define MP6_M667_CTORS { mp6_m667_reset, 0 }
#define MP6_M646_CTORS { mp6_m646_reset, 0 }
#define MP6_M675_CTORS { mp6_m675_reset, 0 }
#define MP6_MGMRENSHO_CTORS { mp6_mgmrensho_reset, 0 }
#define MP6_M662_CTORS { mp6_m662_reset, 0 }
#define MP6_M631_CTORS { mp6_m631_reset, 0 }
#define MP6_M625_CTORS { mp6_m625_reset, 0 }
#define MP6_M644_CTORS { mp6_m644_reset, 0 }
#define MP6_M680_CTORS { mp6_m680_reset, 0 }
#define MP6_M609_CTORS { mp6_m609_reset, 0 }
#define MP6_M619_CTORS { mp6_m619_reset, 0 }
#define MP6_M647_CTORS { mp6_m647_reset, 0 }
#define MP6_M660_CTORS { mp6_m660_reset, 0 }
#define MP6_M617_CTORS { mp6_m617_reset, 0 }
#define MP6_M620_CTORS { mp6_m620_reset, 0 }
#define MP6_M642_CTORS { mp6_m642_reset, 0 }
#define MP6_M665_CTORS { mp6_m665_reset, 0 }
#define MP6_M666_CTORS { mp6_m666_reset, 0 }
#define MP6_M613_CTORS { mp6_m613_reset, 0 }
#define MP6_M626_CTORS { mp6_m626_reset, 0 }
#define MP6_M674_CTORS { mp6_m674_reset, 0 }
#define MP6_M676_CTORS { mp6_m676_reset, 0 }
#define MP6_MGMTOURNAMENT_CTORS { mp6_mgmtournament_reset, 0 }
#define MP6_TTWARS_CTORS { mp6_ttwars_reset, 0 }
#define MP6_W03_CTORS { mp6_w03_reset, 0 }
#define MP6_W04_CTORS { mp6_w04_reset, 0 }
#define MP6_OWNER_CTORS(prefix) { mp6_##prefix##_reset, 0 }
#define MP6_COVERAGE_DLLS(X) \
    X(mdmic, "dll/mdmicdll.rel", mdmic_prolog, mdmic_epilog, "mdmicdll", MDMIC, MP6_OWNER_CTORS(mdmic)) \
    X(mdsing, "dll/mdsingdll.rel", mdsing_prolog, mdsing_epilog, "mdsingdll", MDSING, MP6_OWNER_CTORS(mdsing)) \
    X(mgmbattle, "dll/mgmbattledll.rel", mgmbattle_prolog, mgmbattle_epilog, "mgmbattledll", MGMBATTLE, MP6_OWNER_CTORS(mgmbattle)) \
    X(mgmbingo, "dll/mgmbingodll.rel", mgmbingo_prolog, mgmbingo_epilog, "mgmbingodll", MGMBINGO, MP6_OWNER_CTORS(mgmbingo)) \
    X(mgmdecathlon, "dll/mgmdecathlondll.rel", mgmdecathlon_prolog, mgmdecathlon_epilog, "mgmdecathlondll", MGMDECATHLON, MP6_OWNER_CTORS(mgmdecathlon)) \
    X(w02, "dll/w02dll.rel", w02_prolog, w02_epilog, "w02dll", W02, MP6_OWNER_CTORS(w02)) \
    X(w05, "dll/w05dll.rel", w05_prolog, w05_epilog, "w05dll", W05, MP6_OWNER_CTORS(w05)) \
    X(w06, "dll/w06dll.rel", w06_prolog, w06_epilog, "w06dll", W06, MP6_OWNER_CTORS(w06)) \
    X(w10, "dll/w10dll.rel", w10_prolog, w10_epilog, "w10dll", W10, MP6_OWNER_CTORS(w10)) \
    X(w11, "dll/w11dll.rel", w11_prolog, w11_epilog, "w11dll", W11, MP6_OWNER_CTORS(w11)) \
    X(s01, "dll/s01dll.rel", s01_prolog, s01_epilog, "s01dll", S01, MP6_OWNER_CTORS(s01)) \
    X(s02, "dll/s02dll.rel", s02_prolog, s02_epilog, "s02dll", S02, MP6_OWNER_CTORS(s02)) \
    X(s03, "dll/s03dll.rel", s03_prolog, s03_epilog, "s03dll", S03, MP6_OWNER_CTORS(s03)) \
    X(actman, "dll/actmandll.rel", actman_prolog, actman_epilog, "actmandll", ACTMAN, MP6_OWNER_CTORS(actman)) \
    X(ending, "dll/endingdll.rel", ending_prolog, ending_epilog, "endingdll", ENDING, MP6_OWNER_CTORS(ending)) \
    X(meschk, "dll/meschkdll.rel", meschk_prolog, meschk_epilog, "meschkdll", MESCHK, MP6_OWNER_CTORS(meschk)) \
    X(micquiz, "dll/micquizdll.rel", micquiz_prolog, micquiz_epilog, "micquizdll", MICQUIZ, MP6_OWNER_CTORS(micquiz)) \
    X(micquizsel, "dll/micquizseldll.rel", micquizsel_prolog, micquizsel_epilog, "micquizseldll", MICQUIZSEL, MP6_OWNER_CTORS(micquizsel)) \
    X(miraclebook, "dll/miraclebookdll.rel", miraclebook_prolog, miraclebook_epilog, "miraclebookdll", MIRACLEBOOK, MP6_OWNER_CTORS(miraclebook)) \
    X(motchk, "dll/motchkdll.rel", motchk_prolog, motchk_epilog, "motchkdll", MOTCHK, MP6_OWNER_CTORS(motchk)) \
    X(opening, "dll/openingdll.rel", opening_prolog, opening_epilog, "openingdll", OPENING, MP6_OWNER_CTORS(opening)) \
    X(option, "dll/optiondll.rel", option_prolog, option_epilog, "optiondll", OPTION, MP6_OWNER_CTORS(option)) \
    X(result, "dll/resultdll.rel", result_prolog, result_epilog, "resultdll", RESULT, MP6_OWNER_CTORS(result)) \
    X(saf, "dll/safdll.rel", saf_prolog, saf_epilog, "safdll", SAF, MP6_OWNER_CTORS(saf)) \
    X(sequence, "dll/sequencedll.rel", sequence_prolog, sequence_epilog, "sequencedll", SEQUENCE, MP6_OWNER_CTORS(sequence)) \
    X(staff, "dll/staffdll.rel", staff_prolog, staff_epilog, "staffdll", STAFF, MP6_OWNER_CTORS(staff))
#define MP6_DECLARE_COVERAGE_RESET(prefix, path, prolog, epilog, name, id, ctor_init) \
    extern void mp6_##prefix##_reset(void);
MP6_COVERAGE_DLLS(MP6_DECLARE_COVERAGE_RESET)
#undef MP6_DECLARE_COVERAGE_RESET

#define MP6_RECOVERED_DLLS(X) \
    X(w04, "dll/w04dll.rel", w04_prolog, w04_epilog, "w04dll", W04, MP6_W04_CTORS) \
    X(w03, "dll/w03dll.rel", w03_prolog, w03_epilog, "w03dll", W03, MP6_W03_CTORS) \
    X(ttwars, "dll/ttwarsdll.rel", ttwars_prolog, ttwars_epilog, "ttwarsdll", TTWARS, MP6_TTWARS_CTORS) \
    X(m616Dll, "dll/m616dll.rel", m616Dll_prolog, m616Dll_epilog, "m616Dll", M616, MP6_M616_CTORS) \
    X(m670Dll, "dll/m670dll.rel", m670Dll_prolog, m670Dll_epilog, "m670Dll", M670, MP6_M670_CTORS) \
    X(m602, "dll/m602dll.rel", m602_prolog, m602_epilog, "m602dll", M602, MP6_M602_CTORS) \
    X(m608, "dll/m608dll.rel", m608_prolog, m608_epilog, "m608dll", M608, MP6_M608_CTORS) \
    X(m612, "dll/m612dll.rel", m612_prolog, m612_epilog, "m612dll", M612, MP6_M612_CTORS) \
    X(m618, "dll/m618dll.rel", m618_prolog, m618_epilog, "m618dll", M618, MP6_M618_CTORS) \
    X(m621, "dll/m621dll.rel", m621_prolog, m621_epilog, "m621dll", M621, MP6_M621_CTORS) \
    X(m627, "dll/m627dll.rel", m627_prolog, m627_epilog, "m627dll", M627, MP6_M627_CTORS) \
    X(m629, "dll/m629dll.rel", m629_prolog, m629_epilog, "m629dll", M629, MP6_M629_CTORS) \
    X(m630, "dll/m630dll.rel", m630_prolog, m630_epilog, "m630dll", M630, MP6_M630_CTORS) \
    X(m631, "dll/m631dll.rel", m631_prolog, m631_epilog, "m631dll", M631, MP6_M631_CTORS) \
    X(m632, "dll/m632dll.rel", m632_prolog, m632_epilog, "m632dll", M632, MP6_M632_CTORS) \
    X(m635, "dll/m635dll.rel", m635_prolog, m635_epilog, "m635dll", M635, MP6_M635_CTORS) \
    X(m636, "dll/m636dll.rel", m636_prolog, m636_epilog, "m636dll", M636, MP6_M636_CTORS) \
    X(m637, "dll/m637dll.rel", m637_prolog, m637_epilog, "m637dll", M637, MP6_M637_CTORS) \
    X(m638, "dll/m638dll.rel", m638_prolog, m638_epilog, "m638dll", M638, MP6_M638_CTORS) \
    X(m640, "dll/m640dll.rel", m640_prolog, m640_epilog, "m640dll", M640, MP6_M640_CTORS) \
    X(m650, "dll/m650dll.rel", m650_prolog, m650_epilog, "m650dll", M650, MP6_M650_CTORS) \
    X(m651, "dll/m651dll.rel", m651_prolog, m651_epilog, "m651dll", M651, MP6_M651_CTORS) \
    X(m656, "dll/m656dll.rel", m656_prolog, m656_epilog, "m656dll", M656, MP6_M656_CTORS) \
    X(m657, "dll/m657dll.rel", m657_prolog, m657_epilog, "m657dll", M657, MP6_M657_CTORS) \
    X(m633, "dll/m633dll.rel", m633_prolog, m633_epilog, "m633dll", M633, MP6_M633_CTORS) \
    X(m659, "dll/m659dll.rel", m659_prolog, m659_epilog, "m659dll", M659, MP6_M659_CTORS) \
    X(m643, "dll/m643dll.rel", m643_prolog, m643_epilog, "m643dll", M643, MP6_M643_CTORS) \
    X(m646, "dll/m646dll.rel", m646_prolog, m646_epilog, "m646dll", M646, MP6_M646_CTORS) \
    X(m649, "dll/m649dll.rel", m649_prolog, m649_epilog, "m649dll", M649, MP6_M649_CTORS) \
    X(m658, "dll/m658dll.rel", m658_prolog, m658_epilog, "m658dll", M658, MP6_M658_CTORS) \
    X(m664, "dll/m664dll.rel", m664_prolog, m664_epilog, "m664dll", M664, MP6_M664_CTORS) \
    X(m667, "dll/m667dll.rel", m667_prolog, m667_epilog, "m667dll", M667, MP6_M667_CTORS) \
    X(m668, "dll/m668dll.rel", m668_prolog, m668_epilog, "m668dll", M668, MP6_M668_CTORS) \
    X(m675, "dll/m675dll.rel", m675_prolog, m675_epilog, "m675dll", M675, MP6_M675_CTORS) \
    X(m662, "dll/m662dll.rel", m662_prolog, m662_epilog, "m662dll", M662, MP6_M662_CTORS) \
    X(m625, "dll/m625dll.rel", m625_prolog, m625_epilog, "m625dll", M625, MP6_M625_CTORS) \
    X(m644, "dll/m644dll.rel", m644_prolog, m644_epilog, "m644dll", M644, MP6_M644_CTORS) \
    X(m680, "dll/m680dll.rel", m680_prolog, m680_epilog, "m680dll", M680, MP6_M680_CTORS) \
    X(m613, "dll/m613dll.rel", m613_prolog, m613_epilog, "m613dll", M613, MP6_M613_CTORS) \
    X(m626, "dll/m626dll.rel", m626_prolog, m626_epilog, "m626dll", M626, MP6_M626_CTORS) \
    X(m674, "dll/m674dll.rel", m674_prolog, m674_epilog, "m674dll", M674, MP6_M674_CTORS) \
    X(m676, "dll/m676dll.rel", m676_prolog, m676_epilog, "m676dll", M676, MP6_M676_CTORS) \
    X(m609, "dll/m609dll.rel", m609_prolog, m609_epilog, "m609dll", M609, MP6_M609_CTORS) \
    X(m619, "dll/m619dll.rel", m619_prolog, m619_epilog, "m619dll", M619, MP6_M619_CTORS) \
    X(m647, "dll/m647dll.rel", m647_prolog, m647_epilog, "m647dll", M647, MP6_M647_CTORS) \
    X(m660, "dll/m660dll.rel", m660_prolog, m660_epilog, "m660dll", M660, MP6_M660_CTORS) \
    X(m617, "dll/m617dll.rel", m617_prolog, m617_epilog, "m617dll", M617, MP6_M617_CTORS) \
    X(m620, "dll/m620dll.rel", m620_prolog, m620_epilog, "m620dll", M620, MP6_M620_CTORS) \
    X(m642, "dll/m642dll.rel", m642_prolog, m642_epilog, "m642dll", M642, MP6_M642_CTORS) \
    X(m665, "dll/m665dll.rel", m665_prolog, m665_epilog, "m665dll", M665, MP6_M665_CTORS) \
    X(m666, "dll/m666dll.rel", m666_prolog, m666_epilog, "m666dll", M666, MP6_M666_CTORS) \
    X(mdmini, "dll/mdminidll.rel", mdmini_prolog, mdmini_epilog, "mdminidll", MDMINI, MP6_MD_MINI_CTORS) \
    X(mgmfree, "dll/mgmfreedll.rel", mgmfree_prolog, mgmfree_epilog, "mgmfreedll", MGMFREE, MP6_MGMFREE_CTORS) \
    X(mgmrensho, "dll/mgmrenshodll.rel", mgmrensho_prolog, mgmrensho_epilog, "mgmrenshodll", MGMRENSHO, MP6_MGMRENSHO_CTORS) \
    X(mgmtournament, "dll/mgmtournamentdll.rel", mgmtournament_prolog, mgmtournament_epilog, "mgmtournamentdll", MGMTOURNAMENT, MP6_MGMTOURNAMENT_CTORS) \
    X(inst, "dll/instdll.rel", inst_prolog, inst_epilog, "instdll", INST, MP6_INST_CTORS) \
    MP6_COVERAGE_DLLS(X)

#define MP6_ENUM_RECOVERED_DLL(prefix, path, prolog, epilog, name, id, ctor_init) MP6_DLL_##id,
enum {
    MP6_DLL_NONE = 0,
    MP6_DLL_BOOT,
    MP6_DLL_SELMENU,
    MP6_DLL_FILESEL,
    MP6_DLL_MDSEL,
    MP6_DLL_MDPARTY,
    MP6_DLL_W01,
    MP6_DLL_MINIGAME_STUB,
    MP6_DLL_MDPRESULT,
    MP6_RECOVERED_DLLS(MP6_ENUM_RECOVERED_DLL)
};
#undef MP6_ENUM_RECOVERED_DLL

#define MP6_DECLARE_RECOVERED_DLL(prefix, path, prolog, epilog, name, id, ctor_init) \
    extern int prolog(void); \
    extern void epilog(void); \
    const VoidFunc prefix##_ctors[] = ctor_init; \
    const VoidFunc prefix##_dtors[] = { 0 };
MP6_RECOVERED_DLLS(MP6_DECLARE_RECOVERED_DLL)
#undef MP6_DECLARE_RECOVERED_DLL

typedef struct {
    const char *path;
    int (*prolog)(void);
    void (*epilog)(void);
    const char *name;
    int id;
} Mp6RecoveredDll;

#define MP6_RECOVERED_DLL_ROW(prefix, path, prolog, epilog, name, id, ctor_init) \
    { path, &prolog, &epilog, name, MP6_DLL_##id },
static const Mp6RecoveredDll g_recoveredDlls[] = {
    MP6_RECOVERED_DLLS(MP6_RECOVERED_DLL_ROW)
};
#undef MP6_RECOVERED_DLL_ROW

static int mp6_dll_name_equal(const char *left, const char *right)
{
    while (*left && *right &&
           tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
        ++left;
        ++right;
    }
    return *left == 0 && *right == 0;
}

static const Mp6RecoveredDll *mp6_recovered_dll_by_path(const char *path)
{
    size_t i;
    for (i = 0; i < sizeof(g_recoveredDlls) / sizeof(g_recoveredDlls[0]); i++) {
        if (mp6_dll_name_equal(path, g_recoveredDlls[i].path)) {
            return &g_recoveredDlls[i];
        }
    }
    return NULL;
}

static const Mp6RecoveredDll *mp6_recovered_dll_by_id(unsigned id)
{
    size_t i;
    for (i = 0; i < sizeof(g_recoveredDlls) / sizeof(g_recoveredDlls[0]); i++) {
        if ((unsigned)g_recoveredDlls[i].id == id) {
            return &g_recoveredDlls[i];
        }
    }
    return NULL;
}

/* Every not-yet-decompiled minigame DLL on the real disc. Matched by
 * FILENAME SHAPE ("m" + digits + "dll.rel", case-insensitive), not a
 * hardcoded list -- the real disc tree contains BOTH the contiguous
 * m601-m681/m699 range include/ovl_table.h's compiled-in table lists AND
 * 7 extra numeric IDs that exist as real disc files but are absent from
 * that table entirely (m433/m535/m541/m559/m562/m571/m580 -- presumably
 * loaded by some board's own direct filename construction rather than an
 * ovl_table.h lookup) -- a shape-based match handles either calling
 * convention identically, with no dependency on which one a given board
 * actually uses. Deliberately does NOT match "md*dll"/"mgm*dll"/"mic*dll"
 * (letters, not digits, right after the leading "m") -- those are
 * menu/quiz-select/etc. DLLs, a different family entirely, out of this
 * stub's scope (OSLink's catch-all below covers them instead). */
static BOOL is_minigame_stub_path(const char *path, char *nameOut, size_t nameOutSize)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t i;
    size_t digitEnd;
    size_t len = strlen(base);

    if (len < 2 || (base[0] != 'm' && base[0] != 'M')) {
        return FALSE;
    }
    i = 1;
    while (i < len && isdigit((unsigned char)base[i])) {
        i++;
    }
    digitEnd = i;
    if (digitEnd == 1) {
        return FALSE; /* no digits at all right after 'm' -- not this family */
    }
    /* Remainder must be "dll.rel" / "Dll.rel" / "DLL.rel" (case-insensitive). */
    if (len - digitEnd != 7) {
        return FALSE;
    }
    if (tolower((unsigned char)base[digitEnd + 0]) != 'd' || tolower((unsigned char)base[digitEnd + 1]) != 'l' ||
        tolower((unsigned char)base[digitEnd + 2]) != 'l' || base[digitEnd + 3] != '.' ||
        tolower((unsigned char)base[digitEnd + 4]) != 'r' || tolower((unsigned char)base[digitEnd + 5]) != 'e' ||
        tolower((unsigned char)base[digitEnd + 6]) != 'l') {
        return FALSE;
    }
    if (nameOut && nameOutSize > 0) {
        size_t copyLen = len < nameOutSize - 1 ? len : nameOutSize - 1;
        memcpy(nameOut, base, copyLen);
        nameOut[copyLen] = '\0';
    }
    return TRUE;
}

/* Set by synth_dll_for_path the instant a minigame-stub path is
 * recognized; read by mp6_minigame_stub_prolog (called later, with no
 * arguments of its own -- OMDLLPROLOG is BOOL(*)(void), matching every
 * other DLL's real prolog signature) so its banner can name the actual
 * requested file, not just "a minigame". Single-slot: this port loads at
 * most one DLL at a time in every reachable path so far (matching
 * MP6_DLL_BOOT/SELMENU/FILESEL's own one-at-a-time synthetic scheme). */
static char g_minigameStubName[64] = "(unknown)";

/* Defer the return until the overlay manager has finished starting this DLL.
 * A child must yield once; calling omOvlReturnEx from the prolog would tear
 * down an overlay before omcurdll has been assigned. */
static void mp6_minigame_stub_return_child(void)
{
    HuPrcVSleep();
    /* Tell the player here, on the black screen where the game would have
     * started. The UI takes the notice on its next frame and holds it open;
     * without a launcher UI (automation, straight boots) nothing takes it,
     * so give up after a few frames instead of waiting. */
    mp6_unavailable_overlay_announce();
    for (int frame = 0; frame < 4 && mp6_unavailable_notice_pending(); ++frame) HuPrcVSleep();
    while (mp6_unavailable_notice_open()) HuPrcVSleep();
    mp6_unavailable_overlay_return();
    /* The overlay manager normally deletes this child during the return. */
    for (;;) HuPrcVSleep();
}

static BOOL mp6_minigame_stub_prolog(void)
{
    HUPROCESS *returnProcess;
    printf("[STUB] module '%s' not yet available -- returning to the previous supported screen.\n",
           g_minigameStubName);
    fflush(stdout);
    mp6_dll_stub_black_screen_active = 1;
    returnProcess = HuPrcChildCreate(mp6_minigame_stub_return_child, 0x100, 0x1000, 0,
                            HuPrcCurrentGet());
    if (returnProcess == NULL) {
        /* Without this child omWatchOverlayProc's ChildWatch loop never
         * yields and pins the game thread at 100% CPU.  Report prolog
         * failure instead of claiming a usable placeholder. */
        mp6_dll_stub_black_screen_active = 0;
        fprintf(stderr, "[STUB] could not allocate the minigame placeholder process\n");
        return FALSE;
    }
    return TRUE;
}

static void mp6_minigame_stub_epilog(void)
{
    /* Drop the temporary black frame when the overlay manager returns. */
    printf("[STUB] minigame '%s' epilog -- clearing black-screen placeholder\n", g_minigameStubName);
    fflush(stdout);
    mp6_dll_stub_black_screen_active = 0;
}

/* Side table: DVDOpen tags a DVDFileInfo* with which synthetic DLL it is;
 * DVDReadAsyncPrio/DVDReadPrio/DVDClose consult it by pointer identity.
 * (DVDFileInfo has no spare field we can safely repurpose without risking
 * a real field game/dvd.c also reads, so a small side table is cleaner.) */
#define MP6_MAX_OPEN_SYNTH 8
static struct { DVDFileInfo *key; int dll; } g_openSynth[MP6_MAX_OPEN_SYNTH];

static int synth_dll_for_path(const char *path)
{
    if (strcmp(path, "dll/bootdll.rel") == 0) return MP6_DLL_BOOT;
    if (strcmp(path, "dll/selmenuDLL.rel") == 0) return MP6_DLL_SELMENU;
    if (strcmp(path, "dll/fileseldll.rel") == 0) return MP6_DLL_FILESEL;
    /* Exact path string per game/ovllist.c's own `DLL(name)` macro
     * expansion, `{ "dll/" #name ".rel", 0 }` of ovl_table.h's
     * DLL(mdseldll) entry -- all lowercase, unlike selmenuDLL above. */
    if (strcmp(path, "dll/mdseldll.rel") == 0) return MP6_DLL_MDSEL;
    /* Same DLL(name) macro expansion, ovl_table.h line 92 =
     * DLL(mdpartydll) = index 91 -- all lowercase again. */
    if (strcmp(path, "dll/mdpartydll.rel") == 0) return MP6_DLL_MDPARTY;
    if (strcmp(path, "dll/mdpresultdll.rel") == 0) return MP6_DLL_MDPRESULT;
    /* ovl_table.h's DLL(w01dll) expansion is exactly this lowercase path. */
    if (strcmp(path, "dll/w01dll.rel") == 0) return MP6_DLL_W01;
    /* mdseldll calls Star Bank through this overlay, but the native port has
     * no recovered mdbankdll implementation.  Route the missing module
     * through the existing unavailable-overlay lifecycle instead of letting
     * HuDvdDataReadDirect() panic on a real-FST miss.  Keep the requested path
     * in the shared stub label so the log names the rejected call, and do not
     * synthesize any Star Bank state or menu behavior here. */
    if (strcmp(path, "dll/mdbankdll.rel") == 0) {
        snprintf(g_minigameStubName, sizeof(g_minigameStubName), "%s", path);
        return MP6_DLL_MINIGAME_STUB;
    }
    {
        const Mp6RecoveredDll *recovered = mp6_recovered_dll_by_path(path);
        if (recovered != NULL) {
            return recovered->id;
        }
    }
    if (is_minigame_stub_path(path, g_minigameStubName, sizeof(g_minigameStubName))) {
        return MP6_DLL_MINIGAME_STUB;
    }
    return MP6_DLL_NONE;
}

static void synth_tag(DVDFileInfo *f, int dll)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_SYNTH; i++) {
        if (g_openSynth[i].key == NULL || g_openSynth[i].key == f) {
            g_openSynth[i].key = f;
            g_openSynth[i].dll = dll;
            return;
        }
    }
    fprintf(stderr, "[WARN] dll_bridge: synth-file table full, dropping tag for %p\n", (void *)f);
}

static int synth_lookup(DVDFileInfo *f)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_SYNTH; i++) {
        if (g_openSynth[i].key == f) return g_openSynth[i].dll;
    }
    return MP6_DLL_NONE;
}

static void synth_untag(DVDFileInfo *f)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_SYNTH; i++) {
        if (g_openSynth[i].key == f) {
            g_openSynth[i].key = NULL;
            g_openSynth[i].dll = MP6_DLL_NONE;
            return;
        }
    }
}

BOOL DVDOpen(char *fileName, DVDFileInfo *fileInfo)
{
    MP6_LOG_ONCE("DVD", "DVDOpen");
    memset(fileInfo, 0, sizeof(*fileInfo));
    {
        int dll = synth_dll_for_path(fileName);
        if (dll != MP6_DLL_NONE) {
            fileInfo->length = sizeof(OSModuleHeader);
            synth_tag(fileInfo, dll);
            printf("[BOOT] DVDOpen: synthetic REL bridge for \"%s\"\n", fileName);
            return TRUE;
        }
    }
    /* Real disc file, served over the extracted GP6E01/files/ tree -- see
     * src/dvd/dvd_files.c. Honestly returns FALSE ("not found") if
     * fileName isn't a real FST entry, or is one but the extracted file
     * is missing. */
    if (mp6_dvd_open_by_path(fileName, fileInfo)) {
        return TRUE;
    }
    printf("[BOOT] DVDOpen: \"%s\" not found (not a synthetic REL path, not a real FST entry)\n", fileName);
    return FALSE;
}

static void fill_fake_module_header(void *addr, s32 length, int dll)
{
    OSModuleHeader *hdr = (OSModuleHeader *)addr;
    if ((size_t)length < sizeof(OSModuleHeader)) {
        memset(addr, 0, (size_t)length);
        return;
    }
    memset(hdr, 0, sizeof(*hdr));
    hdr->info.id = (OSModuleID)(0xD11C0000u + (unsigned)dll);
    /* Everything else (numSections, bssSize, prolog/epilog offsets, ...)
     * stays 0 -- OSLink() below never walks sections, it just recognizes
     * info.id and writes the real function pointers directly. */
}

BOOL DVDReadAsyncPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
    MP6_LOG_ONCE("DVD", "DVDReadAsyncPrio");
    (void)prio;
    {
        int dll = synth_lookup(fileInfo);
        if (dll != MP6_DLL_NONE) {
            (void)offset;
            fill_fake_module_header(addr, length, dll);
            if (callback) callback(length, fileInfo);
            return TRUE;
        }
    }
    /* Real file, completes synchronously (see src/dvd/dvd_files.c's
     * header comment for why every reachable caller in this slice
     * tolerates that) -- the callback has already fired and `addr`
     * already holds the bytes by the time this returns. */
    if (mp6_dvd_read(fileInfo, addr, length, offset)) {
        if (callback) callback(length, fileInfo);
        return TRUE;
    }
    if (callback) callback(DVD_RESULT_FATAL_ERROR, fileInfo);
    return FALSE;
}

BOOL DVDReadPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, s32 prio)
{
    MP6_LOG_ONCE("DVD", "DVDReadPrio");
    (void)prio;
    {
        int dll = synth_lookup(fileInfo);
        if (dll != MP6_DLL_NONE) {
            (void)offset;
            fill_fake_module_header(addr, length, dll);
            return TRUE;
        }
    }
    return mp6_dvd_read(fileInfo, addr, length, offset);
}

BOOL DVDClose(DVDFileInfo *f)
{
    MP6_LOG_ONCE("DVD", "DVDClose");
    synth_untag(f);
    mp6_dvd_close(f); /* no-op if f isn't one of ITS open handles */
    return TRUE;
}

s32 DVDGetDriveStatus(void)
{
    MP6_LOG_ONCE("DVD", "DVDGetDriveStatus");
    /* A constant, never-changing "fine" status so game/dvd.c's
     * HuDvdErrorWatch() (which only acts on a CHANGE in status) never
     * fires any of its error branches. See this file's header comment. */
    return 0;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo *fileInfo)
{
    MP6_LOG_ONCE("DVD", "DVDFastOpen");
    memset(fileInfo, 0, sizeof(*fileInfo));
    /* Entrynum-only open (no path string) -- there's no synthetic-REL
     * path here at all (the fake DLLs are only ever opened by literal
     * path string, see DVDOpen above), so this is entirely real-FST
     * territory. entrynum comes from a prior real
     * DVDConvertPathToEntrynum() call the caller cached (e.g. game/data.c's
     * HuDataInit() populates DataDirStat[].entryNum this way). */
    return mp6_dvd_open_by_entrynum(entrynum, fileInfo);
}

s32 DVDCancel(volatile DVDCommandBlock *block)
{
    MP6_LOG_ONCE("DVD", "DVDCancel");
    (void)block;
    return 0;
}

BOOL DVDCancelAsync(DVDCommandBlock *block, DVDCBCallback callback)
{
    MP6_LOG_ONCE("DVD", "DVDCancelAsync");
    (void)block;
    (void)callback;
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock *block)
{
    MP6_LOG_ONCE("DVD", "DVDGetCommandBlockStatus");
    (void)block;
    return 0;
}

s32 DVDConvertPathToEntrynum(char *pathPtr)
{
    /* game/data.c's HuDataInit() calls this once per entry in a compiled-in
     * table of every data/*.bin path the game knows about (248 of them),
     * purely to sanity-check "does this path exist on the disc" up front:
     * on any -1 it OSPanics immediately, before main()'s loop ever reaches
     * omMasterInit or the first HuPrcCall tick. Resolution is real, against
     * the disc's own FST (src/dvd/dvd_files.c); every one of those 248
     * DATADIR() paths resolves against this disc's fst.bin, so HuDataInit's
     * panic-on-(-1) never fires on a complete extraction. A path that's
     * genuinely NOT on the disc still honestly returns -1, exactly like
     * real hardware would. */
    MP6_LOG_ONCE("DVD", "DVDConvertPathToEntrynum");
    return mp6_dvd_path_to_entrynum(pathPtr);
}

/* One place that both prints the existing "[BOOT] OSLink: bound X prolog/
 * epilog" line (unchanged wording -- docs/W01_INTEGRATION.md's pass oracle
 * greps for it verbatim) and publishes it as a `dll.bound` game event
 * (include/mp6_events.h). Posting from here rather than relying on
 * shims_manual.c's OSReport tap because these lines are printf, not
 * OSReport -- a direct post at the seam that already knows the fact is
 * both cheaper and impossible to desynchronise from the printed text. */
static void mp6_dll_bridge_report_bound(const char *moduleName)
{
    printf("[BOOT] OSLink: bound %s prolog/epilog\n", moduleName);
    mp6_event_post("dll.bound", 0, moduleName);
}

BOOL OSLink(OSModuleInfo *newModule, void *bss)
{
    MP6_LOG_ONCE("OS", "OSLink");
    (void)bss;
    /* Tell the renderer which scene (overlay) is loading: it builds the
     * shaders recorded for that scene now, on its compile threads, while the
     * scene loads -- before its first frames need them (unified pipeline
     * cache, compat/aurora/base/0088; docs/SHADER_CACHE.md). */
    aurora_set_pipeline_scene((u32)omcurovl);
    {
        OSModuleHeader *hdr = (OSModuleHeader *)newModule; /* info is the 1st member */
        unsigned dll = (unsigned)(hdr->info.id - 0xD11C0000u);
        const Mp6RecoveredDll *recovered = mp6_recovered_dll_by_id(dll);
        if (recovered != NULL) {
            hdr->prolog = (u32)(uintptr_t)recovered->prolog;
            hdr->epilog = (u32)(uintptr_t)recovered->epilog;
            mp6_dll_bridge_report_bound(recovered->name);
            return TRUE;
        }
        switch (dll) {
            case MP6_DLL_BOOT:
                hdr->prolog = (u32)(uintptr_t)&bootDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&bootDll_epilog;
                mp6_dll_bridge_report_bound("bootDll");
                return TRUE;
            case MP6_DLL_SELMENU:
                hdr->prolog = (u32)(uintptr_t)&selmenuDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&selmenuDll_epilog;
                mp6_dll_bridge_report_bound("selmenuDll");
                return TRUE;
            case MP6_DLL_FILESEL:
                hdr->prolog = (u32)(uintptr_t)&fileselDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&fileselDll_epilog;
                mp6_dll_bridge_report_bound("fileselDll");
                return TRUE;
            case MP6_DLL_MDSEL:
                /* Real mode select. The default: catch-all below still
                 * covers everything not decompiled (the mdXXXdll mode
                 * overlays this menu itself calls next, in particular). */
                hdr->prolog = (u32)(uintptr_t)&mdselDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&mdselDll_epilog;
                mp6_dll_bridge_report_bound("mdselDll");
                return TRUE;
            case MP6_DLL_MDPARTY:
                /* Real party-mode setup. The default: catch-all below
                 * still covers everything not decompiled (the board DLLs
                 * this setup flow itself calls next, in particular). */
                hdr->prolog = (u32)(uintptr_t)&mdpartyDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&mdpartyDll_epilog;
                mp6_dll_bridge_report_bound("mdpartyDll");
                return TRUE;
            case MP6_DLL_MDPRESULT:
                hdr->prolog = (u32)(uintptr_t)&mdpresultDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&mdpresultDll_epilog;
                mp6_dll_bridge_report_bound("mdpresultDll");
                return TRUE;
            case MP6_DLL_W01:
                hdr->prolog = (u32)(uintptr_t)&w01Dll_prolog;
                hdr->epilog = (u32)(uintptr_t)&w01Dll_epilog;
                mp6_dll_bridge_report_bound("w01Dll");
                return TRUE;
            case MP6_DLL_MINIGAME_STUB:
                hdr->prolog = (u32)(uintptr_t)&mp6_minigame_stub_prolog;
                hdr->epilog = (u32)(uintptr_t)&mp6_minigame_stub_epilog;
                printf("[BOOT] OSLink: bound minigame-stub prolog/epilog for '%s'\n", g_minigameStubName);
                mp6_event_post("dll.bound", 0, "minigame-stub");
                return TRUE;
            default:
                /* Catch-all for ANY module id this switch doesn't
                 * recognize: bind the same black-screen stub
                 * prolog/epilog as the minigame family. Without this, an
                 * unrecognized overlay's header would carry real,
                 * un-relocated disc bytes straight through as its
                 * prolog/epilog pointers -- a guaranteed wild jump the
                 * instant objdll.c calls
                 * `((OMDLLPROLOG)dll->module->prolog)()` (a raw,
                 * GC-address-shaped value used as an x86_64 function
                 * pointer). A generic catch-all covers every
                 * not-yet-decompiled DLL uniformly regardless of filename
                 * shape, instead of extending is_minigame_stub_path's
                 * name-pattern allowlist one name at a time. The adjacent,
                 * unmodified decomp OSReport (`objdll>Link DLL:%s`)
                 * already names the file in the log immediately before
                 * this line, so nothing is lost by this branch itself only
                 * logging the raw id. */
                fprintf(stderr, "[WARN] OSLink: unrecognized module id 0x%08x -- treating as a "
                        "not-yet-decompiled module, same graceful stub as the minigame family\n",
                        (unsigned)hdr->info.id);
                hdr->prolog = (u32)(uintptr_t)&mp6_minigame_stub_prolog;
                hdr->epilog = (u32)(uintptr_t)&mp6_minigame_stub_epilog;
                snprintf(g_minigameStubName, sizeof(g_minigameStubName), "id 0x%08x", (unsigned)hdr->info.id);
                return TRUE;
        }
    }
}

BOOL OSUnlink(OSModuleInfo *oldModule)
{
    MP6_LOG_ONCE("OS", "OSUnlink");
    (void)oldModule;
    return TRUE;
}

/* The storage behind mp6_boot.h's extern declaration -- read every frame
 * by src/gx/aurora_bridge.c's VIWaitForRetrace (Aurora build) to
 * force a black clear. Starts 0 (no effect) so every normal scenario
 * (opening/title/file-select) is completely unaffected; only a real (or
 * test-forced) minigame-stub prolog ever sets it. */
volatile int mp6_dll_stub_black_screen_active = 0;

/* Test-only forced-load hook -- see mp6_boot.h's own comment for the full
 * rationale. Accepts MP6_TEST_LOAD_DLL as a bare name ("m601Dll"), a name
 * with extension ("m601Dll.rel"), or a full path ("dll/m601Dll.rel") --
 * normalizes to the one shape synth_dll_for_path/DVDOpen actually expect
 * (a "dll/"-prefixed, ".rel"-suffixed path, matching every real caller in
 * this codebase, e.g. "dll/fileseldll.rel" above) so this exercises the
 * EXACT SAME code path a real (future, not-yet-decompiled) board's own
 * omDLLLink call would, not a parallel/simplified test-only shortcut. */
/* Exercise the real overlay manager, not a prolog called out of context. */
static OMOVL g_forcedTestOverlay;
extern s32 rnd_seed; /* game/main.c: rand8 state */
static void mp6_dll_bridge_selftest_teams(OMOVL overlay)
{
    /* Forced-load tests bypass Free Play's player/team picker. Derive valid
     * team slots from the game's metadata, never from a list of module IDs.
     * Normal menu launches do not call this fixture. */
    int game = MgNoGet(overlay);
    if (game < 0) return;
    int type = MgDataTbl[game].type;
    if (type != MG_TYPE_1VS3 && type != MG_TYPE_2VS2 && type != MG_TYPE_KETTOU) return;
    for (int player = 0; player < GW_PLAYER_MAX; ++player) {
        /* A duel has two participants, not three players sharing team 1.
         * Inactive slots must not create duplicate player/controller records. */
        GwPlayerConf[player].grpNo = type == MG_TYPE_KETTOU ? (player < 2 ? player : 2)
                                  : type == MG_TYPE_2VS2 ? player / 2 : player != 0;
    }
}

/* A forced board load skips Party Mode's setup, and the board quits straight
 * to the results screen without it. Set up a computer-only party on that board
 * first (the native-coverage probe's recipe), so tools that visit every
 * module -- tools/generate_shader_seed.py -- can play boards too. */
static void mp6_dll_bridge_selftest_board(OMOVL overlay)
{
    int board;
    switch (overlay) {
    case DLL_w01dll: board = 0; break;
    case DLL_w02dll: board = 1; break;
    case DLL_w03dll: board = 2; break;
    case DLL_w04dll: board = 3; break;
    case DLL_w05dll: board = 4; break;
    case DLL_w06dll: board = 5; break;
    case DLL_s01dll: board = 6; break;
    case DLL_s02dll: board = 7; break;
    case DLL_s03dll: board = 8; break;
    case DLL_w10dll: board = 9; break;
    case DLL_w11dll: board = 10; break;
    default: return;
    }
    mbSavePartyInit(FALSE, TRUE, 0, 20, 0, 0, 0, 0);
    GwSystem.boardNo = board;
    _ClearFlag(FLAG_BOARD_SAVEINIT);
    /* MP6_TEST_BOARD_QUICK=1: skip the board's opening (star intro, turn
     * order) the way the game's own board-debug flag does, so a test is
     * at the first turn within seconds. */
    if (getenv("MP6_TEST_BOARD_QUICK") != NULL) {
        _SetFlag(FLAG_BOARD_DEBUG);
    }
    /* MP6_TEST_BOARD_NIGHT=1: start at night, for boards whose events
     * differ by time of day (E. Gadd's Garage's capsule exchange). */
    if (getenv("MP6_TEST_BOARD_NIGHT") != NULL) {
        GwSystem.curTime = 1;
    }
    for (int player = 0; player < GW_PLAYER_MAX; ++player) {
        GwPlayer[player].charNo = GwPlayerConf[player].charNo;
        GwPlayer[player].padNo = GwPlayerConf[player].padNo;
        /* MP6_TEST_BOARD_HUMAN=1: player one is a person (so Start opens the
         * pause menu, which a computer-only party never allows). */
        GwPlayer[player].comF = !(player == 0 && getenv("MP6_TEST_BOARD_HUMAN") != NULL);
        /* Instructions, results and the minigame itself read the player
         * type from GwPlayerConf, as Party Mode's setup leaves it. Keep
         * both in step, or a computer board player is a human in its
         * minigame (and instructions wait for that pad's Start). */
        GwPlayerConf[player].type = GwPlayer[player].comF ? GW_PLAYER_TYPE_COM
                                                          : GW_PLAYER_TYPE_MAN;
        GwPlayer[player].comDif = 1;
        GwPlayer[player].team = 0;
    }
    /* MP6_TEST_BOARD_COINS=a,b,c,d: starting coins per player, so a test
     * can steer coin-ranked computer choices (duel opponents, steals). */
    const char *coins = getenv("MP6_TEST_BOARD_COINS");
    for (int player = 0; coins && *coins && player < GW_PLAYER_MAX; ++player) {
        GwPlayer[player].coin = (s16)atoi(coins);
        coins = strchr(coins, ',');
        if (coins) ++coins;
    }
}

int mp6_test_masu_type(int type)
{
    static int forced = -2;
    if (forced == -2) {
        const char *env = getenv("MP6_TEST_MASU_TYPE");
        forced = env && *env ? atoi(env) : -1;
    }
    return forced >= 0 ? forced : type;
}

/* A forced results or instructions load needs the party state a board leaves
 * behind (without it the results screen returns at once). MP6_TEST_MG_NO
 * picks the MgDataTbl entry (default 0, a 4-player game) and
 * MP6_TEST_RESULT_TEAMS=1 the team variant. */
static void mp6_dll_bridge_selftest_minigame_flow(OMOVL overlay)
{
    const char *mg = getenv("MP6_TEST_MG_NO");
    if (overlay == DLL_mdpresultdll) {
        /* The party results ceremony: bonus stars on, four distinct places
         * so every rank's exit (the winner's flight, the others' falls)
         * plays. */
        mbSavePartyInit(getenv("MP6_TEST_RESULT_TEAMS") != NULL, TRUE, 0, 20, 0, 0, 0, 0);
        for (int player = 0; player < GW_PLAYER_MAX; ++player) {
            GwPlayer[player].charNo = GwPlayerConf[player].charNo;
            GwPlayer[player].comF = TRUE;
            GwPlayerConf[player].type = GW_PLAYER_TYPE_COM;
            GwPlayer[player].team = player / 2;
            GwPlayer[player].star = 6 - player * 2;
            GwPlayer[player].coin = 10 + player * 7;
            /* One bonus star each for players 1-3 (minigame coins, orbs
             * used, ? spaces), so every award flies to a single winner. */
            GwPlayer[player].coinTotalMg = player == 0 ? 90 : 10;
            GwPlayer[player].capsuleUseNum = player == 1 ? 9 : 1;
            GwPlayer[player].hatenaMasuNum = player == 2 ? 9 : 1;
        }
        return;
    }
    if (overlay != DLL_resultdll && overlay != DLL_instdll) return;
    mbSavePartyInit(getenv("MP6_TEST_RESULT_TEAMS") != NULL, TRUE, 0, 20, 0, 0, 0, 0);
    GwSystem.mgNo = mg ? atoi(mg) : 0;
    for (int player = 0; player < GW_PLAYER_MAX; ++player) {
        GwPlayer[player].charNo = GwPlayerConf[player].charNo;
        /* Player one is a person, so the screen waits for A. */
        GwPlayer[player].comF = player != 0;
        GwPlayerConf[player].type = player ? GW_PLAYER_TYPE_COM : GW_PLAYER_TYPE_MAN;
        GwPlayer[player].team = player / 2;
        GwPlayer[player].coin = 10 + player * 7;
        GwPlayer[player].mgCoin = player == 1 ? 10 : 0;
    }
}

/* Every real route into a minigame (Mini-Game mode's menu, a board's
 * mgcall) first moves each player's character motion archive into ARAM.
 * Without that, CharMotionCreate reads the whole archive into HEAP_DVD and
 * keeps it there, and modules that create many motions for all four
 * players (Daft Rafts, Insectiride) exhaust the 5 MB heap at Extra Memory
 * x1. Do the same here before a forced minigame load. */
static void mp6_dll_bridge_selftest_char_motions(OMOVL overlay)
{
    if (MgNoGet(overlay) < 0) return;
    for (int player = 0; player < GW_PLAYER_MAX; ++player) {
        /* The menu copies the picks into GwPlayer before this, as here. */
        GwPlayer[player].charNo = GwPlayerConf[player].charNo;
        CharMotionInitAsync((s16)GwPlayer[player].charNo);
    }
}

/* MP6_TEST_CHARS=a,b,c,d: the four players' characters (CHARNO_* numbers),
 * so tools that record what a scene draws can cover every character (the
 * shader seed recorder rotates them across its runs). */
static void mp6_dll_bridge_selftest_chars(void)
{
    const char *chars = getenv("MP6_TEST_CHARS");
    for (int player = 0; chars && *chars && player < GW_PLAYER_MAX; ++player) {
        GwPlayerConf[player].charNo = (s16)atoi(chars);
        chars = strchr(chars, ',');
        if (chars) ++chars;
    }
}

/* MP6_TEST_ALL_COM=<0-3>: every minigame player is a computer at that
 * difficulty, so a forced minigame plays itself the same way each run.
 * MP6_TEST_FRAND_SEED=<n>: pin the shared random stream (otherwise seeded
 * from the clock) and rand8 as the minigame is called. Together they make a
 * forced minigame repeatable for tools/gameplay_view_check.py. */
static void mp6_dll_bridge_selftest_repeatable(OMOVL overlay)
{
    const char *com = getenv("MP6_TEST_ALL_COM");
    const char *seed = getenv("MP6_TEST_FRAND_SEED");
    if (com && *com && MgNoGet(overlay) >= 0) {
        for (int player = 0; player < GW_PLAYER_MAX; ++player) {
            GwPlayerConf[player].type = GW_PLAYER_TYPE_COM;
            GwPlayerConf[player].comDif = (s16)atoi(com);
            GwPlayer[player].comF = TRUE;
            GwPlayer[player].comDif = (s16)atoi(com);
        }
    }
    if (seed && *seed) {
        u32 value = (u32)strtoul(seed, NULL, 0);
        mp6_frand_seed_set(value ? value : 1);
        rnd_seed = (s32)value;
    }
}

static void mp6_dll_bridge_selftest_child(void)
{
    do { HuPrcVSleep(); } while (omcurovl == DLL_NONE || fadeStat);
    printf("[DLLSTUB-TEST] calling overlay %d through omOvlCallEx\n", (int)g_forcedTestOverlay);
    fflush(stdout);
    mp6_dll_bridge_selftest_chars();
    mp6_dll_bridge_selftest_minigame_flow(g_forcedTestOverlay);
    mp6_dll_bridge_selftest_board(g_forcedTestOverlay);
    mp6_dll_bridge_selftest_teams(g_forcedTestOverlay);
    mp6_dll_bridge_selftest_char_motions(g_forcedTestOverlay);
    mp6_dll_bridge_selftest_repeatable(g_forcedTestOverlay);
    if (!getenv("MP6_TEST_LOAD_DLL_HOLD")) {
        mp6_max_ticks = (int)mp6_tick_count + 600;
        mp6_ticks_unlimited = 0;
    }
    omOvlCallEx(g_forcedTestOverlay, TRUE, 0, 0);
    for (;;) HuPrcVSleep();
}

void mp6_dll_bridge_selftest_check_env(void)
{
    static int checked;
    const char *raw;
    char name[64];
    size_t length;
    int overlay;
    if (checked) return;
    raw = getenv("MP6_TEST_LOAD_DLL");
    if (!raw || !raw[0]) { checked = 1; return; }
    /* Do not interrupt the one-shot boot initialization. */
    if (omcurovl == DLL_NONE || omcurovl == DLL_bootdll) return;
    checked = 1;
    if (strncmp(raw, "dll/", 4) == 0) raw += 4;
    length = strlen(raw);
    if (length >= sizeof(name)) return;
    for (size_t i = 0; i <= length; ++i) name[i] = (char)tolower((unsigned char)raw[i]);
    if (length > 4 && strcmp(name + length - 4, ".rel") == 0) name[length - 4] = 0;
    for (overlay = 0; ; ++overlay) {
        const char *candidate = mp6_event_ovl_name(overlay);
        if (!candidate) break;
        if (mp6_dll_name_equal(candidate, name)) {
            g_forcedTestOverlay = (OMOVL)overlay;
            if (!HuPrcChildCreate(mp6_dll_bridge_selftest_child, 0x100, 0x2000, 0,
                                  HuPrcCurrentGet())) {
                fprintf(stderr, "[DLLSTUB-TEST] could not create test process\n");
            }
            return;
        }
    }
    fprintf(stderr, "[DLLSTUB-TEST] unknown overlay: %s\n", name);
}
