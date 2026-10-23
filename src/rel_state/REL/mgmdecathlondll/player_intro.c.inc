#include "mp6_rel_state.h"
#define MP6_MGMDECATHLON_PLAYER_INTRO_BSS(X) \
    X(lbl_1_bss_8C) \
    X(lbl_1_bss_88) \
    X(lbl_1_bss_78)
#define MP6_MGMDECATHLON_PLAYER_INTRO_DATA(X) \
    X(lbl_1_data_2B0) \
    X(lbl_1_data_2BC)
MP6_REL_RESET_DEFINE(mp6_mgmdecathlon_player_intro_reset,
                     MP6_MGMDECATHLON_PLAYER_INTRO_BSS,
                     MP6_MGMDECATHLON_PLAYER_INTRO_DATA)
