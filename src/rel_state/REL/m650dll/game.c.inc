#include "mp6_rel_state.h"
#define MP6_M650_GAME_BSS(X) \
    X(lbl_1_bss_28)
#define MP6_M650_GAME_DATA(X) \
    X(lbl_1_data_28) \
    X(lbl_1_data_34) \
    X(lbl_1_data_40) \
    X(lbl_1_data_48) \
    X(lbl_1_data_58) \
    X(lbl_1_data_80) \
    X(lbl_1_data_88) \
    X(lbl_1_data_90) \
    X(lbl_1_data_98) \
    X(lbl_1_data_A0) \
    X(lbl_1_data_A8) \
    X(lbl_1_data_150)
MP6_REL_RESET_DEFINE(mp6_m650_game_reset, MP6_M650_GAME_BSS, MP6_M650_GAME_DATA)
