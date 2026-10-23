#include "mp6_rel_state.h"
#define MP6_M644_ROUTES_BSS(X) \
    X(lbl_1_bss_5AC) \
    X(lbl_1_bss_3D8)
#define MP6_M644_ROUTES_DATA(X) \
    X(lbl_1_data_2C0) \
    X(lbl_1_data_2E0) \
    X(lbl_1_data_2F0) \
    X(lbl_1_data_2F2) \
    X(lbl_1_data_2F4) \
    X(lbl_1_data_2F8) \
    X(lbl_1_data_308) \
    X(lbl_1_data_318) \
    X(lbl_1_data_320) \
    X(lbl_1_data_35C)
MP6_REL_RESET_DEFINE(mp6_m644_routes_reset, MP6_M644_ROUTES_BSS, MP6_M644_ROUTES_DATA)
