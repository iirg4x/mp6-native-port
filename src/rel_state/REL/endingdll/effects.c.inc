#include "mp6_rel_state.h"
#define MP6_ENDING_EFFECTS_BSS(X) \
    X(lbl_1_bss_1E30) \
    X(lbl_1_bss_1E2C) \
    X(lbl_1_bss_1E2A) \
    X(lbl_1_bss_1E26) \
    X(lbl_1_bss_1E22) \
    X(lbl_1_bss_1E1E) \
    X(lbl_1_bss_1E1C) \
    X(lbl_1_bss_1DE0)
#define MP6_ENDING_EFFECTS_DATA(X) \
    X(lbl_1_data_128)
MP6_REL_RESET_DEFINE(mp6_ending_effects_reset, MP6_ENDING_EFFECTS_BSS, MP6_ENDING_EFFECTS_DATA)
