#include "mp6_rel_state.h"
#define MP6_MGMTOURNAMENT_WINDOW_BSS(X) \
    X(lbl_1_bss_C8)
#define MP6_MGMTOURNAMENT_WINDOW_DATA(X) \
    X(lbl_1_data_140) \
    X(lbl_1_data_160) \
    X(lbl_1_data_198) \
    X(lbl_1_data_270)
MP6_REL_RESET_DEFINE(mp6_mgmtournament_window_reset,
    MP6_MGMTOURNAMENT_WINDOW_BSS,
    MP6_MGMTOURNAMENT_WINDOW_DATA)
