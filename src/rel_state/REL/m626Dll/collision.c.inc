#include "mp6_rel_state.h"
#define MP6_M626_COLLISION_BSS(X) \
    X(lbl_1_bss_6D0)
#define MP6_M626_COLLISION_DATA(X) \
    X(lbl_1_data_908)
MP6_REL_RESET_DEFINE(mp6_m626_collision_reset, MP6_M626_COLLISION_BSS, MP6_M626_COLLISION_DATA)
