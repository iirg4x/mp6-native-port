#include "mp6_rel_state.h"
#define MP6_M646_COLLISION_BSS(X) \
    X(lbl_1_bss_60)
#define MP6_M646_COLLISION_DATA(X) \
    X(lbl_1_data_108) \
    X(lbl_1_data_11C) \
    X(lbl_1_data_12C)
MP6_REL_RESET_DEFINE(mp6_m646_collision_reset, MP6_M646_COLLISION_BSS, MP6_M646_COLLISION_DATA)
