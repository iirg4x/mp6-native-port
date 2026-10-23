#include "mp6_rel_state.h"
#define MP6_M625_TEXT_0_BSS(X)
#define MP6_M625_TEXT_0_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_50) \
    X(lbl_1_data_74) \
    X(lbl_1_data_BC)
MP6_REL_RESET_DEFINE(mp6_m625_text_0_reset, MP6_M625_TEXT_0_BSS, MP6_M625_TEXT_0_DATA)
