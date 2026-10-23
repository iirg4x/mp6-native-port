#include "mp6_rel_state.h"
#define MP6_M625_SCORE_BOX_BSS(X) \
    X(lbl_1_bss_7C8)
#define MP6_M625_SCORE_BOX_DATA(X) \
    X(lbl_1_data_410) \
    X(lbl_1_data_414) \
    X(lbl_1_data_438)
MP6_REL_RESET_DEFINE(mp6_m625_score_box_reset, MP6_M625_SCORE_BOX_BSS, MP6_M625_SCORE_BOX_DATA)
