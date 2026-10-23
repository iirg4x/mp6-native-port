#include "mp6_rel_state.h"
#define MP6_M613_PROGRESS_BSS(X) \
    X(lbl_1_bss_1218) \
    X(lbl_1_bss_1210) \
    X(lbl_1_bss_D60) \
    X(lbl_1_bss_400)
#define MP6_M613_PROGRESS_DATA(X) \
    X(lbl_1_data_210) \
    X(lbl_1_data_240)
MP6_REL_RESET_DEFINE(mp6_m613_progress_reset, MP6_M613_PROGRESS_BSS, MP6_M613_PROGRESS_DATA)
