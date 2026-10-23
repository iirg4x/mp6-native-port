#include "mp6_rel_state.h"
#define MP6_SAF_SAF_DATA_BSS(X) \
    X(lbl_1_bss_4C0) \
    X(lbl_1_bss_4BC) \
    X(lbl_1_bss_49C) \
    X(lbl_1_bss_1B8) \
    X(lbl_1_bss_1B4) \
    X(lbl_1_bss_1B0) \
    X(lbl_1_bss_1AC) \
    X(lbl_1_bss_1A4) \
    X(lbl_1_bss_1A0) \
    X(lbl_1_bss_19C) \
    X(lbl_1_bss_198) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_SAF_SAF_DATA_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_30) \
    X(lbl_1_data_3C) \
    X(lbl_1_data_54)
MP6_REL_RESET_DEFINE(mp6_saf_saf_data_reset, MP6_SAF_SAF_DATA_BSS, MP6_SAF_SAF_DATA_DATA)
