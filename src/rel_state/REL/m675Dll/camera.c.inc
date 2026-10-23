#include "mp6_rel_state.h"
#define MP6_M675_CAMERA_BSS(X) \
    X(lbl_1_bss_2D8) \
    X(lbl_1_bss_2D4) \
    X(lbl_1_bss_2D0) \
    X(lbl_1_bss_2CC) \
    X(lbl_1_bss_2C8) \
    X(lbl_1_bss_188) \
    X(lbl_1_bss_48)
#define MP6_M675_CAMERA_DATA(X) \
    X(lbl_1_data_F0) \
    X(lbl_1_data_FC)
MP6_REL_RESET_DEFINE(mp6_m675_camera_reset, MP6_M675_CAMERA_BSS, MP6_M675_CAMERA_DATA)
