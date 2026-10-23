#include "mp6_rel_state.h"
#define MP6_M612_M612_BSS(X) \
    X(lbl_1_bss_4AC) \
    X(lbl_1_bss_49C) \
    X(lbl_1_bss_48C) \
    X(lbl_1_bss_44C) \
    X(lbl_1_bss_43C) \
    X(lbl_1_bss_42C) \
    X(lbl_1_bss_41C) \
    X(lbl_1_bss_40C) \
    X(lbl_1_bss_3FC) \
    X(lbl_1_bss_3EC) \
    X(lbl_1_bss_3DC) \
    X(lbl_1_bss_3CC) \
    X(lbl_1_bss_3BC) \
    X(lbl_1_bss_3AC) \
    X(lbl_1_bss_39C) \
    X(lbl_1_bss_38C) \
    X(lbl_1_bss_37C) \
    X(lbl_1_bss_36C) \
    X(lbl_1_bss_30C) \
    X(lbl_1_bss_308) \
    X(lbl_1_bss_304) \
    X(lbl_1_bss_300) \
    X(lbl_1_bss_2FC) \
    X(lbl_1_bss_2F8) \
    X(lbl_1_bss_2F4) \
    X(lbl_1_bss_2F0) \
    X(lbl_1_bss_2EC) \
    X(lbl_1_bss_2E8) \
    X(lbl_1_bss_2C0) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_40) \
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
#define MP6_M612_M612_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_10) \
    X(lbl_1_data_20) \
    X(lbl_1_data_30) \
    X(lbl_1_data_40) \
    X(lbl_1_data_50) \
    X(lbl_1_data_74) \
    X(lbl_1_data_84) \
    X(lbl_1_data_8C) \
    X(lbl_1_data_90) \
    X(lbl_1_data_128) \
    X(lbl_1_data_138) \
    X(lbl_1_data_148) \
    X(lbl_1_data_158) \
    X(lbl_1_data_168)
MP6_REL_RESET_DEFINE(mp6_m612_m612_reset, MP6_M612_M612_BSS, MP6_M612_M612_DATA)

void mp6_m612_tables_reset(void);
void mp6_m612_reset(void)
{
    mp6_m612_m612_reset();
    mp6_m612_tables_reset();
}
