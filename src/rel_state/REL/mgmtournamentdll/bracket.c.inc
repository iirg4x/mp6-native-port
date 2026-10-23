#include "mp6_rel_state.h"
#define MP6_MGMTOURNAMENT_BRACKET_BSS(X) \
    X(lbl_1_bss_E0) \
    X(lbl_1_bss_DC) \
    X(lbl_1_bss_D8)
#define MP6_MGMTOURNAMENT_BRACKET_DATA(X) \
    X(lbl_1_data_278) \
    X(lbl_1_data_298) \
    X(lbl_1_data_29C) \
    X(lbl_1_data_2F0) \
    X(lbl_1_data_30C) \
    X(lbl_1_data_318) \
    X(lbl_1_data_330) \
    X(lbl_1_data_3AC) \
    X(lbl_1_data_3BC) \
    X(lbl_1_data_3C4) \
    X(lbl_1_data_3FC) \
    X(lbl_1_data_424) \
    X(lbl_1_data_42C)
MP6_REL_RESET_DEFINE(mp6_mgmtournament_bracket_reset,
    MP6_MGMTOURNAMENT_BRACKET_BSS,
    MP6_MGMTOURNAMENT_BRACKET_DATA)
