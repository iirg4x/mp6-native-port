#include "mp6_rel_state.h"
#define MP6_M674_TARGETS_BSS(X) \
    X(lbl_1_bss_6BC) \
    X(lbl_1_bss_4BC) \
    X(lbl_1_bss_4B8) \
    X(lbl_1_bss_4B4) \
    X(lbl_1_bss_4B0) \
    X(lbl_1_bss_4AC) \
    X(lbl_1_bss_3E0) \
    X(lbl_1_bss_378) \
    X(lbl_1_bss_310)
#define MP6_M674_TARGETS_DATA(X) \
    X(lbl_1_data_270) \
    X(lbl_1_data_2A0) \
    X(lbl_1_data_2AC) \
    X(lbl_1_data_2B8) \
    X(lbl_1_data_2C4) \
    X(lbl_1_data_330) \
    X(lbl_1_data_5A4) \
    X(lbl_1_data_5D4) \
    X(lbl_1_data_60C) \
    X(lbl_1_data_634) \
    X(lbl_1_data_644) \
    X(lbl_1_data_650) \
    X(lbl_1_data_65C) \
    X(lbl_1_data_668) \
    X(lbl_1_data_674)
MP6_REL_RESET_DEFINE(mp6_m674_targets_reset, MP6_M674_TARGETS_BSS, MP6_M674_TARGETS_DATA)
