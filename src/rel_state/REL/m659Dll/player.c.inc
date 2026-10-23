#include "mp6_rel_state.h"
#define MP6_M659_PLAYER_BSS(X)
#define MP6_M659_PLAYER_DATA(X) \
    X(lbl_1_data_70) \
    X(lbl_1_data_90) \
    X(lbl_1_data_98) \
    X(lbl_1_data_A4) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_D4) \
    X(lbl_1_data_FC) \
    X(lbl_1_data_130)
MP6_REL_RESET_DEFINE(mp6_m659_player_reset, MP6_M659_PLAYER_BSS, MP6_M659_PLAYER_DATA)
