#include "mp6_rel_state.h"
#define MP6_MOTCHK_MOTCHK_BSS(X) \
    X(CameraObj) \
    X(OutViewObj) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_48) \
    X(TimingHookMode) \
    X(MotChkObjMan) \
    X(MotChkLightId) \
    X(CharMdlId) \
    X(CharMotId) \
    X(CharMotIdleId) \
    X(CharNo) \
    X(CharMotNo) \
    X(CharMdlNo)
#define MP6_MOTCHK_MOTCHK_DATA(X) \
    X(MotNameTbl) \
    X(CharDirTbl)
MP6_REL_RESET_DEFINE(mp6_motchk_motchk_reset, MP6_MOTCHK_MOTCHK_BSS, MP6_MOTCHK_MOTCHK_DATA)

void mp6_motchk_motchk_reset(void);
void mp6_motchk_reset(void)
{
    mp6_motchk_motchk_reset();
}
