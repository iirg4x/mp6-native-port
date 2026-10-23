#include "mp6_rel_state.h"
#define MP6_M674_COLLISION_BSS(X) \
    X(lbl_1_bss_6D0)
#define MP6_M674_COLLISION_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m674_collision_reset, MP6_M674_COLLISION_BSS, MP6_M674_COLLISION_DATA)
