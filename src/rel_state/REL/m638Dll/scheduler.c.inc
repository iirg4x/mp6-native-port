#include "mp6_rel_state.h"
#define MP6_M638_SCHEDULER_BSS(X) \
    X(lbl_1_bss_A18) \
    X(lbl_1_bss_18)
#define MP6_M638_SCHEDULER_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m638_scheduler_reset, MP6_M638_SCHEDULER_BSS, MP6_M638_SCHEDULER_DATA)
