#include "mp6_rel_state.h"
#define MP6_M635_GRAPHICS_BSS(X) \
    X(lbl_1_bss_BC) \
    X(lbl_1_bss_B4) \
    X(lbl_1_bss_74) \
    X(lbl_1_bss_68) \
    X(lbl_1_bss_64) \
    X(lbl_1_bss_60)
#define MP6_M635_GRAPHICS_DATA(X) \
    X(lbl_1_data_A0) \
    X(lbl_1_data_A8) \
    X(lbl_1_data_C0) \
    X(lbl_1_data_D8) \
    X(lbl_1_data_E4) \
    X(lbl_1_data_F0) \
    X(lbl_1_data_F8) \
    X(lbl_1_data_128) \
    X(lbl_1_data_158) \
    X(lbl_1_data_188) \
    X(lbl_1_data_1A8) \
    X(lbl_1_data_2E8) \
    X(lbl_1_data_3E4) \
    X(lbl_1_data_3EC) \
    X(lbl_1_data_3F4) \
    X(lbl_1_data_404) \
    X(lbl_1_data_414) \
    X(lbl_1_data_41C) \
    X(lbl_1_data_44C)
MP6_REL_RESET_DEFINE(mp6_m635_graphics_reset, MP6_M635_GRAPHICS_BSS, MP6_M635_GRAPHICS_DATA)
