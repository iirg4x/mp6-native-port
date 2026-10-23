#include "mp6_rel_state.h"
#define MP6_ENDING_ENDING_BSS(X) \
    X(lbl_1_bss_1DDC) \
    X(lbl_1_bss_1D5C) \
    X(lbl_1_bss_1ADC) \
    X(lbl_1_bss_1A5C) \
    X(lbl_1_bss_1A56) \
    X(lbl_1_bss_1A52) \
    X(lbl_1_bss_1A48) \
    X(lbl_1_bss_1A20) \
    X(lbl_1_bss_1A1C) \
    X(lbl_1_bss_1A08) \
    X(lbl_1_bss_19F4) \
    X(lbl_1_bss_1834) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2E) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_26) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_ENDING_ENDING_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_38) \
    X(lbl_1_data_10E)
MP6_REL_RESET_DEFINE(mp6_ending_ending_reset, MP6_ENDING_ENDING_BSS, MP6_ENDING_ENDING_DATA)

void mp6_ending_ending_reset(void);
void mp6_ending_effects_reset(void);
void mp6_ending_reset(void)
{
    mp6_ending_ending_reset();
    mp6_ending_effects_reset();
}
