#include "mp6_rel_state.h"
#define MP6_M676_TEXT_05A10_BSS(X) \
    X(lbl_1_bss_478) \
    X(lbl_1_bss_468) \
    X(lbl_1_bss_458) \
    X(lbl_1_bss_454) \
    X(lbl_1_bss_450) \
    X(lbl_1_bss_43C) \
    X(lbl_1_bss_438)
#define MP6_M676_TEXT_05A10_DATA(X) \
    X(lbl_1_data_130) \
    X(lbl_1_data_150) \
    X(lbl_1_data_1B0) \
    X(lbl_1_data_1E0) \
    X(lbl_1_data_1F0) \
    X(lbl_1_data_220) \
    X(lbl_1_data_23C) \
    X(lbl_1_data_2A4) \
    X(lbl_1_data_2C4)
MP6_REL_RESET_DEFINE(mp6_m676_text_05A10_reset, MP6_M676_TEXT_05A10_BSS, MP6_M676_TEXT_05A10_DATA)
