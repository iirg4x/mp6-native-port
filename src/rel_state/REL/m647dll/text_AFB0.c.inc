#include "mp6_rel_state.h"
#define MP6_M647_TEXT_AFB0_BSS(X) \
    X(lbl_1_bss_364) \
    X(lbl_1_bss_358)
#define MP6_M647_TEXT_AFB0_DATA(X) \
    X(lbl_1_data_5A8)
MP6_REL_RESET_DEFINE(mp6_m647_text_AFB0_reset, MP6_M647_TEXT_AFB0_BSS, MP6_M647_TEXT_AFB0_DATA)
