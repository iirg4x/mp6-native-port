#include "mp6_rel_state.h"
#define MP6_M662_MAIN_BSS(X) \
    X(lbl_1_bss_C60) \
    X(lbl_1_bss_C58) \
    X(lbl_1_bss_C50) \
    X(lbl_1_bss_B08) \
    X(lbl_1_bss_ACC) \
    X(lbl_1_bss_A7C) \
    X(lbl_1_bss_A74) \
    X(lbl_1_bss_4B4) \
    X(lbl_1_bss_3F4) \
    X(lbl_1_bss_2F4) \
    X(lbl_1_bss_210) \
    X(lbl_1_bss_D0) \
    X(lbl_1_bss_0)
#define MP6_M662_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_F6)
MP6_REL_RESET_DEFINE(mp6_m662_main_reset, MP6_M662_MAIN_BSS, MP6_M662_MAIN_DATA)

void mp6_m662_reset(void)
{
    mp6_m662_main_reset();
    mp6_m662_game_reset();
    mp6_m662_puzzle_reset();
}
