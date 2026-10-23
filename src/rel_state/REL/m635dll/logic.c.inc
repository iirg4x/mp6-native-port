#include "mp6_rel_state.h"
#define MP6_M635_LOGIC_BSS(X) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M635_LOGIC_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m635_logic_reset, MP6_M635_LOGIC_BSS, MP6_M635_LOGIC_DATA)
