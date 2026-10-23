#include "mp6_rel_state.h"
#define MP6_M632_GAME_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M632_GAME_DATA(X) \
    X(lbl_1_data_78) \
    X(lbl_1_data_8C) \
    X(lbl_1_data_A0) \
    X(lbl_1_data_B0) \
    X(lbl_1_data_BC) \
    X(lbl_1_data_CF) \
    X(lbl_1_data_E2) \
    X(lbl_1_data_F5) \
    X(lbl_1_data_108) \
    X(lbl_1_data_118) \
    X(lbl_1_data_128) \
    X(lbl_1_data_158) \
    X(lbl_1_data_198) \
    X(lbl_1_data_1A8) \
    X(lbl_1_data_1E8) \
    X(lbl_1_data_228) \
    X(lbl_1_data_268) \
    X(lbl_1_data_2A8) \
    X(lbl_1_data_2B8) \
    X(lbl_1_data_2CD) \
    X(lbl_1_data_2E2) \
    X(lbl_1_data_2F7) \
    X(lbl_1_data_30C) \
    X(lbl_1_data_321) \
    X(lbl_1_data_336) \
    X(lbl_1_data_34B) \
    X(lbl_1_data_360) \
    X(lbl_1_data_378) \
    X(lbl_1_data_390) \
    X(lbl_1_data_3A8) \
    X(lbl_1_data_3C0) \
    X(lbl_1_data_3D8) \
    X(lbl_1_data_3F0) \
    X(lbl_1_data_408) \
    X(lbl_1_data_420) \
    X(lbl_1_data_437) \
    X(lbl_1_data_451) \
    X(lbl_1_data_468) \
    X(lbl_1_data_480)
MP6_REL_RESET_DEFINE(mp6_m632_game_reset, MP6_M632_GAME_BSS, MP6_M632_GAME_DATA)
