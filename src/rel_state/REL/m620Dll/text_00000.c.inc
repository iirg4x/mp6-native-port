#include "mp6_rel_state.h"
#define MP6_M620_TEXT_00000_BSS(X) \
    X(lbl_1_bss_660) \
    X(lbl_1_bss_658) \
    X(lbl_1_bss_654) \
    X(lbl_1_bss_650) \
    X(lbl_1_bss_648) \
    X(lbl_1_bss_448) \
    X(lbl_1_bss_438) \
    X(lbl_1_bss_3AC) \
    X(lbl_1_bss_3A8) \
    X(lbl_1_bss_3A4) \
    X(lbl_1_bss_394) \
    X(lbl_1_bss_388) \
    X(lbl_1_bss_384) \
    X(lbl_1_bss_380) \
    X(lbl_1_bss_37C) \
    X(lbl_1_bss_378) \
    X(lbl_1_bss_338) \
    X(lbl_1_bss_330) \
    X(lbl_1_bss_320) \
    X(lbl_1_bss_310) \
    X(lbl_1_bss_30C) \
    X(lbl_1_bss_308) \
    X(lbl_1_bss_300) \
    X(lbl_1_bss_2FC) \
    X(lbl_1_bss_2F8) \
    X(lbl_1_bss_2F4) \
    X(lbl_1_bss_2F0) \
    X(lbl_1_bss_1B0) \
    X(lbl_1_bss_70) \
    X(lbl_1_bss_6C) \
    X(lbl_1_bss_68) \
    X(lbl_1_bss_64) \
    X(lbl_1_bss_60) \
    X(lbl_1_bss_5C) \
    X(lbl_1_bss_58) \
    X(lbl_1_bss_54) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_0)
#define MP6_M620_TEXT_00000_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_1C) \
    X(lbl_1_data_28) \
    X(lbl_1_data_34) \
    X(lbl_1_data_38) \
    X(lbl_1_data_80) \
    X(lbl_1_data_A8) \
    X(lbl_1_data_B4)
MP6_REL_RESET_DEFINE(mp6_m620_text_00000_reset, MP6_M620_TEXT_00000_BSS, MP6_M620_TEXT_00000_DATA)

void mp6_m620_text_0117C_reset(void);
void mp6_m620_text_64A8_reset(void);
void mp6_m620_text_0B7D4_reset(void);
void mp6_m620_text_0ED3C_reset(void);
void mp6_m620_reset(void)
{
    mp6_m620_text_00000_reset();
    mp6_m620_text_0117C_reset();
    mp6_m620_text_64A8_reset();
    mp6_m620_text_0B7D4_reset();
    mp6_m620_text_0ED3C_reset();
}
