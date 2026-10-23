#include "mp6_rel_state.h"
#define MP6_W10_BOARD_BSS(X) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_0)
#define MP6_W10_BOARD_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_8) \
    X(lbl_1_data_58) \
    X(lbl_1_data_61) \
    X(lbl_1_data_6A) \
    X(lbl_1_data_6E) \
    X(lbl_1_data_72) \
    X(lbl_1_data_76) \
    X(lbl_1_data_7A) \
    X(lbl_1_data_7E) \
    X(lbl_1_data_82) \
    X(lbl_1_data_86) \
    X(lbl_1_data_8A) \
    X(lbl_1_data_90) \
    X(lbl_1_data_9C) \
    X(lbl_1_data_AC)
MP6_REL_RESET_DEFINE(mp6_w10_board_reset, MP6_W10_BOARD_BSS, MP6_W10_BOARD_DATA)

void mp6_w10_board_reset(void);
void mp6_w10_reset(void)
{
    mp6_w10_board_reset();
}
