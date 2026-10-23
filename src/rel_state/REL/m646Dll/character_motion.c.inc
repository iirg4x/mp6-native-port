#include "mp6_rel_state.h"
#define MP6_M646_CHARACTER_MOTION_BSS(X) \
    X(lbl_1_bss_70)
#define MP6_M646_CHARACTER_MOTION_DATA(X) \
    X(lbl_1_data_190) \
    X(lbl_1_data_1C0) \
    X(lbl_1_data_1CB) \
    X(lbl_1_data_1D6) \
    X(lbl_1_data_1E1) \
    X(lbl_1_data_1EC) \
    X(lbl_1_data_1FC) \
    X(lbl_1_data_20C) \
    X(lbl_1_data_21C) \
    X(lbl_1_data_22A) \
    X(lbl_1_data_238) \
    X(lbl_1_data_254) \
    X(lbl_1_data_270) \
    X(lbl_1_data_279)
MP6_REL_RESET_DEFINE(mp6_m646_character_motion_reset, MP6_M646_CHARACTER_MOTION_BSS, MP6_M646_CHARACTER_MOTION_DATA)
