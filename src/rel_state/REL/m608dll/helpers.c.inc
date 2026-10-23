#include "mp6_rel_state.h"
#define MP6_M608_HELPERS_BSS(X)
#define MP6_M608_HELPERS_DATA(X) \
    X(lbl_1_data_2F8) \
    X(lbl_1_data_320) \
    X(lbl_1_data_370) \
    X(lbl_1_data_3C0)
MP6_REL_RESET_DEFINE(mp6_m608_helpers_reset, MP6_M608_HELPERS_BSS, MP6_M608_HELPERS_DATA)
