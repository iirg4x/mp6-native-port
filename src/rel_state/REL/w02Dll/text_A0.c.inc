#include "mp6_rel_state.h"
#define MP6_W02_TEXT_A0_BSS(X) \
    X(lbl_1_bss_5C) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_W02_TEXT_A0_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_24) \
    X(lbl_1_data_48) \
    X(lbl_1_data_78) \
    X(lbl_1_data_C0) \
    X(lbl_1_data_1C0) \
    X(lbl_1_data_234) \
    X(lbl_1_data_284) \
    X(lbl_1_data_2F0) \
    X(lbl_1_data_32C) \
    X(lbl_1_data_348) \
    X(lbl_1_data_418) \
    X(lbl_1_data_490) \
    X(lbl_1_data_4B8) \
    X(lbl_1_data_4D4) \
    X(lbl_1_data_4DC) \
    X(lbl_1_data_4E8) \
    X(lbl_1_data_4F4) \
    X(lbl_1_data_4F8) \
    X(lbl_1_data_504) \
    X(lbl_1_data_510) \
    X(lbl_1_data_554) \
    X(lbl_1_data_584) \
    X(lbl_1_data_594) \
    X(lbl_1_data_5BC) \
    X(lbl_1_data_5C8) \
    X(lbl_1_data_5D4) \
    X(lbl_1_data_5D8) \
    X(lbl_1_data_5E4) \
    X(lbl_1_data_5F0) \
    X(lbl_1_data_5F4) \
    X(lbl_1_data_600) \
    X(lbl_1_data_60C) \
    X(lbl_1_data_610) \
    X(lbl_1_data_61C) \
    X(lbl_1_data_628) \
    X(lbl_1_data_62C) \
    X(lbl_1_data_638) \
    X(lbl_1_data_644) \
    X(lbl_1_data_648) \
    X(lbl_1_data_64C) \
    X(lbl_1_data_6C0) \
    X(lbl_1_data_6F8) \
    X(lbl_1_data_704) \
    X(lbl_1_data_7F4) \
    X(lbl_1_data_888) \
    X(lbl_1_data_8C4) \
    X(lbl_1_data_900) \
    X(lbl_1_data_930) \
    X(lbl_1_data_960) \
    X(lbl_1_data_9E0) \
    X(lbl_1_data_A4C) \
    X(lbl_1_data_A84) \
    X(lbl_1_data_A90) \
    X(lbl_1_data_A9C) \
    X(lbl_1_data_AC0) \
    X(lbl_1_data_B08) \
    X(lbl_1_data_B14) \
    X(lbl_1_data_B34) \
    X(lbl_1_data_B3C) \
    X(lbl_1_data_B94) \
    X(lbl_1_data_BA8) \
    X(lbl_1_data_BE0) \
    X(lbl_1_data_C10) \
    X(lbl_1_data_C64) \
    X(lbl_1_data_CB4) \
    X(lbl_1_data_CC0) \
    X(lbl_1_data_CCC) \
    X(lbl_1_data_D44) \
    X(lbl_1_data_D74) \
    X(lbl_1_data_874)
MP6_REL_RESET_DEFINE(mp6_w02_text_A0_reset, MP6_W02_TEXT_A0_BSS, MP6_W02_TEXT_A0_DATA)

void mp6_w02_text_A0_reset(void);
void mp6_w02_reset(void)
{
    mp6_w02_text_A0_reset();
}
