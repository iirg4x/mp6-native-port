#include "mp6_rel_state.h"
#define MP6_MDPRESULT_MDPRESULT_BSS(X) \
    X(lbl_1_bss_130E) \
    X(lbl_1_bss_1304) \
    X(lbl_1_bss_12BC) \
    X(lbl_1_bss_12B0) \
    X(lbl_1_bss_12A0) \
    X(lbl_1_bss_129C) \
    X(lbl_1_bss_1298) \
    X(lbl_1_bss_1278) \
    X(lbl_1_bss_1248) \
    X(lbl_1_bss_11AC) \
    X(lbl_1_bss_11A0) \
    X(lbl_1_bss_117C) \
    X(lbl_1_bss_10D4) \
    X(lbl_1_bss_10CC) \
    X(lbl_1_bss_109C) \
    X(lbl_1_bss_F9C) \
    X(lbl_1_bss_D9C) \
    X(lbl_1_bss_ADC) \
    X(lbl_1_bss_ABC) \
    X(lbl_1_bss_AAC) \
    X(lbl_1_bss_8EC) \
    X(lbl_1_bss_8AC) \
    X(lbl_1_bss_81C) \
    X(lbl_1_bss_71C) \
    X(lbl_1_bss_714) \
    X(lbl_1_bss_70C) \
    X(lbl_1_bss_66C) \
    X(lbl_1_bss_3D2) \
    X(lbl_1_bss_21A) \
    X(lbl_1_bss_62) \
    X(lbl_1_bss_60) \
    X(lbl_1_bss_5C) \
    X(lbl_1_bss_58) \
    X(lbl_1_bss_56) \
    X(lbl_1_bss_54) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
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
#define MP6_MDPRESULT_MDPRESULT_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_C0) \
    X(lbl_1_data_15C) \
    X(lbl_1_data_168) \
    X(lbl_1_data_3A8) \
    X(lbl_1_data_3B4) \
    X(lbl_1_data_5F4) \
    X(lbl_1_data_620) \
    X(lbl_1_data_624) \
    X(lbl_1_data_646) \
    X(lbl_1_data_64C) \
    X(lbl_1_data_654) \
    X(lbl_1_data_666) \
    X(lbl_1_data_678) \
    X(lbl_1_data_67D) \
    X(lbl_1_data_682) \
    X(lbl_1_data_684) \
    X(lbl_1_data_68C) \
    X(lbl_1_data_6A2) \
    X(lbl_1_data_6D0) \
    X(lbl_1_data_6E0) \
    X(lbl_1_data_6ED) \
    X(lbl_1_data_6F7) \
    X(lbl_1_data_703) \
    X(lbl_1_data_70A) \
    X(lbl_1_data_719) \
    X(lbl_1_data_750) \
    X(lbl_1_data_754) \
    X(lbl_1_data_758) \
    X(lbl_1_data_75C) \
    X(lbl_1_data_76C)
MP6_REL_RESET_DEFINE(mp6_mdpresult_mdpresult_reset, MP6_MDPRESULT_MDPRESULT_BSS, MP6_MDPRESULT_MDPRESULT_DATA)

void mp6_mdpresult_utility_reset(void);
void mp6_mdpresult_reset(void)
{
    mp6_mdpresult_mdpresult_reset();
    mp6_mdpresult_utility_reset();
}
