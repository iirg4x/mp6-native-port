#include "mp6_rel_state.h"
#define MP6_OPTION_RECORDS_BSS(X)
#define MP6_OPTION_RECORDS_DATA(X) \
    X(lbl_1_data_450) \
    X(lbl_1_data_462) \
    X(lbl_1_data_46A) \
    X(lbl_1_data_472) \
    X(lbl_1_data_479) \
    X(lbl_1_data_481) \
    X(lbl_1_data_486) \
    X(lbl_1_data_48C) \
    X(lbl_1_data_4D4) \
    X(lbl_1_data_4D8) \
    X(lbl_1_data_4DD) \
    X(lbl_1_data_4E2) \
    X(lbl_1_data_4F0)
MP6_REL_RESET_DEFINE(mp6_option_records_reset, MP6_OPTION_RECORDS_BSS, MP6_OPTION_RECORDS_DATA)
