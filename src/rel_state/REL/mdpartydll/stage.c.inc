#include "mp6_rel_state.h"
#define MP6_MDPARTY_STAGE_BSS(X) \
    X(lbl_1_bss_B40) \
    X(lbl_1_bss_AF0) \
    X(lbl_1_bss_AEC) \
    X(lbl_1_bss_AE4) \
    X(lbl_1_bss_AE2) \
    X(lbl_1_bss_ADA) \
    X(lbl_1_bss_AD6) \
    X(lbl_1_bss_AD2) \
    X(lbl_1_bss_A96) \
    X(lbl_1_bss_A94) \
    X(lbl_1_bss_A90) \
    X(lbl_1_bss_A8C) \
    X(lbl_1_bss_A88) \
    X(lbl_1_bss_A84) \
    X(lbl_1_bss_A80)
#define MP6_MDPARTY_STAGE_DATA(X) \
    X(lbl_1_data_F10)
MP6_REL_RESET_DEFINE(mp6_mdparty_stage_reset, MP6_MDPARTY_STAGE_BSS, MP6_MDPARTY_STAGE_DATA)
