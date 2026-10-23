#include "mp6_rel_state.h"
#define MP6_M665_OPPONENT_BSS(X) \
    X(lbl_1_bss_EA8) \
    X(lbl_1_bss_CA8)
#define MP6_M665_OPPONENT_DATA(X) \
    X(lbl_1_data_338) \
    X(lbl_1_data_340) \
    X(lbl_1_data_34C) \
    X(lbl_1_data_358) \
    X(lbl_1_data_364) \
    X(lbl_1_data_36C) \
    X(lbl_1_data_375) \
    X(lbl_1_data_380) \
    X(lbl_1_data_38B) \
    X(lbl_1_data_3A9)
MP6_REL_RESET_DEFINE(mp6_m665_opponent_reset, MP6_M665_OPPONENT_BSS, MP6_M665_OPPONENT_DATA)
