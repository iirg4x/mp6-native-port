#include "mp6_rel_state.h"
#define MP6_M608_SCORE_BSS(X) \
    X(lbl_1_bss_3DF0)
#define MP6_M608_SCORE_DATA(X) \
    X(lbl_1_data_410) \
    X(lbl_1_data_44C)
MP6_REL_RESET_DEFINE(mp6_m608_score_reset, MP6_M608_SCORE_BSS, MP6_M608_SCORE_DATA)
