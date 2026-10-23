#include "mp6_rel_state.h"
#define MP6_M608_SETUP_BSS(X)
#define MP6_M608_SETUP_DATA(X) \
    X(lbl_1_data_295) \
    X(lbl_1_data_2A0) \
    X(lbl_1_data_2B0) \
    X(lbl_1_data_2B5) \
    X(lbl_1_data_2BA) \
    X(lbl_1_data_2C5) \
    X(lbl_1_data_2D0) \
    X(lbl_1_data_2DB)
MP6_REL_RESET_DEFINE(mp6_m608_setup_reset, MP6_M608_SETUP_BSS, MP6_M608_SETUP_DATA)
