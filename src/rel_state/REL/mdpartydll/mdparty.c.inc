#include "mp6_rel_state.h"
#define MP6_MDPARTY_MDPARTY_BSS(X) \
    X(lbl_1_bss_A6C) \
    X(lbl_1_bss_A68) \
    X(lbl_1_bss_A60) \
    X(lbl_1_bss_A10) \
    X(lbl_1_bss_9F4) \
    X(lbl_1_bss_9BC) \
    X(lbl_1_bss_93C) \
    X(lbl_1_bss_918) \
    X(lbl_1_bss_8A4) \
    X(lbl_1_bss_800) \
    X(lbl_1_bss_288) \
    X(lbl_1_bss_228) \
    X(lbl_1_bss_1C8) \
    X(lbl_1_bss_108) \
    X(lbl_1_bss_104) \
    X(lbl_1_bss_D4) \
    X(lbl_1_bss_A4) \
    X(lbl_1_bss_8C) \
    X(lbl_1_bss_80) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2E) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_2A) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_MDPARTY_MDPARTY_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_1C) \
    X(lbl_1_data_9C) \
    X(lbl_1_data_C0) \
    X(lbl_1_data_800) \
    X(lbl_1_data_8A4) \
    X(lbl_1_data_BC8) \
    X(lbl_1_data_BCC) \
    X(lbl_1_data_BEE) \
    X(lbl_1_data_BFC) \
    X(lbl_1_data_C34) \
    X(lbl_1_data_C66) \
    X(lbl_1_data_C77) \
    X(lbl_1_data_C89) \
    X(lbl_1_data_C9B) \
    X(lbl_1_data_CA9) \
    X(lbl_1_data_CAB) \
    X(lbl_1_data_CDC) \
    X(lbl_1_data_D0C) \
    X(lbl_1_data_D3E) \
    X(lbl_1_data_D60) \
    X(lbl_1_data_D7A) \
    X(lbl_1_data_D94) \
    X(lbl_1_data_DA2) \
    X(lbl_1_data_DA8) \
    X(lbl_1_data_DB4) \
    X(lbl_1_data_DC6) \
    X(lbl_1_data_DD8) \
    X(lbl_1_data_DDF) \
    X(lbl_1_data_DE6) \
    X(lbl_1_data_DF2) \
    X(lbl_1_data_DFA) \
    X(lbl_1_data_E02) \
    X(lbl_1_data_E0A) \
    X(lbl_1_data_E12) \
    X(lbl_1_data_ED8)
MP6_REL_RESET_DEFINE(mp6_mdparty_mdparty_reset, MP6_MDPARTY_MDPARTY_BSS, MP6_MDPARTY_MDPARTY_DATA)

void mp6_mdparty_stage_reset(void);
void mp6_mdparty_reset(void)
{
    mp6_mdparty_mdparty_reset();
    mp6_mdparty_stage_reset();
}
