#include "mp6_rel_state.h"
#define MP6_M644_CAMERA_BSS(X) \
    X(lbl_1_bss_8)
#define MP6_M644_CAMERA_DATA(X) \
    X(lbl_1_data_40)
MP6_REL_RESET_DEFINE(mp6_m644_camera_reset, MP6_M644_CAMERA_BSS, MP6_M644_CAMERA_DATA)
