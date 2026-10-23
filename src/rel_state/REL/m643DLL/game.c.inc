#include "mp6_rel_state.h"
#define MP6_M643_GAME_BSS(X) \
    X(lbl_1_bss_174) \
    X(lbl_1_bss_16C) \
    X(lbl_1_bss_15C) \
    X(lbl_1_bss_154) \
    X(lbl_1_bss_14C) \
    X(lbl_1_bss_148) \
    X(lbl_1_bss_144) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M643_GAME_DATA(X) \
    X(lbl_1_data_28) \
    X(lbl_1_data_50) \
    X(lbl_1_data_60) \
    X(lbl_1_data_88) \
    X(lbl_1_data_94) \
    X(lbl_1_data_9C) \
    X(lbl_1_data_A4) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_B8) \
    X(lbl_1_data_C4)
MP6_REL_RESET_DEFINE(mp6_m643_game_reset, MP6_M643_GAME_BSS, MP6_M643_GAME_DATA)
