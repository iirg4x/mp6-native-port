#include "mp6_rel_state.h"
#define MP6_M665_PLAYER_BSS(X) \
    X(lbl_1_bss_2E8) \
    X(lbl_1_bss_2E4) \
    X(lbl_1_bss_2E0) \
    X(lbl_1_bss_280)
#define MP6_M665_PLAYER_DATA(X) \
    X(lbl_1_data_100)
MP6_REL_RESET_DEFINE(mp6_m665_player_reset, MP6_M665_PLAYER_BSS, MP6_M665_PLAYER_DATA)
