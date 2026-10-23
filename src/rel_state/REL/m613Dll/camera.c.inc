#include "mp6_rel_state.h"
#define MP6_M613_CAMERA_BSS(X)
#define MP6_M613_CAMERA_DATA(X) \
    X(lbl_1_data_178) \
    X(lbl_1_data_1C8) \
    X(lbl_1_data_1EC)
MP6_REL_RESET_DEFINE(mp6_m613_camera_reset, MP6_M613_CAMERA_BSS, MP6_M613_CAMERA_DATA)
