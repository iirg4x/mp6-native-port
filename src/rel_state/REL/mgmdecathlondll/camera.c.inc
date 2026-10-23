#include "mp6_rel_state.h"
#define MP6_MGMDECATHLON_CAMERA_BSS(X) \
    X(lbl_1_bss_A) \
    X(lbl_1_bss_2) \
    X(lbl_1_bss_0)
#define MP6_MGMDECATHLON_CAMERA_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_mgmdecathlon_camera_reset,
                     MP6_MGMDECATHLON_CAMERA_BSS,
                     MP6_MGMDECATHLON_CAMERA_DATA)
