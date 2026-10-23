#include "mp6_rel_state.h"
#define MP6_M640_GAME_BSS(X) \
    X(lbl_1_bss_4FC) \
    X(lbl_1_bss_4E8) \
    X(lbl_1_bss_3F8) \
    X(lbl_1_bss_3F4) \
    X(lbl_1_bss_3F0) \
    X(lbl_1_bss_3E4) \
    X(lbl_1_bss_3CC) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M640_GAME_DATA(X) \
    X(lbl_1_data_28) \
    X(lbl_1_data_34) \
    X(lbl_1_data_40) \
    X(lbl_1_data_44) \
    X(lbl_1_data_6C) \
    X(lbl_1_data_74) \
    X(lbl_1_data_7C) \
    X(lbl_1_data_84) \
    X(lbl_1_data_E4) \
    X(lbl_1_data_1A4) \
    X(lbl_1_data_1D4)
MP6_REL_RESET_DEFINE(mp6_m640_game_reset, MP6_M640_GAME_BSS, MP6_M640_GAME_DATA)
