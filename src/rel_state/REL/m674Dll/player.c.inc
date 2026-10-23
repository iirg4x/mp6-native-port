#include "mp6_rel_state.h"
#define MP6_M674_PLAYER_BSS(X) \
    X(lbl_1_bss_30C) \
    X(lbl_1_bss_2FC) \
    X(lbl_1_bss_2E8) \
    X(lbl_1_bss_2E4) \
    X(lbl_1_bss_2E0)
#define MP6_M674_PLAYER_DATA(X) \
    X(lbl_1_data_D8) \
    X(lbl_1_data_F8) \
    X(lbl_1_data_158) \
    X(lbl_1_data_188) \
    X(lbl_1_data_198) \
    X(lbl_1_data_1C8) \
    X(lbl_1_data_1D8) \
    X(lbl_1_data_1E4) \
    X(lbl_1_data_240)
MP6_REL_RESET_DEFINE(mp6_m674_player_reset, MP6_M674_PLAYER_BSS, MP6_M674_PLAYER_DATA)
