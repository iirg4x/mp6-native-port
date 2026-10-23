#include "mp6_rel_state.h"
#define MP6_M658_GAMEPLAY_BSS(X) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M658_GAMEPLAY_DATA(X) \
    X(lbl_1_data_30) \
    X(lbl_1_data_34) \
    X(lbl_1_data_38) \
    X(lbl_1_data_7C) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_F4) \
    X(lbl_1_data_1A4) \
    X(lbl_1_data_1E4) \
    X(lbl_1_data_214) \
    X(lbl_1_data_230) \
    X(lbl_1_data_23C) \
    X(lbl_1_data_248) \
    X(lbl_1_data_24C) \
    X(lbl_1_data_258) \
    X(lbl_1_data_264) \
    X(lbl_1_data_26C) \
    X(lbl_1_data_44C) \
    X(lbl_1_data_494)
MP6_REL_RESET_DEFINE(mp6_m658_gameplay_reset, MP6_M658_GAMEPLAY_BSS, MP6_M658_GAMEPLAY_DATA)
