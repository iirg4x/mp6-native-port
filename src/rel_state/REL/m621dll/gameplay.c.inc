#include "mp6_rel_state.h"
#define MP6_M621_GAMEPLAY_BSS(X) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3E) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_3A) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_36) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M621_GAMEPLAY_DATA(X) \
    X(lbl_1_data_30) \
    X(lbl_1_data_3C) \
    X(lbl_1_data_64) \
    X(lbl_1_data_74) \
    X(lbl_1_data_8C) \
    X(lbl_1_data_9C) \
    X(lbl_1_data_BC) \
    X(lbl_1_data_258) \
    X(lbl_1_data_2AC) \
    X(lbl_1_data_2B0) \
    X(lbl_1_data_334) \
    X(lbl_1_data_344) \
    X(lbl_1_data_37E) \
    X(lbl_1_data_380)
MP6_REL_RESET_DEFINE(mp6_m621_gameplay_reset, MP6_M621_GAMEPLAY_BSS, MP6_M621_GAMEPLAY_DATA)
