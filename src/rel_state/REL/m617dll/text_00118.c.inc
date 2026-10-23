#include "mp6_rel_state.h"
#define MP6_M617_TEXT_00118_BSS(X) \
    X(lbl_1_bss_38F0) \
    X(lbl_1_bss_38E8) \
    X(lbl_1_bss_38E4) \
    X(lbl_1_bss_379C) \
    X(lbl_1_bss_3760) \
    X(lbl_1_bss_3710) \
    X(lbl_1_bss_3708) \
    X(lbl_1_bss_E88) \
    X(lbl_1_bss_DC0) \
    X(lbl_1_bss_AB0) \
    X(lbl_1_bss_9C8) \
    X(lbl_1_bss_9BC) \
    X(lbl_1_bss_458) \
    X(lbl_1_bss_398) \
    X(lbl_1_bss_370) \
    X(lbl_1_bss_0)
#define MP6_M617_TEXT_00118_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_F6) \
    X(lbl_1_data_FE)
MP6_REL_RESET_DEFINE(mp6_m617_text_00118_reset, MP6_M617_TEXT_00118_BSS, MP6_M617_TEXT_00118_DATA)

void mp6_m617_text_093D0_reset(void);
void mp6_m617_reset(void)
{
    mp6_m617_text_00118_reset();
    mp6_m617_text_093D0_reset();
}
