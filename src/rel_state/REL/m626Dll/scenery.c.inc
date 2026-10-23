#include "mp6_rel_state.h"
#define MP6_M626_SCENERY_BSS(X) \
    X(lbl_1_bss_6C0) \
    X(lbl_1_bss_4C0) \
    X(lbl_1_bss_448) \
    X(lbl_1_bss_310) \
    X(lbl_1_bss_30C) \
    X(lbl_1_bss_308)
#define MP6_M626_SCENERY_DATA(X) \
    X(lbl_1_data_470) \
    X(lbl_1_data_528) \
    X(lbl_1_data_5D0) \
    X(lbl_1_data_684) \
    X(lbl_1_data_7E4) \
    X(lbl_1_data_80C) \
    X(lbl_1_data_838) \
    X(lbl_1_data_850) \
    X(lbl_1_data_85C) \
    X(lbl_1_data_87F)
MP6_REL_RESET_DEFINE(mp6_m626_scenery_reset, MP6_M626_SCENERY_BSS, MP6_M626_SCENERY_DATA)
