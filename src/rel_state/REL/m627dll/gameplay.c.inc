#include "mp6_rel_state.h"
#define MP6_M627_GAMEPLAY_BSS(X) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_8)
#define MP6_M627_GAMEPLAY_DATA(X) \
    X(lbl_1_data_30) \
    X(lbl_1_data_4C) \
    X(lbl_1_data_54) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_D40) \
    X(lbl_1_data_D44) \
    X(lbl_1_data_DBC) \
    X(lbl_1_data_DE0) \
    X(lbl_1_data_E04) \
    X(lbl_1_data_E10) \
    X(lbl_1_data_E1C) \
    X(lbl_1_data_E28) \
    X(lbl_1_data_E34) \
    X(lbl_1_data_E40) \
    X(lbl_1_data_E4C) \
    X(lbl_1_data_E58) \
    X(lbl_1_data_E88) \
    X(lbl_1_data_EB8) \
    X(lbl_1_data_ED4) \
    X(lbl_1_data_F1C) \
    X(lbl_1_data_F68) \
    X(lbl_1_data_F78) \
    X(lbl_1_data_F82)
MP6_REL_RESET_DEFINE(mp6_m627_gameplay_reset, MP6_M627_GAMEPLAY_BSS, MP6_M627_GAMEPLAY_DATA)
