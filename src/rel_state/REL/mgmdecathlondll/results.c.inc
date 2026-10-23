#include "mp6_rel_state.h"
#define MP6_MGMDECATHLON_RESULTS_BSS(X) \
    X(lbl_1_bss_BC) \
    X(lbl_1_bss_B8)
#define MP6_MGMDECATHLON_RESULTS_DATA(X) \
    X(lbl_1_data_5D8) \
    X(lbl_1_data_5DC) \
    X(lbl_1_data_60C) \
    X(lbl_1_data_61C) \
    X(lbl_1_data_64C) \
    X(lbl_1_data_684) \
    X(lbl_1_data_6B4) \
    X(lbl_1_data_6DC) \
    X(lbl_1_data_6F0) \
    X(lbl_1_data_700) \
    X(lbl_1_data_704) \
    X(lbl_1_data_714) \
    X(lbl_1_data_724) \
    X(lbl_1_data_744)
MP6_REL_RESET_DEFINE(mp6_mgmdecathlon_results_reset,
                     MP6_MGMDECATHLON_RESULTS_BSS,
                     MP6_MGMDECATHLON_RESULTS_DATA)
