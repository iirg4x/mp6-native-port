#include "mp6_rel_state.h"
#define MP6_M676_SCORE_BSS(X) \
    X(lbl_1_bss_10B0)
#define MP6_M676_SCORE_DATA(X) \
    X(lbl_1_data_7B0)
MP6_REL_RESET_DEFINE(mp6_m676_score_reset, MP6_M676_SCORE_BSS, MP6_M676_SCORE_DATA)
