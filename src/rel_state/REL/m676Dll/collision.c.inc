#include "mp6_rel_state.h"
#define MP6_M676_COLLISION_BSS(X) \
    X(lbl_1_bss_10B8)
#define MP6_M676_COLLISION_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m676_collision_reset, MP6_M676_COLLISION_BSS, MP6_M676_COLLISION_DATA)
