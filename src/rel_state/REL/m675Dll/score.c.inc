#include "mp6_rel_state.h"
#define MP6_M675_SCORE_BSS(X) \
    X(lbl_1_bss_540)
#define MP6_M675_SCORE_DATA(X) \
    X(lbl_1_data_860)
MP6_REL_RESET_DEFINE(mp6_m675_score_reset, MP6_M675_SCORE_BSS, MP6_M675_SCORE_DATA)
