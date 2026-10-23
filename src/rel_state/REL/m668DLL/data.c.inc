#include "mp6_rel_state.h"
#define MP6_M668_DATA_BSS(X) \
    X(lbl_1_bss_644) \
    X(lbl_1_bss_640) \
    X(lbl_1_bss_340) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M668_DATA_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_3C) \
    X(lbl_1_data_64) \
    X(lbl_1_data_94) \
    X(lbl_1_data_AC)
MP6_REL_RESET_DEFINE(mp6_m668_data_reset, MP6_M668_DATA_BSS, MP6_M668_DATA_DATA)

void mp6_m668_reset(void)
{
    mp6_m668_data_reset();
}
