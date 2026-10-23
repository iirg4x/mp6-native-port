#include "mp6_rel_state.h"
#define MP6_M642_MAIN_BSS(X) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M642_MAIN_DATA(X) \
    X(lbl_1_data_30) \
    X(lbl_1_data_D8) \
    X(lbl_1_data_F4) \
    X(lbl_1_data_104) \
    X(lbl_1_data_113) \
    X(lbl_1_data_124) \
    X(lbl_1_data_12C) \
    X(lbl_1_data_30C) \
    X(lbl_1_data_318) \
    X(lbl_1_data_324) \
    X(lbl_1_data_32C) \
    X(lbl_1_data_338) \
    X(lbl_1_data_344) \
    X(lbl_1_data_350) \
    X(lbl_1_data_35C) \
    X(lbl_1_data_368) \
    X(lbl_1_data_374) \
    X(lbl_1_data_3B4) \
    X(lbl_1_data_3C0) \
    X(lbl_1_data_3CC) \
    X(lbl_1_data_450) \
    X(lbl_1_data_488) \
    X(lbl_1_data_490) \
    X(lbl_1_data_498)
MP6_REL_RESET_DEFINE(mp6_m642_main_reset, MP6_M642_MAIN_BSS, MP6_M642_MAIN_DATA)
