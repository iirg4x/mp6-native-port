#include "mp6_rel_state.h"
#define MP6_M602_SUPPORT_BSS(X) \
    X(lbl_1_bss_18)
#define MP6_M602_SUPPORT_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m602_support_reset, MP6_M602_SUPPORT_BSS, MP6_M602_SUPPORT_DATA)
