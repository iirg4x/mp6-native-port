#include "mp6_rel_state.h"
#define MP6_MGMDECATHLON_SCENERY_BSS(X) \
    X(lbl_1_bss_A0) \
    X(lbl_1_bss_9C) \
    X(lbl_1_bss_98) \
    X(lbl_1_bss_96) \
    X(lbl_1_bss_94) \
    X(lbl_1_bss_90)
#define MP6_MGMDECATHLON_SCENERY_DATA(X) \
    X(lbl_1_data_358) \
    X(lbl_1_data_374) \
    X(lbl_1_data_39C) \
    X(lbl_1_data_3C0) \
    X(lbl_1_data_3E0) \
    X(lbl_1_data_420)
MP6_REL_RESET_DEFINE(mp6_mgmdecathlon_scenery_reset,
                     MP6_MGMDECATHLON_SCENERY_BSS,
                     MP6_MGMDECATHLON_SCENERY_DATA)
