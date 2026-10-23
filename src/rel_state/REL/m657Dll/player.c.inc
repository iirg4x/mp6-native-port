#include "mp6_rel_state.h"
#define MP6_M657_PLAYER_BSS(X) \
    X(lbl_1_bss_30)
#define MP6_M657_PLAYER_DATA(X) \
    X(lbl_1_data_D8) \
    X(lbl_1_data_100) \
    X(lbl_1_data_128) \
    X(lbl_1_data_134) \
    X(lbl_1_data_170) \
    X(lbl_1_data_172)
MP6_REL_RESET_DEFINE(mp6_m657_player_reset, MP6_M657_PLAYER_BSS, MP6_M657_PLAYER_DATA)
