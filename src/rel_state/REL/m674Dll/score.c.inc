#include "mp6_rel_state.h"
#define MP6_M674_SCORE_BSS(X) \
    X(lbl_1_bss_6C8)
#define MP6_M674_SCORE_DATA(X) \
    X(lbl_1_data_680)
MP6_REL_RESET_DEFINE(mp6_m674_score_reset, MP6_M674_SCORE_BSS, MP6_M674_SCORE_DATA)
