#include "mp6_rel_state.h"
#define MP6_M631_GAME_BSS(X) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M631_GAME_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_A0)
MP6_REL_RESET_DEFINE(mp6_m631_game_reset, MP6_M631_GAME_BSS, MP6_M631_GAME_DATA)

void mp6_m631_reset(void)
{
    mp6_m631_actor_reset();
    mp6_m631_game_reset();
    mp6_m631_utilities_reset();
}
