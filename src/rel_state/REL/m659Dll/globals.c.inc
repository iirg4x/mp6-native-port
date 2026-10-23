#include "mp6_rel_state.h"
#define MP6_M659_GLOBALS_BSS(X) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M659_GLOBALS_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m659_globals_reset, MP6_M659_GLOBALS_BSS, MP6_M659_GLOBALS_DATA)
