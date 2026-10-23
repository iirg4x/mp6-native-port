#include "mp6_rel_state.h"
#define MP6_M608_DATA_BSS(X) \
    X(lbl_1_bss_3E80)
#define MP6_M608_DATA_DATA(X) \
    X(lbl_1_data_4F0) \
    X(lbl_1_data_518) \
    X(lbl_1_data_528) \
    X(lbl_1_data_538) \
    X(lbl_1_data_548) \
    X(lbl_1_data_56C) \
    X(lbl_1_data_58C) \
    X(lbl_1_data_59D) \
    X(lbl_1_data_5AE) \
    X(lbl_1_data_5BF) \
    X(lbl_1_data_5D0) \
    X(lbl_1_data_5E0) \
    X(lbl_1_data_5F1) \
    X(lbl_1_data_602) \
    X(lbl_1_data_613) \
    X(lbl_1_data_624) \
    X(lbl_1_data_634) \
    X(lbl_1_data_63A) \
    X(lbl_1_data_640) \
    X(lbl_1_data_646) \
    X(lbl_1_data_64C) \
    X(lbl_1_data_65C) \
    X(lbl_1_data_666) \
    X(lbl_1_data_670) \
    X(lbl_1_data_67A) \
    X(lbl_1_data_684) \
    X(lbl_1_data_694)
MP6_REL_RESET_DEFINE(mp6_m608_data_reset, MP6_M608_DATA_BSS, MP6_M608_DATA_DATA)
