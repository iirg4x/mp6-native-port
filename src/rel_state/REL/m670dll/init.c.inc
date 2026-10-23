#include "mp6_rel_state.h"
#define MP6_M670_INIT_BSS(X) \
    X(lbl_1_bss_10)
#define MP6_M670_INIT_DATA(X) \
    X(lbl_1_data_78) \
    X(lbl_1_data_90) \
    X(lbl_1_data_F0) \
    X(lbl_1_data_104) \
    X(lbl_1_data_110)
MP6_REL_RESET_DEFINE(mp6_m670_init_reset, MP6_M670_INIT_BSS, MP6_M670_INIT_DATA)
