#include "mp6_rel_state.h"
#define MP6_W06_BOARD_MAIN_BSS(X) \
    X(lbl_1_bss_4B4) \
    X(lbl_1_bss_4B0) \
    X(lbl_1_bss_404) \
    X(lbl_1_bss_2E4) \
    X(lbl_1_bss_2E0) \
    X(lbl_1_bss_2AC) \
    X(lbl_1_bss_7C) \
    X(lbl_1_bss_74) \
    X(lbl_1_bss_68) \
    X(lbl_1_bss_64) \
    X(lbl_1_bss_60) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_W06_BOARD_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_8) \
    X(lbl_1_data_C) \
    X(lbl_1_data_54) \
    X(lbl_1_data_84) \
    X(lbl_1_data_124) \
    X(lbl_1_data_134) \
    X(lbl_1_data_13C) \
    X(lbl_1_data_16C) \
    X(lbl_1_data_178) \
    X(lbl_1_data_184) \
    X(lbl_1_data_188) \
    X(lbl_1_data_194) \
    X(lbl_1_data_1A0) \
    X(lbl_1_data_1B2) \
    X(lbl_1_data_1C4) \
    X(lbl_1_data_1F8) \
    X(lbl_1_data_210) \
    X(lbl_1_data_228) \
    X(lbl_1_data_244) \
    X(lbl_1_data_25C) \
    X(lbl_1_data_268) \
    X(lbl_1_data_270) \
    X(lbl_1_data_288) \
    X(lbl_1_data_2A0) \
    X(lbl_1_data_2AC) \
    X(lbl_1_data_2B4) \
    X(lbl_1_data_2C0) \
    X(lbl_1_data_2D8) \
    X(lbl_1_data_2F8) \
    X(lbl_1_data_30D) \
    X(lbl_1_data_314) \
    X(lbl_1_data_344) \
    X(lbl_1_data_35C) \
    X(lbl_1_data_384) \
    X(lbl_1_data_38C) \
    X(lbl_1_data_3AC) \
    X(lbl_1_data_3B0) \
    X(lbl_1_data_578) \
    X(lbl_1_data_6FC) \
    X(lbl_1_data_75C) \
    X(lbl_1_data_774) \
    X(lbl_1_data_7A4) \
    X(lbl_1_data_7A8) \
    X(lbl_1_data_7B0) \
    X(lbl_1_data_804) \
    X(lbl_1_data_810) \
    X(lbl_1_data_81C)
MP6_REL_RESET_DEFINE(mp6_w06_board_main_reset, MP6_W06_BOARD_MAIN_BSS, MP6_W06_BOARD_MAIN_DATA)

void mp6_w06_board_main_reset(void);
void mp6_w06_reset(void)
{
    mp6_w06_board_main_reset();
}
