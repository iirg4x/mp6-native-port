#include "mp6_rel_state.h"
#define MP6_MGMRENSHO_PLAYER_BSS(X) \
    X(lbl_1_bss_140)
#define MP6_MGMRENSHO_PLAYER_DATA(X) \
    X(lbl_1_data_338) \
    X(lbl_1_data_360) \
    X(lbl_1_data_364) \
    X(lbl_1_data_368) \
    X(lbl_1_data_380) \
    X(lbl_1_data_45C)
MP6_REL_RESET_DEFINE(mp6_mgmrensho_player_reset,
                     MP6_MGMRENSHO_PLAYER_BSS,
                     MP6_MGMRENSHO_PLAYER_DATA)
