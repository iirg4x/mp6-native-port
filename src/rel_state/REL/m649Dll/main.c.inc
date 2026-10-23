#include "mp6_rel_state.h"
#define MP6_M649_MAIN_BSS(X) \
    X(lbl_1_bss_5A8) \
    X(lbl_1_bss_5A4) \
    X(lbl_1_bss_5A0) \
    X(lbl_1_bss_59C) \
    X(lbl_1_bss_39C) \
    X(lbl_1_bss_398) \
    X(lbl_1_bss_368) \
    X(lbl_1_bss_338) \
    X(lbl_1_bss_334) \
    X(lbl_1_bss_324) \
    X(lbl_1_bss_314) \
    X(lbl_1_bss_310) \
    X(lbl_1_bss_308) \
    X(lbl_1_bss_304) \
    X(lbl_1_bss_300) \
    X(lbl_1_bss_2FC) \
    X(lbl_1_bss_2F8) \
    X(lbl_1_bss_1B8) \
    X(lbl_1_bss_78) \
    X(lbl_1_bss_74) \
    X(lbl_1_bss_70) \
    X(lbl_1_bss_6C) \
    X(lbl_1_bss_68) \
    X(lbl_1_bss_64) \
    X(lbl_1_bss_60) \
    X(lbl_1_bss_5C) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M649_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_1C) \
    X(lbl_1_data_28) \
    X(lbl_1_data_34) \
    X(lbl_1_data_38) \
    X(lbl_1_data_80) \
    X(lbl_1_data_A8) \
    X(lbl_1_data_B8) \
    X(lbl_1_data_C4) \
    X(lbl_1_data_C8) \
    X(lbl_1_data_E0) \
    X(lbl_1_data_128) \
    X(lbl_1_data_158) \
    X(lbl_1_data_174) \
    X(lbl_1_data_1B4) \
    X(lbl_1_data_1C4) \
    X(lbl_1_data_224) \
    X(lbl_1_data_248)
MP6_REL_RESET_DEFINE(mp6_m649_main_reset, MP6_M649_MAIN_BSS, MP6_M649_MAIN_DATA)

void mp6_m649_effect_reset(void);
void mp6_m649_prop_reset(void);
void mp6_m649_score_reset(void);
void mp6_m649_reset(void) {
    mp6_m649_main_reset();
    mp6_m649_effect_reset();
    mp6_m649_prop_reset();
    mp6_m649_score_reset();
}
