#include "mp6_rel_state.h"
#define MP6_MGMTOURNAMENT_HOST_BSS(X) \
    X(lbl_1_bss_100) \
    X(lbl_1_bss_FC) \
    X(lbl_1_bss_F0)
#define MP6_MGMTOURNAMENT_HOST_DATA(X) \
    X(lbl_1_data_440)
MP6_REL_RESET_DEFINE(mp6_mgmtournament_host_reset,
    MP6_MGMTOURNAMENT_HOST_BSS,
    MP6_MGMTOURNAMENT_HOST_DATA)
