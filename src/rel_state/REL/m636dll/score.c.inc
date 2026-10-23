#include "mp6_rel_state.h"
#define MP6_M636_SCORE_BSS(X) X(lbl_1_bss_0)
#define MP6_M636_SCORE_DATA(X) X(lbl_1_data_230) \
    X(lbl_1_data_240) \
    X(lbl_1_data_270) \
    X(lbl_1_data_280) \
    X(lbl_1_data_288) \
    X(lbl_1_data_294) \
    X(lbl_1_data_2A0) \
    X(lbl_1_data_2AC) \
    X(lbl_1_data_36C) \
    X(lbl_1_data_378)
MP6_REL_RESET_DEFINE(mp6_m636_score_reset, MP6_M636_SCORE_BSS, MP6_M636_SCORE_DATA)
