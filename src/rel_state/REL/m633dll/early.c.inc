#include "mp6_rel_state.h"
#define MP6_M633_EARLY_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M633_EARLY_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_41) \
    X(lbl_1_data_5A) \
    X(lbl_1_data_73) \
    X(lbl_1_data_8C) \
    X(lbl_1_data_A5) \
    X(lbl_1_data_C0) \
    X(lbl_1_data_C1) \
    X(lbl_1_data_D2) \
    X(lbl_1_data_E3) \
    X(lbl_1_data_F4) \
    X(lbl_1_data_104) \
    X(lbl_1_data_11F) \
    X(lbl_1_data_136) \
    X(lbl_1_data_14F) \
    X(lbl_1_data_160) \
    X(lbl_1_data_177) \
    X(lbl_1_data_188) \
    X(lbl_1_data_1A0) \
    X(lbl_1_data_1B8) \
    X(lbl_1_data_1D0) \
    X(lbl_1_data_1E0) \
    X(lbl_1_data_220) \
    X(lbl_1_data_260) \
    X(lbl_1_data_270) \
    X(lbl_1_data_280)
MP6_REL_RESET_DEFINE(mp6_m633_early_reset, MP6_M633_EARLY_BSS, MP6_M633_EARLY_DATA)

void mp6_m633_reset(void)
{
    mp6_m633_early_reset();
}
