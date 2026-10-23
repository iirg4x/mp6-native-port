#include "mp6_rel_state.h"
#define MP6_M608_SPRITE_BSS(X) \
    X(lbl_1_bss_3E68)
#define MP6_M608_SPRITE_DATA(X) \
    X(lbl_1_data_4B0)
MP6_REL_RESET_DEFINE(mp6_m608_sprite_reset, MP6_M608_SPRITE_BSS, MP6_M608_SPRITE_DATA)
