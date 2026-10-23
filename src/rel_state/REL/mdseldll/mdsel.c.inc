#include "mp6_rel_state.h"
#define MP6_MDSEL_MDSEL_BSS(X) \
    X(lbl_1_bss_1A4C) \
    X(lbl_1_bss_1A48) \
    X(lbl_1_bss_1A40) \
    X(lbl_1_bss_1A3C) \
    X(lbl_1_bss_1A38) \
    X(lbl_1_bss_1A36) \
    X(lbl_1_bss_1A30) \
    X(lbl_1_bss_1A2C) \
    X(lbl_1_bss_1A28) \
    X(lbl_1_bss_19EC) \
    X(lbl_1_bss_19DC) \
    X(lbl_1_bss_19AC) \
    X(lbl_1_bss_9BC) \
    X(lbl_1_bss_8AC) \
    X(lbl_1_bss_24C) \
    X(lbl_1_bss_1D4) \
    X(lbl_1_bss_1B0) \
    X(lbl_1_bss_128) \
    X(lbl_1_bss_A0) \
    X(lbl_1_bss_94) \
    X(lbl_1_bss_80) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3E) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_3A) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_MDSEL_MDSEL_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_8) \
    X(lbl_1_data_28) \
    X(lbl_1_data_70) \
    X(lbl_1_data_7C) \
    X(lbl_1_data_80) \
    X(lbl_1_data_A1) \
    X(lbl_1_data_CF) \
    X(lbl_1_data_E0) \
    X(lbl_1_data_F2) \
    X(lbl_1_data_104) \
    X(lbl_1_data_112) \
    X(lbl_1_data_114) \
    X(lbl_1_data_150) \
    X(lbl_1_data_154) \
    X(lbl_1_data_15C) \
    X(lbl_1_data_15E) \
    X(lbl_1_data_169) \
    X(lbl_1_data_1A8) \
    X(lbl_1_data_1E7) \
    X(lbl_1_data_223) \
    X(lbl_1_data_25E) \
    X(lbl_1_data_275) \
    X(lbl_1_data_27F) \
    X(lbl_1_data_2BD) \
    X(lbl_1_data_2D0) \
    X(lbl_1_data_2D2) \
    X(lbl_1_data_308) \
    X(lbl_1_data_333) \
    X(lbl_1_data_35C)
MP6_REL_RESET_DEFINE(mp6_mdsel_mdsel_reset, MP6_MDSEL_MDSEL_BSS, MP6_MDSEL_MDSEL_DATA)

void mp6_mdsel_reset(void)
{
    mp6_mdsel_mdsel_reset();
}
