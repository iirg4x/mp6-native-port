#include "mp6_rel_state.h"
#define MP6_M618_GAME_BSS(X) \
    X(lbl_1_bss_558) \
    X(lbl_1_bss_554) \
    X(lbl_1_bss_550) \
    X(lbl_1_bss_54C) \
    X(lbl_1_bss_548) \
    X(lbl_1_bss_544) \
    X(lbl_1_bss_540) \
    X(lbl_1_bss_53C) \
    X(lbl_1_bss_4CC) \
    X(lbl_1_bss_4C8) \
    X(lbl_1_bss_30C) \
    X(lbl_1_bss_2F4) \
    X(lbl_1_bss_2CC) \
    X(lbl_1_bss_2BC) \
    X(lbl_1_bss_2B4) \
    X(lbl_1_bss_114) \
    X(lbl_1_bss_84) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_40) \
    X(m618UnknownBss3C) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_8)
#define MP6_M618_GAME_DATA(X) \
    X(lbl_1_data_28) \
    X(lbl_1_data_38) \
    X(lbl_1_data_48)
MP6_REL_RESET_DEFINE(mp6_m618_game_reset, MP6_M618_GAME_BSS, MP6_M618_GAME_DATA)
