#include "mp6_rel_state.h"
#define MP6_M646_TARGET_SELECTION_BSS(X) \
    X(lbl_1_bss_C0)
#define MP6_M646_TARGET_SELECTION_DATA(X) \
    X(lbl_1_data_1DC8)
MP6_REL_RESET_DEFINE(mp6_m646_target_selection_reset, MP6_M646_TARGET_SELECTION_BSS, MP6_M646_TARGET_SELECTION_DATA)
