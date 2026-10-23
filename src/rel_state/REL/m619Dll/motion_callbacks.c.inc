#include "mp6_rel_state.h"
#define MP6_M619_MOTION_CALLBACKS_BSS(X) \
    X(lbl_1_bss_1414) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10)
#define MP6_M619_MOTION_CALLBACKS_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m619_motion_callbacks_reset, MP6_M619_MOTION_CALLBACKS_BSS, MP6_M619_MOTION_CALLBACKS_DATA)
