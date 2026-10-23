#include "mp6_rel_state.h"
#define MP6_OPENING_OPENING_BSS(X) \
    X(lbl_1_bss_1E4) \
    X(lbl_1_bss_1DE) \
    X(lbl_1_bss_1CE) \
    X(lbl_1_bss_1CC) \
    X(lbl_1_bss_1C4) \
    X(lbl_1_bss_1C0) \
    X(lbl_1_bss_1B8) \
    X(lbl_1_bss_7C) \
    X(lbl_1_bss_78) \
    X(lbl_1_bss_60) \
    X(lbl_1_bss_54) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_2E) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_OPENING_OPENING_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_8) \
    X(lbl_1_data_148) \
    X(lbl_1_data_154) \
    X(lbl_1_data_160) \
    X(lbl_1_data_16C) \
    X(lbl_1_data_178) \
    X(lbl_1_data_184) \
    X(lbl_1_data_190) \
    X(lbl_1_data_19E) \
    X(lbl_1_data_1AC) \
    X(lbl_1_data_1B9) \
    X(lbl_1_data_1C9) \
    X(lbl_1_data_1D2) \
    X(lbl_1_data_1DC) \
    X(lbl_1_data_20C) \
    X(lbl_1_data_21E) \
    X(lbl_1_data_230) \
    X(lbl_1_data_242) \
    X(lbl_1_data_254) \
    X(lbl_1_data_266) \
    X(lbl_1_data_278) \
    X(lbl_1_data_28C) \
    X(lbl_1_data_2A0) \
    X(lbl_1_data_2B3) \
    X(lbl_1_data_2C9) \
    X(lbl_1_data_2DC) \
    X(lbl_1_data_2F0) \
    X(lbl_1_data_320) \
    X(lbl_1_data_32C) \
    X(lbl_1_data_338) \
    X(lbl_1_data_346) \
    X(lbl_1_data_355) \
    X(lbl_1_data_369)
MP6_REL_RESET_DEFINE(mp6_opening_opening_reset, MP6_OPENING_OPENING_BSS, MP6_OPENING_OPENING_DATA)

void mp6_opening_opening_reset(void);
void mp6_opening_reset(void)
{
    mp6_opening_opening_reset();
}
