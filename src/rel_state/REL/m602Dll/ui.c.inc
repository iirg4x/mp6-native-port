#include "mp6_rel_state.h"
#define MP6_M602_UI_BSS(X) \
    X(lbl_1_bss_49A) \
    X(lbl_1_bss_492) \
    X(lbl_1_bss_48A) \
    X(lbl_1_bss_482) \
    X(lbl_1_bss_480) \
    X(lbl_1_bss_47C) \
    X(lbl_1_bss_478)
#define MP6_M602_UI_DATA(X) \
    X(lbl_1_data_4B0) \
    X(lbl_1_data_4B4) \
    X(lbl_1_data_4E4) \
    X(lbl_1_data_4FC) \
    X(lbl_1_data_50C) \
    X(lbl_1_data_51C) \
    X(lbl_1_data_52C)
MP6_REL_RESET_DEFINE(mp6_m602_ui_reset, MP6_M602_UI_BSS, MP6_M602_UI_DATA)
