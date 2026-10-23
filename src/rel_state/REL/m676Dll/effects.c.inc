#include "mp6_rel_state.h"
#define MP6_M676_EFFECTS_BSS(X) \
    X(lbl_1_bss_10AC) \
    X(lbl_1_bss_10A8)
#define MP6_M676_EFFECTS_DATA(X) \
    X(lbl_1_data_7A0)
MP6_REL_RESET_DEFINE(mp6_m676_effects_reset, MP6_M676_EFFECTS_BSS, MP6_M676_EFFECTS_DATA)
