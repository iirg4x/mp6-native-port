#include "mp6_rel_state.h"
#define MP6_MGMBINGO_TEXT_00E9C_BSS(X) \
    X(lbl_1_bss_AC) \
    X(lbl_1_bss_A8)
#define MP6_MGMBINGO_TEXT_00E9C_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_mgmbingo_text_00E9C_reset,
                     MP6_MGMBINGO_TEXT_00E9C_BSS,
                     MP6_MGMBINGO_TEXT_00E9C_DATA)
