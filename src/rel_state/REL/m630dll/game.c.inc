#include "mp6_rel_state.h"
#define MP6_M630_GAME_BSS(X) \
    X(lbl_1_bss_834) \
    X(lbl_1_bss_828) \
    X(lbl_1_bss_824) \
    X(lbl_1_bss_754) \
    X(lbl_1_bss_750) \
    X(lbl_1_bss_744) \
    X(lbl_1_bss_6FC) \
    X(lbl_1_bss_6E4) \
    X(lbl_1_bss_1D4) \
    X(lbl_1_bss_174) \
    X(lbl_1_bss_170) \
    X(lbl_1_bss_16C) \
    X(lbl_1_bss_168) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M630_GAME_DATA(X) \
    X(lbl_1_data_28) \
    X(lbl_1_data_4C) \
    X(lbl_1_data_7C)
MP6_REL_RESET_DEFINE(mp6_m630_game_reset, MP6_M630_GAME_BSS, MP6_M630_GAME_DATA)
