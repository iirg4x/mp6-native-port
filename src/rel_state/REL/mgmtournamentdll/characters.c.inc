#include "mp6_rel_state.h"
#define MP6_MGMTOURNAMENT_CHARACTERS_BSS(X) \
    X(lbl_1_bss_12C) \
    X(lbl_1_bss_128) \
    X(lbl_1_bss_114) \
    X(lbl_1_bss_10C) \
    X(lbl_1_bss_10A) \
    X(lbl_1_bss_108)
#define MP6_MGMTOURNAMENT_CHARACTERS_DATA(X) \
    X(lbl_1_data_510) \
    X(lbl_1_data_544) \
    X(lbl_1_data_548) \
    X(lbl_1_data_578) \
    X(lbl_1_data_690) \
    X(lbl_1_data_710) \
    X(lbl_1_data_760) \
    X(lbl_1_data_790) \
    X(lbl_1_data_7C0) \
    X(lbl_1_data_7F0) \
    X(lbl_1_data_820) \
    X(lbl_1_data_838)
MP6_REL_RESET_DEFINE(mp6_mgmtournament_characters_reset,
    MP6_MGMTOURNAMENT_CHARACTERS_BSS,
    MP6_MGMTOURNAMENT_CHARACTERS_DATA)
