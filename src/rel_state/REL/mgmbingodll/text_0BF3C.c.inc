#include "mp6_rel_state.h"
#define MP6_MGMBINGO_TEXT_0BF3C_BSS(X) \
    X(lbl_1_bss_ACC) \
    X(lbl_1_bss_AA4) \
    X(lbl_1_bss_A84) \
    X(lbl_1_bss_A82) \
    X(lbl_1_bss_A72) \
    X(lbl_1_bss_A70)
#define MP6_MGMBINGO_TEXT_0BF3C_DATA(X) \
    X(lbl_1_data_848) \
    X(lbl_1_data_868) \
    X(lbl_1_data_898) \
    X(lbl_1_data_8B0) \
    X(lbl_1_data_9D0) \
    X(lbl_1_data_9F0) \
    X(lbl_1_data_9FC)
MP6_REL_RESET_DEFINE(mp6_mgmbingo_text_0BF3C_reset,
                     MP6_MGMBINGO_TEXT_0BF3C_BSS,
                     MP6_MGMBINGO_TEXT_0BF3C_DATA)
