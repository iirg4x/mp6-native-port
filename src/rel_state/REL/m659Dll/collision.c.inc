#include "mp6_rel_state.h"
#define MP6_M659_COLLISION_BSS(X) \
    X(lbl_1_bss_54) \
    X(lbl_1_bss_50)
#define MP6_M659_COLLISION_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m659_collision_reset, MP6_M659_COLLISION_BSS, MP6_M659_COLLISION_DATA)
