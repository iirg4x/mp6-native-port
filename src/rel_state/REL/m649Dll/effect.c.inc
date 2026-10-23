#include "mp6_rel_state.h"
#define MP6_M649_EFFECT_BSS(X)
#define MP6_M649_EFFECT_DATA(X) \
    X(lbl_1_data_538) \
    X(lbl_1_data_540)
MP6_REL_RESET_DEFINE(mp6_m649_effect_reset, MP6_M649_EFFECT_BSS, MP6_M649_EFFECT_DATA)
