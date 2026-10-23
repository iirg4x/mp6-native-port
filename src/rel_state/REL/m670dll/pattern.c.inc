#include "mp6_rel_state.h"
#define MP6_M670_PATTERN_BSS(X)
#define MP6_M670_PATTERN_DATA(X) \
    X(lbl_1_data_1D8) \
    X(lbl_1_data_1F0) \
    X(lbl_1_data_208) \
    X(lbl_1_data_220) \
    X(lbl_1_data_238) \
    X(lbl_1_data_250)
MP6_REL_RESET_DEFINE(mp6_m670_pattern_reset, MP6_M670_PATTERN_BSS, MP6_M670_PATTERN_DATA)
