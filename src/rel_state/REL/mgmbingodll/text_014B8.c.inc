#include "mp6_rel_state.h"
#define MP6_MGMBINGO_TEXT_014B8_BSS(X) \
    X(lbl_1_bss_C8)
#define MP6_MGMBINGO_TEXT_014B8_DATA(X) \
    X(lbl_1_data_90) \
    X(lbl_1_data_B0) \
    X(lbl_1_data_D8) \
    X(lbl_1_data_DC) \
    X(lbl_1_data_114) \
    X(lbl_1_data_2A0)
MP6_REL_RESET_DEFINE(mp6_mgmbingo_text_014B8_reset,
                     MP6_MGMBINGO_TEXT_014B8_BSS,
                     MP6_MGMBINGO_TEXT_014B8_DATA)
