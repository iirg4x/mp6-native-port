#include "mp6_rel_state.h"
#define MP6_M650_OBSTACLE_BSS(X) \
    X(lbl_1_bss_4B6) \
    X(lbl_1_bss_4B4) \
    X(lbl_1_bss_4AC) \
    X(lbl_1_bss_2AC) \
    X(lbl_1_bss_2A8)
#define MP6_M650_OBSTACLE_DATA(X) \
    X(lbl_1_data_1F8) \
    X(lbl_1_data_248) \
    X(lbl_1_data_448) \
    X(lbl_1_data_450)
MP6_REL_RESET_DEFINE(mp6_m650_obstacle_reset, MP6_M650_OBSTACLE_BSS, MP6_M650_OBSTACLE_DATA)
