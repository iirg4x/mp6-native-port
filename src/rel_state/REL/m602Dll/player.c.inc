#include "mp6_rel_state.h"
#define MP6_M602_PLAYER_BSS(X) \
    X(lbl_1_bss_318) \
    X(lbl_1_bss_314) \
    X(lbl_1_bss_310) \
    X(lbl_1_bss_30C) \
    X(lbl_1_bss_30A) \
    X(lbl_1_bss_308) \
    X(lbl_1_bss_2FC) \
    X(lbl_1_bss_2F4) \
    X(lbl_1_bss_2F2) \
    X(lbl_1_bss_2F0) \
    X(lbl_1_bss_250) \
    X(lbl_1_bss_24E) \
    X(lbl_1_bss_24C) \
    X(lbl_1_bss_24A) \
    X(lbl_1_bss_248)
#define MP6_M602_PLAYER_DATA(X) \
    X(lbl_1_data_3D0) \
    X(lbl_1_data_400) \
    X(lbl_1_data_404) \
    X(lbl_1_data_414) \
    X(lbl_1_data_424) \
    X(lbl_1_data_434) \
    X(lbl_1_data_444) \
    X(lbl_1_data_44E)
MP6_REL_RESET_DEFINE(mp6_m602_player_reset, MP6_M602_PLAYER_BSS, MP6_M602_PLAYER_DATA)
