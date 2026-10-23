#include "mp6_rel_state.h"
#define MP6_INST_DISPLAY_BSS(X) \
    X(lbl_1_bss_6AA) \
    X(lbl_1_bss_4AA) \
    X(lbl_1_bss_49E) \
    X(lbl_1_bss_43A) \
    X(lbl_1_bss_438) \
    X(lbl_1_bss_238) \
    X(lbl_1_bss_D8) \
    X(lbl_1_bss_D4) \
    X(lbl_1_bss_84) \
    X(lbl_1_bss_78) \
    X(lbl_1_bss_70) \
    X(lbl_1_bss_6E) \
    X(lbl_1_bss_6C) \
    X(lbl_1_bss_66) \
    X(lbl_1_bss_64) \
    X(lbl_1_bss_5C) \
    X(lbl_1_bss_58) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_48)
#define MP6_INST_DISPLAY_DATA(X) \
    X(lbl_1_data_700) \
    X(lbl_1_data_70C) \
    X(lbl_1_data_718) \
    X(lbl_1_data_724) \
    X(lbl_1_data_730) \
    X(lbl_1_data_74A) \
    X(lbl_1_data_764) \
    X(lbl_1_data_77E) \
    X(lbl_1_data_798)
MP6_REL_RESET_DEFINE(mp6_inst_display_reset, MP6_INST_DISPLAY_BSS, MP6_INST_DISPLAY_DATA)
