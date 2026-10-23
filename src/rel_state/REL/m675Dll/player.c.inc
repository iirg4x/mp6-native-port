#include "mp6_rel_state.h"
#define MP6_M675_PLAYER_BSS(X) \
    X(lbl_1_bss_310) \
    X(lbl_1_bss_300) \
    X(lbl_1_bss_2FC) \
    X(lbl_1_bss_2F8) \
    X(lbl_1_bss_2E4) \
    X(lbl_1_bss_2E0)
#define MP6_M675_PLAYER_DATA(X) \
    X(lbl_1_data_100) \
    X(lbl_1_data_124) \
    X(lbl_1_data_1A8) \
    X(lbl_1_data_1D8) \
    X(lbl_1_data_1E8) \
    X(lbl_1_data_218) \
    X(lbl_1_data_228) \
    X(lbl_1_data_258) \
    X(lbl_1_data_268) \
    X(lbl_1_data_298) \
    X(lbl_1_data_2A8) \
    X(lbl_1_data_32C) \
    X(lbl_1_data_364) \
    X(lbl_1_data_3EC) \
    X(lbl_1_data_43C)
MP6_REL_RESET_DEFINE(mp6_m675_player_reset, MP6_M675_PLAYER_BSS, MP6_M675_PLAYER_DATA)
