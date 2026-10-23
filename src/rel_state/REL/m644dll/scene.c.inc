#include "mp6_rel_state.h"
#define MP6_M644_SCENE_BSS(X) \
    X(lbl_1_bss_40)
#define MP6_M644_SCENE_DATA(X) \
    X(lbl_1_data_70) \
    X(lbl_1_data_80)
MP6_REL_RESET_DEFINE(mp6_m644_scene_reset, MP6_M644_SCENE_BSS, MP6_M644_SCENE_DATA)
