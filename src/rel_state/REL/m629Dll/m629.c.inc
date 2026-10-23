#include "mp6_rel_state.h"
#define MP6_M629_BSS(X) \
    X(lbl_1_bss_69C) \
    X(lbl_1_bss_5FC) \
    X(lbl_1_bss_5F8) \
    X(lbl_1_bss_5F4) \
    X(lbl_1_bss_5E4) \
    X(lbl_1_bss_5E0) \
    X(lbl_1_bss_5DC) \
    X(lbl_1_bss_5D8) \
    X(lbl_1_bss_5D4) \
    X(lbl_1_bss_5D0) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M629_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_38) \
    X(lbl_1_data_60) \
    X(lbl_1_data_6A) \
    X(lbl_1_data_74) \
    X(lbl_1_data_80) \
    X(lbl_1_data_A8) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_B0) \
    X(lbl_1_data_B4) \
    X(lbl_1_data_B8) \
    X(lbl_1_data_C0) \
    X(lbl_1_data_C4) \
    X(lbl_1_data_C8) \
    X(lbl_1_data_CC) \
    X(lbl_1_data_DC) \
    X(lbl_1_data_EC) \
    X(lbl_1_data_FC) \
    X(lbl_1_data_124) \
    X(lbl_1_data_134) \
    X(lbl_1_data_140) \
    X(lbl_1_data_14C) \
    X(lbl_1_data_160) \
    X(lbl_1_data_168) \
    X(lbl_1_data_1B4) \
    X(lbl_1_data_1B8)
MP6_REL_RESET_DEFINE(mp6_m629_reset, MP6_M629_BSS, MP6_M629_DATA)
