#include "mp6_rel_state.h"
#define MP6_M666_TEXT_000A0_BSS(X) \
    X(lbl_1_bss_DC) \
    X(lbl_1_bss_98) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_2) \
    X(lbl_1_bss_0)
#define MP6_M666_TEXT_000A0_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_3C)
MP6_REL_RESET_DEFINE(mp6_m666_text_000A0_reset, MP6_M666_TEXT_000A0_BSS, MP6_M666_TEXT_000A0_DATA)

void mp6_m666_text_008BC_reset(void);
void mp6_m666_reset(void)
{
    mp6_m666_text_000A0_reset();
    mp6_m666_text_008BC_reset();
}
