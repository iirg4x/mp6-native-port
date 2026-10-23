#include "mp6_rel_state.h"
#define MP6_S02_S02_BSS(X) \
    X(s02Work) \
    X(lbl_1_bss_10) \
    X(s02MapScrollDelta) \
    X(lbl_1_bss_0)
#define MP6_S02_S02_DATA(X) \
    X(s02PairItemHookNameTbl) \
    X(lbl_1_data_20) \
    X(lbl_1_data_60) \
    X(lbl_1_data_6C) \
    X(lbl_1_data_78) \
    X(lbl_1_data_84) \
    X(lbl_1_data_88) \
    X(lbl_1_data_94) \
    X(lbl_1_data_A0) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_B4) \
    X(lbl_1_data_C8) \
    X(s02MapObjectInitialPositions) \
    X(lbl_1_data_178) \
    X(lbl_1_data_208) \
    X(lbl_1_data_220) \
    X(lbl_1_data_244) \
    X(lbl_1_data_24C) \
    X(lbl_1_data_254) \
    X(lbl_1_data_260) \
    X(lbl_1_data_278) \
    X(lbl_1_data_290)
MP6_REL_RESET_DEFINE(mp6_s02_s02_reset, MP6_S02_S02_BSS, MP6_S02_S02_DATA)

void mp6_s02_s02_reset(void);
void mp6_s02_reset(void)
{
    mp6_s02_s02_reset();
}
