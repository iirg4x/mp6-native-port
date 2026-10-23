#include "mp6_rel_state.h"
#define MP6_W11_WORLD11_BOARD_BSS(X) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_2) \
    X(lbl_1_bss_0)
#define MP6_W11_WORLD11_BOARD_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_15) \
    X(lbl_1_data_20) \
    X(lbl_1_data_2C) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_EC) \
    X(lbl_1_data_114) \
    X(lbl_1_data_154) \
    X(lbl_1_data_204) \
    X(lbl_1_data_244) \
    X(lbl_1_data_2A4)
MP6_REL_RESET_DEFINE(mp6_w11_world11_board_reset, MP6_W11_WORLD11_BOARD_BSS, MP6_W11_WORLD11_BOARD_DATA)

void mp6_w11_world11_board_reset(void);
void mp6_w11_reset(void)
{
    mp6_w11_world11_board_reset();
}
