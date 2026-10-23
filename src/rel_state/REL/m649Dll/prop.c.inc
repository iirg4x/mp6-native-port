#include "mp6_rel_state.h"
#define MP6_M649_PROP_BSS(X)
#define MP6_M649_PROP_DATA(X) \
    X(lbl_1_data_464) \
    X(lbl_1_data_47C) \
    X(lbl_1_data_4C4) \
    X(lbl_1_data_524)
MP6_REL_RESET_DEFINE(mp6_m649_prop_reset, MP6_M649_PROP_BSS, MP6_M649_PROP_DATA)
