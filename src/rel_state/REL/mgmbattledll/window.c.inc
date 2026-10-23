#include "mp6_rel_state.h"
#define MP6_MGMBATTLE_WINDOW_BSS(X) \
    X(lbl_1_bss_D8) \
    X(lbl_1_bss_A8)
#define MP6_MGMBATTLE_WINDOW_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_38) \
    X(lbl_1_data_200)
MP6_REL_RESET_DEFINE(mp6_mgmbattle_window_reset,
                     MP6_MGMBATTLE_WINDOW_BSS,
                     MP6_MGMBATTLE_WINDOW_DATA)
