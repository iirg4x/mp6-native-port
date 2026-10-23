#include "mp6_rel_state.h"
#define MP6_W05_BOARD_BSS(X) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0) \
    X(lbl_1_bss_surface)
#define MP6_W05_BOARD_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_C) \
    X(lbl_1_data_40) \
    X(lbl_1_data_50) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_1DC) \
    X(lbl_1_data_214) \
    X(lbl_1_data_254) \
    X(lbl_1_data_280) \
    X(lbl_1_data_2A0) \
    X(lbl_1_data_2CC) \
    X(lbl_1_data_300) \
    X(lbl_1_data_310) \
    X(lbl_1_data_340) \
    X(lbl_1_data_34C) \
    X(lbl_1_data_358) \
    X(lbl_1_data_368) \
    X(lbl_1_data_374) \
    X(lbl_1_data_3A8) \
    X(lbl_1_data_3B4) \
    X(lbl_1_data_3C0) \
    X(lbl_1_data_3C4) \
    X(lbl_1_data_418) \
    X(lbl_1_data_424) \
    X(lbl_1_data_430) \
    X(lbl_1_data_434) \
    X(lbl_1_data_440) \
    X(lbl_1_data_44C) \
    X(lbl_1_data_450) \
    X(lbl_1_data_45C) \
    X(lbl_1_data_468) \
    X(lbl_1_data_46C) \
    X(lbl_1_data_478) \
    X(lbl_1_data_484) \
    X(lbl_1_data_488) \
    X(lbl_1_data_494) \
    X(lbl_1_data_4A0) \
    X(lbl_1_data_4D4) \
    X(lbl_1_data_4E0) \
    X(lbl_1_data_4EC) \
    X(lbl_1_data_51C) \
    X(lbl_1_data_558) \
    X(lbl_1_data_590) \
    X(lbl_1_data_5C8) \
    X(lbl_1_data_610) \
    X(lbl_1_data_634) \
    X(lbl_1_data_64C) \
    X(lbl_1_data_658) \
    X(lbl_1_data_664) \
    X(lbl_1_data_668) \
    X(lbl_1_data_680) \
    X(lbl_1_data_698) \
    X(lbl_1_data_6A4) \
    X(lbl_1_data_6B0) \
    X(lbl_1_data_6BC) \
    X(lbl_1_data_728) \
    X(lbl_1_data_748) \
    X(lbl_1_data_754) \
    X(lbl_1_data_770) \
    X(lbl_1_data_788) \
    X(lbl_1_data_850) \
    X(lbl_1_data_8C0) \
    X(lbl_1_data_8C4) \
    X(lbl_1_data_8C8) \
    X(lbl_1_data_8CC) \
    X(lbl_1_data_8E4) \
    X(lbl_1_data_8F0) \
    X(lbl_1_data_8FC) \
    X(lbl_1_data_908)
MP6_REL_RESET_DEFINE(mp6_w05_board_reset, MP6_W05_BOARD_BSS, MP6_W05_BOARD_DATA)

void mp6_w05_board_reset(void);
void mp6_w05_reset(void)
{
    mp6_w05_board_reset();
}
