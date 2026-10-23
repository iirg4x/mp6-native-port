#include "mp6_rel_state.h"
#define MP6_M613_PLAYER_BSS(X) \
    X(lbl_1_bss_11C) \
    X(lbl_1_bss_10C) \
    X(lbl_1_bss_FC) \
    X(lbl_1_bss_F8)
#define MP6_M613_PLAYER_DATA(X) \
    X(lbl_1_data_78) \
    X(lbl_1_data_8C) \
    X(lbl_1_data_BC) \
    X(lbl_1_data_EC) \
    X(lbl_1_data_FC)
MP6_REL_RESET_DEFINE(mp6_m613_player_reset, MP6_M613_PLAYER_BSS, MP6_M613_PLAYER_DATA)
