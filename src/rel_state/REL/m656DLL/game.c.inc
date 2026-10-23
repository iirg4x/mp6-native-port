#include "mp6_rel_state.h"
#define MP6_M656_GAME_BSS(X) \
    X(lbl_1_bss_AC) \
    X(lbl_1_bss_A4) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M656_GAME_DATA(X) \
    X(lbl_1_data_28) \
    X(lbl_1_data_3C) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_7C) \
    X(lbl_1_data_47C) \
    X(lbl_1_data_490) \
    X(lbl_1_data_4A4) \
    X(lbl_1_data_4B8) \
    X(lbl_1_data_4CC) \
    X(lbl_1_data_4E0) \
    X(lbl_1_data_4F4) \
    X(lbl_1_data_508) \
    X(lbl_1_data_51C) \
    X(lbl_1_data_530) \
    X(lbl_1_data_544) \
    X(lbl_1_data_558) \
    X(lbl_1_data_56C) \
    X(lbl_1_data_580) \
    X(lbl_1_data_594) \
    X(lbl_1_data_5A8) \
    X(lbl_1_data_5BC) \
    X(lbl_1_data_5D0) \
    X(lbl_1_data_5E4) \
    X(lbl_1_data_5F8) \
    X(lbl_1_data_60C) \
    X(lbl_1_data_65C) \
    X(lbl_1_data_66C) \
    X(lbl_1_data_67A) \
    X(lbl_1_data_690) \
    X(lbl_1_data_6A6) \
    X(lbl_1_data_6BA)
MP6_REL_RESET_DEFINE(mp6_m656_game_reset, MP6_M656_GAME_BSS, MP6_M656_GAME_DATA)
