#include "mp6_rel_state.h"
#define MP6_M637_GAME_BSS(X) \
    X(lbl_1_bss_374) \
    X(lbl_1_bss_370) \
    X(lbl_1_bss_36C) \
    X(lbl_1_bss_368) \
    X(lbl_1_bss_364) \
    X(lbl_1_bss_360) \
    X(lbl_1_bss_35C) \
    X(lbl_1_bss_358) \
    X(lbl_1_bss_354) \
    X(lbl_1_bss_350) \
    X(lbl_1_bss_34C) \
    X(lbl_1_bss_330) \
    X(lbl_1_bss_2A0) \
    X(lbl_1_bss_290) \
    X(lbl_1_bss_1D0) \
    X(lbl_1_bss_118) \
    X(lbl_1_bss_114) \
    X(lbl_1_bss_110) \
    X(lbl_1_bss_10E) \
    X(lbl_1_bss_10C) \
    X(lbl_1_bss_10A) \
    X(lbl_1_bss_108) \
    X(lbl_1_bss_104) \
    X(lbl_1_bss_100) \
    X(lbl_1_bss_F8) \
    X(lbl_1_bss_B8) \
    X(lbl_1_bss_8)
#define MP6_M637_GAME_DATA(X) \
    X(lbl_1_data_28) \
    X(lbl_1_data_4C) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_68)
MP6_REL_RESET_DEFINE(mp6_m637_game_reset, MP6_M637_GAME_BSS, MP6_M637_GAME_DATA)
