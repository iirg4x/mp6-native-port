#include "mp6_rel_state.h"
#define MP6_OPTION_MAIN_BSS(X) \
    X(lbl_1_bss_BF8) \
    X(lbl_1_bss_BF0) \
    X(lbl_1_bss_B78) \
    X(lbl_1_bss_B00) \
    X(lbl_1_bss_AFC) \
    X(lbl_1_bss_AE0) \
    X(lbl_1_bss_A08) \
    X(lbl_1_bss_9EC) \
    X(lbl_1_bss_9D8) \
    X(lbl_1_bss_848) \
    X(lbl_1_bss_840) \
    X(lbl_1_bss_83C) \
    X(lbl_1_bss_838) \
    X(lbl_1_bss_6A2) \
    X(lbl_1_bss_6A0) \
    X(lbl_1_bss_66E) \
    X(lbl_1_bss_63C) \
    X(lbl_1_bss_5D6) \
    X(lbl_1_bss_5D4) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_6) \
    X(lbl_1_bss_0)
#define MP6_OPTION_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_320) \
    X(lbl_1_data_33C) \
    X(lbl_1_data_358) \
    X(lbl_1_data_390) \
    X(lbl_1_data_394) \
    X(lbl_1_data_3B0) \
    X(lbl_1_data_3B8) \
    X(lbl_1_data_3CC) \
    X(lbl_1_data_3D8) \
    X(lbl_1_data_3EC) \
    X(lbl_1_data_3F8) \
    X(lbl_1_data_40C)
MP6_REL_RESET_DEFINE(mp6_option_main_reset, MP6_OPTION_MAIN_BSS, MP6_OPTION_MAIN_DATA)

void mp6_option_main_reset(void);
void mp6_option_option_reset(void);
void mp6_option_records_reset(void);
void mp6_option_sound_test_reset(void);
void mp6_option_reset(void)
{
    mp6_option_main_reset();
    mp6_option_option_reset();
    mp6_option_records_reset();
    mp6_option_sound_test_reset();
}
