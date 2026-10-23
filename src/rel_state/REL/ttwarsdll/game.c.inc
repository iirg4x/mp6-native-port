#include "mp6_rel_state.h"
#define MP6_TTWARS_GAME_BSS(X) \
    X(lbl_1_bss_1870) \
    X(lbl_1_bss_186C) \
    X(lbl_1_bss_1868) \
    X(lbl_1_bss_1864) \
    X(lbl_1_bss_1464) \
    X(lbl_1_bss_1454) \
    X(lbl_1_bss_1450) \
    X(lbl_1_bss_144E) \
    X(lbl_1_bss_1446) \
    X(lbl_1_bss_1444) \
    X(lbl_1_bss_143C) \
    X(lbl_1_bss_1438) \
    X(lbl_1_bss_1434) \
    X(lbl_1_bss_1430) \
    X(lbl_1_bss_142C) \
    X(lbl_1_bss_141C) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_TTWARS_GAME_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_C) \
    X(lbl_1_data_18) \
    X(lbl_1_data_1C) \
    X(lbl_1_data_38) \
    X(lbl_1_data_5E) \
    X(lbl_1_data_9C) \
    X(lbl_1_data_A8) \
    X(lbl_1_data_BA) \
    X(lbl_1_data_CC) \
    X(lbl_1_data_DB) \
    X(lbl_1_data_EE) \
    X(lbl_1_data_120) \
    X(lbl_1_data_122) \
    X(lbl_1_data_124) \
    X(lbl_1_data_126) \
    X(lbl_1_data_12F) \
    X(lbl_1_data_136) \
    X(lbl_1_data_13C) \
    X(lbl_1_data_13F) \
    X(lbl_1_data_147)
MP6_REL_RESET_DEFINE(mp6_ttwars_game_reset, MP6_TTWARS_GAME_BSS, MP6_TTWARS_GAME_DATA)

void mp6_ttwars_reset(void)
{
    mp6_ttwars_game_reset();
}
