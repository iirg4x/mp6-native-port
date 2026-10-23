#include "mp6_rel_state.h"
#define MP6_MGMDECATHLON_WINDOWS_BSS(X) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_18)
#define MP6_MGMDECATHLON_WINDOWS_DATA(X) \
    X(lbl_1_data_30) \
    X(lbl_1_data_50) \
    X(lbl_1_data_88) \
    X(lbl_1_data_B0) \
    X(lbl_1_data_2A8)
MP6_REL_RESET_DEFINE(mp6_mgmdecathlon_windows_reset,
                     MP6_MGMDECATHLON_WINDOWS_BSS,
                     MP6_MGMDECATHLON_WINDOWS_DATA)
