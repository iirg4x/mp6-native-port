#include "mp6_rel_state.h"
#define MP6_M625_TEXT_11040_BSS(X) \
    X(lbl_1_bss_7C4) \
    X(lbl_1_bss_4B8)
#define MP6_M625_TEXT_11040_DATA(X) \
    X(lbl_1_data_3E8)
MP6_REL_RESET_DEFINE(mp6_m625_text_11040_reset, MP6_M625_TEXT_11040_BSS, MP6_M625_TEXT_11040_DATA)
