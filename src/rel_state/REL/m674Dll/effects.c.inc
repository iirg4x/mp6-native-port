#include "mp6_rel_state.h"
#define MP6_M674_EFFECTS_BSS(X) \
    X(lbl_1_bss_6C4) \
    X(lbl_1_bss_6C0)
#define MP6_M674_EFFECTS_DATA(X) \
    X(lbl_1_data_678)
MP6_REL_RESET_DEFINE(mp6_m674_effects_reset, MP6_M674_EFFECTS_BSS, MP6_M674_EFFECTS_DATA)
