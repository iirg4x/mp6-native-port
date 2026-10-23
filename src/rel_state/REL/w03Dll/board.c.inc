#include "mp6_rel_state.h"
#define MP6_W03_BOARD_BSS(X) \
    X(lbl_1_bss_5C4) \
    X(lbl_1_bss_4D4) \
    X(lbl_1_bss_4D0) \
    X(lbl_1_bss_4C8) \
    X(lbl_1_bss_468) \
    X(lbl_1_bss_450) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_W03_BOARD_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4B0) \
    X(lbl_1_data_4C8) \
    X(lbl_1_data_4D4) \
    X(lbl_1_data_504) \
    X(lbl_1_data_52C) \
    X(lbl_1_data_554) \
    X(lbl_1_data_568) \
    X(lbl_1_data_57C) \
    X(lbl_1_data_58C) \
    X(lbl_1_data_59C) \
    X(lbl_1_data_5A8) \
    X(lbl_1_data_5B4) \
    X(lbl_1_data_5CC) \
    X(lbl_1_data_5DC) \
    X(lbl_1_data_5E8) \
    X(lbl_1_data_5F4) \
    X(lbl_1_data_5F8) \
    X(lbl_1_data_604) \
    X(lbl_1_data_610) \
    X(lbl_1_data_620) \
    X(lbl_1_data_638) \
    X(lbl_1_data_63D) \
    X(lbl_1_data_644) \
    X(lbl_1_data_654) \
    X(lbl_1_data_664) \
    X(lbl_1_data_67C) \
    X(lbl_1_data_688) \
    X(lbl_1_data_694) \
    X(lbl_1_data_6A4) \
    X(lbl_1_data_6BC) \
    X(lbl_1_data_6EC) \
    X(lbl_1_data_6F4) \
    X(lbl_1_data_70C) \
    X(lbl_1_data_718) \
    X(lbl_1_data_724) \
    X(lbl_1_data_730) \
    X(lbl_1_data_748) \
    X(lbl_1_data_760) \
    X(lbl_1_data_770) \
    X(lbl_1_data_780) \
    X(lbl_1_data_790) \
    X(lbl_1_data_79C) \
    X(lbl_1_data_7A8) \
    X(lbl_1_data_7C0) \
    X(lbl_1_data_7CC) \
    X(lbl_1_data_7D8) \
    X(lbl_1_data_808) \
    X(lbl_1_data_81C) \
    X(lbl_1_data_840) \
    X(lbl_1_data_848) \
    X(lbl_1_data_850) \
    X(lbl_1_data_8B0) \
    X(lbl_1_data_910) \
    X(lbl_1_data_9A0) \
    X(lbl_1_data_9A4) \
    X(lbl_1_data_9B0) \
    X(lbl_1_data_9BC) \
    X(lbl_1_data_9CC)
MP6_REL_RESET_DEFINE(mp6_w03_board_reset, MP6_W03_BOARD_BSS, MP6_W03_BOARD_DATA)

void mp6_w03_reset(void)
{
    mp6_w03_board_reset();
}
