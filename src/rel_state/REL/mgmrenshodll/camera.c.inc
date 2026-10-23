#include "mp6_rel_state.h"
#define MP6_MGMRENSHO_CAMERA_BSS(X) \
    X(lbl_1_bss_B0) \
    X(lbl_1_bss_A8)
#define MP6_MGMRENSHO_CAMERA_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_mgmrensho_camera_reset,
                     MP6_MGMRENSHO_CAMERA_BSS,
                     MP6_MGMRENSHO_CAMERA_DATA)
