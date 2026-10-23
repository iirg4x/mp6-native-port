#include "mp6_rel_state.h"
#define MP6_M647_TEXT_00000_BSS(X) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M647_TEXT_00000_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m647_text_00000_reset, MP6_M647_TEXT_00000_BSS, MP6_M647_TEXT_00000_DATA)

void mp6_m647_text_00A74_reset(void);
void mp6_m647_text_064B8_reset(void);
void mp6_m647_text_AFB0_reset(void);
void mp6_m647_reset(void)
{
    mp6_m647_text_00000_reset();
    mp6_m647_text_00A74_reset();
    mp6_m647_text_064B8_reset();
    mp6_m647_text_AFB0_reset();
}
