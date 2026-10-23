#include "mp6_rel_state.h"
#define MP6_MICQUIZ_EFFECTS_BSS(X) \
    X(lbl_1_bss_5E4) \
    X(lbl_1_bss_5D8)
#define MP6_MICQUIZ_EFFECTS_DATA(X) \
    X(lbl_1_data_E68) \
    X(lbl_1_data_E78)
MP6_REL_RESET_DEFINE(mp6_micquiz_effects_reset, MP6_MICQUIZ_EFFECTS_BSS, MP6_MICQUIZ_EFFECTS_DATA)
