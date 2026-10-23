#include "mp6_rel_state.h"
#define MP6_MGMFREE_MENU_BSS(X) \
    X(lbl_1_bss_9E0) \
    X(lbl_1_bss_9DC) \
    X(lbl_1_bss_9D8) \
    X(lbl_1_bss_9D0) \
    X(lbl_1_bss_9A0) \
    X(lbl_1_bss_990) \
    X(lbl_1_bss_980) \
    X(lbl_1_bss_580) \
    X(lbl_1_bss_554) \
    X(lbl_1_bss_53E) \
    X(lbl_1_bss_518) \
    X(lbl_1_bss_508) \
    X(lbl_1_bss_4F8) \
    X(lbl_1_bss_4DC) \
    X(lbl_1_bss_4C0) \
    X(lbl_1_bss_4B0) \
    X(lbl_1_bss_4AE) \
    X(lbl_1_bss_4AA) \
    X(lbl_1_bss_4A8) \
    X(lbl_1_bss_A8) \
    X(lbl_1_bss_98) \
    X(lbl_1_bss_94) \
    X(lbl_1_bss_84) \
    X(lbl_1_bss_80) \
    X(lbl_1_bss_7A) \
    X(lbl_1_bss_78) \
    X(lbl_1_bss_6E) \
    X(lbl_1_bss_64) \
    X(lbl_1_bss_4C) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3A) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_MGMFREE_MENU_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_10) \
    X(lbl_1_data_3C) \
    X(lbl_1_data_50) \
    X(lbl_1_data_2B0) \
    X(lbl_1_data_2B8) \
    X(lbl_1_data_380) \
    X(lbl_1_data_388) \
    X(lbl_1_data_38C) \
    X(lbl_1_data_3AE) \
    X(lbl_1_data_3CC) \
    X(lbl_1_data_3CE) \
    X(lbl_1_data_426) \
    X(lbl_1_data_42A) \
    X(lbl_1_data_43E) \
    X(lbl_1_data_451) \
    X(lbl_1_data_46A) \
    X(lbl_1_data_485) \
    X(lbl_1_data_49F) \
    X(lbl_1_data_4DC) \
    X(lbl_1_data_4DE)
MP6_REL_RESET_DEFINE(mp6_mgmfree_menu_reset, MP6_MGMFREE_MENU_BSS, MP6_MGMFREE_MENU_DATA)

void mp6_mgmfree_reset(void)
{
    mp6_mgmfree_menu_reset();
}
