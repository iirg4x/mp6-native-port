#include "mp6_rel_state.h"
#define MP6_M675_EFFECTS_BSS(X) \
    X(lbl_1_bss_53C) \
    X(lbl_1_bss_538)
#define MP6_M675_EFFECTS_DATA(X) \
    X(lbl_1_data_848) \
    X(lbl_1_data_858)
MP6_REL_RESET_DEFINE(mp6_m675_effects_reset, MP6_M675_EFFECTS_BSS, MP6_M675_EFFECTS_DATA)
