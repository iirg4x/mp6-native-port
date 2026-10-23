#include "mp6_rel_state.h"
#define MP6_M657_SCORE_BSS(X) \
    X(lbl_1_bss_38)
#define MP6_M657_SCORE_DATA(X) \
    X(lbl_1_data_1E8)
MP6_REL_RESET_DEFINE(mp6_m657_score_reset, MP6_M657_SCORE_BSS, MP6_M657_SCORE_DATA)
