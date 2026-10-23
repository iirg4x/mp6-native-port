#include "mp6_rel_state.h"
#define MP6_M644_PLAYER_BSS(X) \
    X(lbl_1_bss_78)
#define MP6_M644_PLAYER_DATA(X) \
    X(lbl_1_data_178) \
    X(lbl_1_data_188) \
    X(lbl_1_data_1CC)
MP6_REL_RESET_DEFINE(mp6_m644_player_reset, MP6_M644_PLAYER_BSS, MP6_M644_PLAYER_DATA)
