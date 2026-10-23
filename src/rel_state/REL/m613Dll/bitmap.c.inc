#include "mp6_rel_state.h"
#define MP6_M613_BITMAP_BSS(X) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8)
#define MP6_M613_BITMAP_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m613_bitmap_reset, MP6_M613_BITMAP_BSS, MP6_M613_BITMAP_DATA)
