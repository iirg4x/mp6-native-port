#include "mp6_rel_state.h"
#define MP6_BOOT_OPENING_BSS(X) \
    X(OpeningMdlId) \
    X(OpeningUnusedBss) \
    X(BookFrameBuf) \
    X(FilterFrameBuf) \
    X(BookFrameBufAnim) \
    X(OpeningCharAnim) \
    X(BookFrameBufAnimId) \
    X(OpeningCharAnimId) \
    X(BookPos) \
    X(BookOfs) \
    X(BookRot) \
    X(BookRadius) \
    X(OpeningWinId) \
    X(OpeningBgState) \
    X(GuideAngle) \
    X(OpeningBgRotZ) \
    X(StarPos) \
    X(FilterTPLvl) \
    X(CharLightFocusF) \
    X(CharFocusCancelF) \
    X(OpeningEndF) \
    X(FbCopyMode)
#define MP6_BOOT_OPENING_DATA(X) \
    X(WinFrameTypeTbl) \
    X(OpeningCharFileTbl) \
    X(OpeningCharTexNameTbl) \
    X(OpeningCharObjNameTbl) \
    X(BookDefPos) \
    X(BookDefRot) \
    X(OpeningBookFramePrimaryObjectName) \
    X(OpeningBookFrameSecondaryObjectName) \
    X(OpeningBackgroundRootObjectName)
MP6_REL_RESET_DEFINE(mp6_boot_opening_reset, MP6_BOOT_OPENING_BSS, MP6_BOOT_OPENING_DATA)
