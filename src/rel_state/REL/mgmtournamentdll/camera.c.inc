#include "mp6_rel_state.h"
#define MP6_MGMTOURNAMENT_CAMERA_BSS(X) \
    X(lbl_1_bss_C6) \
    X(lbl_1_bss_C4) \
    X(lbl_1_bss_AC) \
    X(lbl_1_bss_A8)
#define MP6_MGMTOURNAMENT_CAMERA_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4)
MP6_REL_RESET_DEFINE(mp6_mgmtournament_camera_reset,
    MP6_MGMTOURNAMENT_CAMERA_BSS,
    MP6_MGMTOURNAMENT_CAMERA_DATA)
