#include "mp6_rel_state.h"
#define MP6_MDMINI_PARTICLES_BSS(X) \
    X(lbl_1_bss_938) \
    X(lbl_1_bss_8E8) \
    X(lbl_1_bss_8E4) \
    X(lbl_1_bss_8DC) \
    X(lbl_1_bss_8DA) \
    X(lbl_1_bss_8D2) \
    X(lbl_1_bss_8CE) \
    X(lbl_1_bss_8CA) \
    X(lbl_1_bss_88E) \
    X(lbl_1_bss_88C) \
    X(lbl_1_bss_888) \
    X(lbl_1_bss_884) \
    X(lbl_1_bss_880) \
    X(lbl_1_bss_87C) \
    X(lbl_1_bss_878)
#define MP6_MDMINI_PARTICLES_DATA(X) \
    X(lbl_1_data_998)
MP6_REL_RESET_DEFINE(mp6_mdmini_particles_reset, MP6_MDMINI_PARTICLES_BSS, MP6_MDMINI_PARTICLES_DATA)
