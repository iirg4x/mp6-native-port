#include "mp6_rel_state.h"
#define MP6_MDMIC_EFFECTS_BSS(X) \
    X(lbl_1_bss_B20) \
    X(lbl_1_bss_AD0) \
    X(lbl_1_bss_ACC) \
    X(lbl_1_bss_AC4) \
    X(lbl_1_bss_AC2) \
    X(lbl_1_bss_ABA) \
    X(lbl_1_bss_AB6) \
    X(lbl_1_bss_AB2) \
    X(lbl_1_bss_A76) \
    X(lbl_1_bss_A74) \
    X(lbl_1_bss_A70) \
    X(lbl_1_bss_A6C) \
    X(lbl_1_bss_A68) \
    X(lbl_1_bss_A64) \
    X(lbl_1_bss_A60)
#define MP6_MDMIC_EFFECTS_DATA(X) \
    X(lbl_1_data_FA0)
MP6_REL_RESET_DEFINE(mp6_mdmic_effects_reset,
                     MP6_MDMIC_EFFECTS_BSS,
                     MP6_MDMIC_EFFECTS_DATA)
