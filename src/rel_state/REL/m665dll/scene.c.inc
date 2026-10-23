#include "mp6_rel_state.h"
#define MP6_M665_SCENE_BSS(X) \
    X(lbl_1_bss_278) \
    X(lbl_1_bss_274) \
    X(lbl_1_bss_250) \
    X(lbl_1_bss_10)
#define MP6_M665_SCENE_DATA(X) \
    X(lbl_1_data_68) \
    X(lbl_1_data_A0) \
    X(lbl_1_data_AC) \
    X(lbl_1_data_B8) \
    X(lbl_1_data_C0) \
    X(lbl_1_data_C8)
MP6_REL_RESET_DEFINE(mp6_m665_scene_reset, MP6_M665_SCENE_BSS, MP6_M665_SCENE_DATA)
