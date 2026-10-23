#include "mp6_rel_state.h"
#define MP6_M602_STAGE_BSS(X) \
    X(lbl_1_bss_A0) \
    X(lbl_1_bss_8C) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_3A) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_32) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2E) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_2A) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20)
#define MP6_M602_STAGE_DATA(X) \
    X(lbl_1_data_E8) \
    X(lbl_1_data_11C) \
    X(lbl_1_data_124) \
    X(lbl_1_data_214) \
    X(lbl_1_data_264) \
    X(lbl_1_data_26D)
MP6_REL_RESET_DEFINE(mp6_m602_stage_reset, MP6_M602_STAGE_BSS, MP6_M602_STAGE_DATA)
