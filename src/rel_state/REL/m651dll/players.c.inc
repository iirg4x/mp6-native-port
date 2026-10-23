#include "mp6_rel_state.h"
#define MP6_M651_PLAYERS_BSS(X) \
    X(lbl_1_bss_A8) \
    X(lbl_1_bss_64) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10)
#define MP6_M651_PLAYERS_DATA(X) \
    X(lbl_1_data_70) \
    X(lbl_1_data_88) \
    X(lbl_1_data_98) \
    X(lbl_1_data_9C) \
    X(lbl_1_data_A8) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_B0)
MP6_REL_RESET_DEFINE(mp6_m651_players_reset, MP6_M651_PLAYERS_BSS, MP6_M651_PLAYERS_DATA)
