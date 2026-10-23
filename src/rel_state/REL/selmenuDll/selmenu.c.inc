#include "mp6_rel_state.h"
#define MP6_SELMENU_SELMENU_BSS(X) \
    X(smPage) \
    X(smCursorNoPrev) \
    X(smMainObj) \
    X(smOutViewObj) \
    X(smPlayerConf) \
    X(smCursorNo) \
    X(smPadBtnDown) \
    X(smPadDStk) \
    X(smPadDStkDown) \
    X(smPadBtnDownAll) \
    X(smPadDStkAll) \
    X(smPadDStkDownAll) \
    X(smCharMdlId) \
    X(smCharSelEndF) \
    X(smCharOnF) \
    X(smPlayerConfEditF) \
    X(smPlayerConfNo) \
    X(smPlayerConfChoiceNo) \
    X(smEmiCompDataNo) \
    X(smEmiCompVal) \
    X(smSound3DNo) \
    X(lbl_1_bss_0)
#define MP6_SELMENU_SELMENU_DATA(X) \
    X(smCharNameTbl) \
    X(smPageData) \
    X(smCharSelCamBitTbl) \
    X(lbl_1_data_844) \
    X(smDvdMusTbl) \
    X(smChar1Prev) \
    X(smPadDStkAllPrev) \
    X(smCharFileTbl) \
    X(smCharMotFileTbl) \
    X(smPlayerConfStrTbl)
MP6_REL_RESET_DEFINE(mp6_selmenu_selmenu_reset, MP6_SELMENU_SELMENU_BSS, MP6_SELMENU_SELMENU_DATA)

void mp6_selmenu_reset(void)
{
    mp6_selmenu_selmenu_reset();
}
