#include "mp6_rel_state.h"
#define MP6_MGMBINGO_TEXT_10548_BSS(X) \
    X(lbl_1_bss_AF4) \
    X(lbl_1_bss_AE8) \
    X(lbl_1_bss_AE4) \
    X(lbl_1_bss_AE0)
#define MP6_MGMBINGO_TEXT_10548_DATA(X) \
    X(lbl_1_data_A60)
MP6_REL_RESET_DEFINE(mp6_mgmbingo_text_10548_reset,
                     MP6_MGMBINGO_TEXT_10548_BSS,
                     MP6_MGMBINGO_TEXT_10548_DATA)
