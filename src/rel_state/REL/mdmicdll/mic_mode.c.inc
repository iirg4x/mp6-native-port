#include "mp6_rel_state.h"
#define MP6_MDMIC_MIC_MODE_BSS(X) \
    X(lbl_1_bss_A50) \
    X(lbl_1_bss_A4C) \
    X(lbl_1_bss_A44) \
    X(lbl_1_bss_9F4) \
    X(lbl_1_bss_9DC) \
    X(lbl_1_bss_9A4) \
    X(lbl_1_bss_924) \
    X(lbl_1_bss_900) \
    X(lbl_1_bss_888) \
    X(lbl_1_bss_814) \
    X(lbl_1_bss_29C) \
    X(lbl_1_bss_298) \
    X(lbl_1_bss_238) \
    X(lbl_1_bss_1D8) \
    X(lbl_1_bss_118) \
    X(lbl_1_bss_114) \
    X(lbl_1_bss_E4) \
    X(lbl_1_bss_B4) \
    X(lbl_1_bss_9C) \
    X(lbl_1_bss_90) \
    X(lbl_1_bss_60) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_42) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2E) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_2A) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_MDMIC_MIC_MODE_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_24) \
    X(lbl_1_data_A4) \
    X(lbl_1_data_C8) \
    X(lbl_1_data_848) \
    X(lbl_1_data_8BC) \
    X(lbl_1_data_C7C) \
    X(lbl_1_data_C80) \
    X(lbl_1_data_CA2) \
    X(lbl_1_data_CA8) \
    X(lbl_1_data_CB4) \
    X(lbl_1_data_CC0) \
    X(lbl_1_data_CF0) \
    X(lbl_1_data_D01) \
    X(lbl_1_data_D13) \
    X(lbl_1_data_D25) \
    X(lbl_1_data_D33) \
    X(lbl_1_data_D36) \
    X(lbl_1_data_D40) \
    X(lbl_1_data_D6F) \
    X(lbl_1_data_D9D) \
    X(lbl_1_data_DCD) \
    X(lbl_1_data_DEF) \
    X(lbl_1_data_E09) \
    X(lbl_1_data_E23) \
    X(lbl_1_data_E32) \
    X(lbl_1_data_E38) \
    X(lbl_1_data_E44) \
    X(lbl_1_data_E56) \
    X(lbl_1_data_E5D) \
    X(lbl_1_data_E64) \
    X(lbl_1_data_E70) \
    X(lbl_1_data_E78) \
    X(lbl_1_data_E80) \
    X(lbl_1_data_E88) \
    X(lbl_1_data_E90) \
    X(lbl_1_data_F56) \
    X(lbl_1_data_F60)
MP6_REL_RESET_DEFINE(mp6_mdmic_mic_mode_reset,
                     MP6_MDMIC_MIC_MODE_BSS,
                     MP6_MDMIC_MIC_MODE_DATA)
void mp6_mdmic_effects_reset(void);

void mp6_mdmic_reset(void)
{
    mp6_mdmic_effects_reset();
    mp6_mdmic_mic_mode_reset();
}
