#include "mp6_rel_state.h"
#define MP6_S01_S01_BSS(X) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_2) \
    X(lbl_1_bss_0)
#define MP6_S01_S01_DATA(X) \
    X(mp6_s01_fogWide) \
    X(lbl_1_data_0) \
    X(lbl_1_data_C) \
    X(lbl_1_data_18) \
    X(lbl_1_data_24) \
    X(lbl_1_data_30) \
    X(lbl_1_data_54) \
    X(lbl_1_data_A4) \
    X(lbl_1_data_B0) \
    X(lbl_1_data_C4) \
    X(lbl_1_data_F4) \
    X(lbl_1_data_10C) \
    X(lbl_1_data_114) \
    X(lbl_1_data_11C) \
    X(lbl_1_data_128)
MP6_REL_RESET_DEFINE(mp6_s01_s01_reset, MP6_S01_S01_BSS, MP6_S01_S01_DATA)

void mp6_s01_s01_reset(void);
void mp6_s01_reset(void)
{
    mp6_s01_s01_reset();
}
