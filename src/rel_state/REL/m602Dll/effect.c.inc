#include "mp6_rel_state.h"
#define MP6_M602_EFFECT_BSS(X) \
    X(lbl_1_bss_23C) \
    X(lbl_1_bss_236) \
    X(lbl_1_bss_234) \
    X(lbl_1_bss_232) \
    X(lbl_1_bss_202) \
    X(lbl_1_bss_1A2) \
    X(lbl_1_bss_1A0)
#define MP6_M602_EFFECT_DATA(X) \
    X(lbl_1_data_278) \
    X(lbl_1_data_298) \
    X(lbl_1_data_2E8) \
    X(lbl_1_data_2F0) \
    X(lbl_1_data_300) \
    X(lbl_1_data_304) \
    X(lbl_1_data_308) \
    X(lbl_1_data_30C) \
    X(lbl_1_data_310) \
    X(lbl_1_data_314) \
    X(lbl_1_data_318) \
    X(lbl_1_data_31C) \
    X(lbl_1_data_320) \
    X(lbl_1_data_324) \
    X(lbl_1_data_329) \
    X(lbl_1_data_32E) \
    X(lbl_1_data_333) \
    X(lbl_1_data_338) \
    X(lbl_1_data_33D) \
    X(lbl_1_data_342) \
    X(lbl_1_data_347) \
    X(lbl_1_data_34C) \
    X(lbl_1_data_351) \
    X(lbl_1_data_356) \
    X(lbl_1_data_35B) \
    X(lbl_1_data_360) \
    X(lbl_1_data_365) \
    X(lbl_1_data_36A) \
    X(lbl_1_data_370)
MP6_REL_RESET_DEFINE(mp6_m602_effect_reset, MP6_M602_EFFECT_BSS, MP6_M602_EFFECT_DATA)
