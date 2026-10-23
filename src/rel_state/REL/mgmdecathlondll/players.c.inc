#include "mp6_rel_state.h"
#define MP6_MGMDECATHLON_PLAYERS_BSS(X) \
    X(lbl_1_bss_A8)
#define MP6_MGMDECATHLON_PLAYERS_DATA(X) \
    X(lbl_1_data_4A8) \
    X(lbl_1_data_4C8) \
    X(lbl_1_data_4F8)
MP6_REL_RESET_DEFINE(mp6_mgmdecathlon_players_reset,
                     MP6_MGMDECATHLON_PLAYERS_BSS,
                     MP6_MGMDECATHLON_PLAYERS_DATA)
