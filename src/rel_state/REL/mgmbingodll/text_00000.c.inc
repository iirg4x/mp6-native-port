#include "mp6_rel_state.h"
#define MP6_MGMBINGO_TEXT_00000_BSS(X) \
    X(lbl_1_bss_A4) \
    X(lbl_1_bss_A2) \
    X(lbl_1_bss_A0) \
    X(lbl_1_bss_9C) \
    X(lbl_1_bss_98) \
    X(lbl_1_bss_94) \
    X(lbl_1_bss_90) \
    X(lbl_1_bss_8C) \
    X(lbl_1_bss_88) \
    X(lbl_1_bss_84) \
    X(lbl_1_bss_2) \
    X(lbl_1_bss_0)
#define MP6_MGMBINGO_TEXT_00000_DATA(X)
MP6_REL_RESET_DEFINE(mp6_mgmbingo_text_00000_reset,
                     MP6_MGMBINGO_TEXT_00000_BSS,
                     MP6_MGMBINGO_TEXT_00000_DATA)
