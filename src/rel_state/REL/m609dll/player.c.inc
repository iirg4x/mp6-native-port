#include "mp6_rel_state.h"
#define MP6_M609_PLAYER_BSS(X) \
    X(lbl_1_bss_6E50) \
    X(lbl_1_bss_6E28)
#define MP6_M609_PLAYER_DATA(X) \
    X(lbl_1_data_3F8) \
    X(lbl_1_data_43C) \
    X(lbl_1_data_49C) \
    X(lbl_1_data_4CC) \
    X(lbl_1_data_4FC) \
    X(lbl_1_data_52C) \
    X(lbl_1_data_55C) \
    X(lbl_1_data_58C) \
    X(lbl_1_data_5A4) \
    X(lbl_1_data_5BC) \
    X(lbl_1_data_604)
MP6_REL_RESET_DEFINE(mp6_m609_player_reset, MP6_M609_PLAYER_BSS, MP6_M609_PLAYER_DATA)
