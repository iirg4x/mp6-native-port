#include "mp6_rel_state.h"
#define MP6_M680_SCHEDULER_BSS(X) \
    X(lbl_1_bss_1410) \
    X(lbl_1_bss_10)
#define MP6_M680_SCHEDULER_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m680_scheduler_reset, MP6_M680_SCHEDULER_BSS, MP6_M680_SCHEDULER_DATA)
