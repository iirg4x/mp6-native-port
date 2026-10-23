#include "mp6_rel_state.h"
#define MP6_M625_TEXT_15C8_BSS(X) \
    X(lbl_1_bss_194) \
    X(lbl_1_bss_0)
#define MP6_M625_TEXT_15C8_DATA(X) \
    X(lbl_1_data_108) \
    X(lbl_1_data_180) \
    X(lbl_1_data_1C0) \
    X(lbl_1_data_1C4)
MP6_REL_RESET_DEFINE(mp6_m625_text_15C8_reset, MP6_M625_TEXT_15C8_BSS, MP6_M625_TEXT_15C8_DATA)
