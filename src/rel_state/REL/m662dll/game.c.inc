#include "mp6_rel_state.h"
#define MP6_M662_GAME_BSS(X)
#define MP6_M662_GAME_DATA(X) \
    X(lbl_1_data_150) \
    X(lbl_1_data_32C)
MP6_REL_RESET_DEFINE(mp6_m662_game_reset, MP6_M662_GAME_BSS, MP6_M662_GAME_DATA)
