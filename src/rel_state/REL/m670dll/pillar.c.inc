#include "mp6_rel_state.h"
#define MP6_M670_PILLAR_BSS(X) \
    X(lbl_1_bss_480) \
    X(lbl_1_bss_47C) \
    X(lbl_1_bss_418) \
    X(lbl_1_bss_3B8)
#define MP6_M670_PILLAR_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m670_pillar_reset, MP6_M670_PILLAR_BSS, MP6_M670_PILLAR_DATA)
