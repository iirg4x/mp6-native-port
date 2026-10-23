#include "mp6_rel_state.h"
#define MP6_M674_SCENE_BSS(X) \
    X(lbl_1_bss_2D8) \
    X(lbl_1_bss_2D4) \
    X(lbl_1_bss_2D0) \
    X(lbl_1_bss_2CC) \
    X(lbl_1_bss_2C8) \
    X(lbl_1_bss_188) \
    X(lbl_1_bss_48)
#define MP6_M674_SCENE_DATA(X) \
    X(lbl_1_data_C8) \
    X(lbl_1_data_D4)
MP6_REL_RESET_DEFINE(mp6_m674_scene_reset, MP6_M674_SCENE_BSS, MP6_M674_SCENE_DATA)
