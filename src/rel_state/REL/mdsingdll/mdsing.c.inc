#include "mp6_rel_state.h"
#define MP6_MDSING_MDSING_BSS(X) \
    X(lbl_1_bss_13A4) \
    X(lbl_1_bss_13A0) \
    X(lbl_1_bss_1398) \
    X(lbl_1_bss_1348) \
    X(lbl_1_bss_1340) \
    X(lbl_1_bss_1308) \
    X(lbl_1_bss_12A4) \
    X(lbl_1_bss_1292) \
    X(lbl_1_bss_1258) \
    X(lbl_1_bss_11F4) \
    X(lbl_1_bss_E74) \
    X(lbl_1_bss_E14) \
    X(lbl_1_bss_DB4) \
    X(lbl_1_bss_CF4) \
    X(lbl_1_bss_CDC) \
    X(lbl_1_bss_CAC) \
    X(lbl_1_bss_C94) \
    X(lbl_1_bss_C80) \
    X(lbl_1_bss_880) \
    X(lbl_1_bss_480) \
    X(lbl_1_bss_47C) \
    X(lbl_1_bss_478) \
    X(lbl_1_bss_474) \
    X(lbl_1_bss_3E4) \
    X(lbl_1_bss_3E0) \
    X(lbl_1_bss_3DC) \
    X(lbl_1_bss_3D8) \
    X(lbl_1_bss_358) \
    X(lbl_1_bss_58) \
    X(lbl_1_bss_54) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_4A) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3E) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_32) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_22) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_MDSING_MDSING_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_320) \
    X(lbl_1_data_32C) \
    X(lbl_1_data_390) \
    X(lbl_1_data_3A4) \
    X(lbl_1_data_744) \
    X(lbl_1_data_7A8) \
    X(lbl_1_data_8EC) \
    X(lbl_1_data_8F0) \
    X(lbl_1_data_912) \
    X(lbl_1_data_918) \
    X(lbl_1_data_928) \
    X(lbl_1_data_95A) \
    X(lbl_1_data_96B) \
    X(lbl_1_data_97D) \
    X(lbl_1_data_98F) \
    X(lbl_1_data_99D) \
    X(lbl_1_data_99F) \
    X(lbl_1_data_9A3) \
    X(lbl_1_data_9D3) \
    X(lbl_1_data_A02) \
    X(lbl_1_data_A33) \
    X(lbl_1_data_A55) \
    X(lbl_1_data_A6F) \
    X(lbl_1_data_A89) \
    X(lbl_1_data_A98) \
    X(lbl_1_data_A9C) \
    X(lbl_1_data_AA8) \
    X(lbl_1_data_ABA) \
    X(lbl_1_data_AC1) \
    X(lbl_1_data_AC8) \
    X(lbl_1_data_AD4) \
    X(lbl_1_data_ADC) \
    X(lbl_1_data_AE4) \
    X(lbl_1_data_AEC) \
    X(lbl_1_data_AF4) \
    X(lbl_1_data_BBA) \
    X(lbl_1_data_BE6) \
    X(lbl_1_data_C0F) \
    X(lbl_1_data_C45) \
    X(lbl_1_data_CC8) \
    X(lbl_1_data_CCC) \
    X(lbl_1_data_CCE)
MP6_REL_RESET_DEFINE(mp6_mdsing_mdsing_reset,
                     MP6_MDSING_MDSING_BSS,
                     MP6_MDSING_MDSING_DATA)
void mp6_mdsing_stage_reset(void);

void mp6_mdsing_reset(void)
{
    mp6_mdsing_mdsing_reset();
    mp6_mdsing_stage_reset();
}
