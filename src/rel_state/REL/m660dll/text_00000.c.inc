#include "mp6_rel_state.h"
#define MP6_M660_TEXT_00000_BSS(X) \
    X(lbl_1_bss_2680) \
    X(lbl_1_bss_2678) \
    X(lbl_1_bss_2530) \
    X(lbl_1_bss_24F4) \
    X(lbl_1_bss_24A4) \
    X(lbl_1_bss_2484) \
    X(lbl_1_bss_60C) \
    X(lbl_1_bss_544) \
    X(lbl_1_bss_2DC) \
    X(lbl_1_bss_1F4) \
    X(lbl_1_bss_1E8) \
    X(lbl_1_bss_80) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_0)
#define MP6_M660_TEXT_00000_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_F6)
MP6_REL_RESET_DEFINE(mp6_m660_text_00000_reset, MP6_M660_TEXT_00000_BSS, MP6_M660_TEXT_00000_DATA)

void mp6_m660_text_06BF8_reset(void);
void mp6_m660_text_0A3E4_reset(void);
void mp6_m660_text_0E6F4_reset(void);
void mp6_m660_reset(void)
{
    mp6_m660_text_00000_reset();
    mp6_m660_text_06BF8_reset();
    mp6_m660_text_0A3E4_reset();
    mp6_m660_text_0E6F4_reset();
}
