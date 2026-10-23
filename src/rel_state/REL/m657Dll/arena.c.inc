#include "mp6_rel_state.h"
#define MP6_M657_ARENA_BSS(X) \
    X(lbl_1_bss_28)
#define MP6_M657_ARENA_DATA(X) \
    X(lbl_1_data_80) \
    X(lbl_1_data_8C)
MP6_REL_RESET_DEFINE(mp6_m657_arena_reset, MP6_M657_ARENA_BSS, MP6_M657_ARENA_DATA)
