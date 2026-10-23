#include "mp6_rel_state.h"
#define MP6_M631_ACTOR_BSS(X)
#define MP6_M631_ACTOR_DATA(X) \
    X(lbl_1_data_100)
MP6_REL_RESET_DEFINE(mp6_m631_actor_reset, MP6_M631_ACTOR_BSS, MP6_M631_ACTOR_DATA)
