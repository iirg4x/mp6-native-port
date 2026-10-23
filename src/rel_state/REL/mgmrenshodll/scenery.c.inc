#include "mp6_rel_state.h"
#define MP6_MGMRENSHO_SCENERY_BSS(X) \
    X(lbl_1_bss_138) \
    X(lbl_1_bss_12C) \
    X(lbl_1_bss_128) \
    X(lbl_1_bss_124) \
    X(lbl_1_bss_11C) \
    X(lbl_1_bss_118)
#define MP6_MGMRENSHO_SCENERY_DATA(X) \
    X(lbl_1_data_298) \
    X(lbl_1_data_2D4) \
    X(lbl_1_data_2E4)
MP6_REL_RESET_DEFINE(mp6_mgmrensho_scenery_reset,
                     MP6_MGMRENSHO_SCENERY_BSS,
                     MP6_MGMRENSHO_SCENERY_DATA)
