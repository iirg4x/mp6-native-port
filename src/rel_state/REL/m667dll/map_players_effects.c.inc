#include "mp6_rel_state.h"
#define MP6_M667_GAME_BSS(X) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M667_GAME_DATA(X) \
    X(lbl_1_data_30) \
    X(lbl_1_data_74) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_E4) \
    X(lbl_1_data_11C) \
    X(lbl_1_data_1EC) \
    X(lbl_1_data_200) \
    X(lbl_1_data_230) \
    X(lbl_1_data_290) \
    X(lbl_1_data_298) \
    X(lbl_1_data_2A7) \
    X(lbl_1_data_2B6) \
    X(lbl_1_data_2C5) \
    X(lbl_1_data_2D4) \
    X(lbl_1_data_2E2) \
    X(lbl_1_data_2F0) \
    X(lbl_1_data_2FE) \
    X(lbl_1_data_30C) \
    X(lbl_1_data_316) \
    X(lbl_1_data_320) \
    X(lbl_1_data_32A) \
    X(lbl_1_data_334) \
    X(lbl_1_data_364) \
    X(lbl_1_data_394) \
    X(lbl_1_data_3C4) \
    X(lbl_1_data_3D4) \
    X(lbl_1_data_3E0) \
    X(lbl_1_data_3EC) \
    X(lbl_1_data_40C) \
    X(lbl_1_data_418) \
    X(lbl_1_data_424) \
    X(lbl_1_data_42C) \
    X(lbl_1_data_43C) \
    X(lbl_1_data_44C) \
    X(lbl_1_data_494) \
    X(lbl_1_data_514)
MP6_REL_RESET_DEFINE(mp6_m667_game_reset, MP6_M667_GAME_BSS, MP6_M667_GAME_DATA)
