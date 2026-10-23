#include "mp6_rel_state.h"
#define MP6_MGMBINGO_TEXT_11410_BSS(X) \
    X(lbl_1_bss_B00) \
    X(lbl_1_bss_AFC) \
    X(lbl_1_bss_AFA) \
    X(lbl_1_bss_AF8)
#define MP6_MGMBINGO_TEXT_11410_DATA(X) \
    X(lbl_1_data_ACC) \
    X(lbl_1_data_AD0) \
    X(lbl_1_data_AD4) \
    X(lbl_1_data_AD8) \
    X(lbl_1_data_AC8)
MP6_REL_RESET_DEFINE(mp6_mgmbingo_text_11410_reset,
                     MP6_MGMBINGO_TEXT_11410_BSS,
                     MP6_MGMBINGO_TEXT_11410_DATA)
void mp6_mgmbingo_text_00000_reset(void);
void mp6_mgmbingo_text_00E9C_reset(void);
void mp6_mgmbingo_text_014B8_reset(void);
void mp6_mgmbingo_text_03B6C_reset(void);
void mp6_mgmbingo_text_0BF3C_reset(void);
void mp6_mgmbingo_text_10548_reset(void);

void mp6_mgmbingo_reset(void)
{
    mp6_mgmbingo_text_00000_reset();
    mp6_mgmbingo_text_00E9C_reset();
    mp6_mgmbingo_text_014B8_reset();
    mp6_mgmbingo_text_03B6C_reset();
    mp6_mgmbingo_text_0BF3C_reset();
    mp6_mgmbingo_text_10548_reset();
    mp6_mgmbingo_text_11410_reset();
}
