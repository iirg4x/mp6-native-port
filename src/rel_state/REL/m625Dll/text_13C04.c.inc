#include "mp6_rel_state.h"
#define MP6_M625_TEXT_13C04_BSS(X) \
    X(lbl_1_bss_818) \
    X(lbl_1_bss_816) \
    X(lbl_1_bss_814) \
    X(lbl_1_bss_810)
#define MP6_M625_TEXT_13C04_DATA(X) \
    X(lbl_1_data_448)
MP6_REL_RESET_DEFINE(mp6_m625_text_13C04_reset, MP6_M625_TEXT_13C04_BSS, MP6_M625_TEXT_13C04_DATA)
