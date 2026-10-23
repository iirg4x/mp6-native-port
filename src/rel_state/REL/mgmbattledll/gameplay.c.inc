#include "mp6_rel_state.h"
#define MP6_MGMBATTLE_GAMEPLAY_BSS(X) \
    X(lbl_1_bss_4D4) \
    X(lbl_1_bss_494) \
    X(lbl_1_bss_492) \
    X(lbl_1_bss_486) \
    X(lbl_1_bss_484) \
    X(lbl_1_bss_478) \
    X(lbl_1_bss_476) \
    X(lbl_1_bss_470) \
    X(lbl_1_bss_468) \
    X(lbl_1_bss_448) \
    X(lbl_1_bss_446) \
    X(lbl_1_bss_444) \
    X(lbl_1_bss_43C) \
    X(lbl_1_bss_43A) \
    X(lbl_1_bss_432) \
    X(lbl_1_bss_42A) \
    X(lbl_1_bss_3EA) \
    X(lbl_1_bss_3E8)
#define MP6_MGMBATTLE_GAMEPLAY_DATA(X) \
    X(lbl_1_data_3F8) \
    X(lbl_1_data_428) \
    X(lbl_1_data_4B8) \
    X(lbl_1_data_4E8) \
    X(lbl_1_data_4F0) \
    X(lbl_1_data_4F8) \
    X(lbl_1_data_628) \
    X(lbl_1_data_630) \
    X(lbl_1_data_638) \
    X(lbl_1_data_640) \
    X(lbl_1_data_680) \
    X(lbl_1_data_6A4) \
    X(lbl_1_data_6A8) \
    X(lbl_1_data_6B4) \
    X(lbl_1_data_6C0) \
    X(lbl_1_data_6CC) \
    X(lbl_1_data_6D8) \
    X(lbl_1_data_6E4) \
    X(lbl_1_data_774) \
    X(lbl_1_data_7D4) \
    X(lbl_1_data_834) \
    X(lbl_1_data_84C)
MP6_REL_RESET_DEFINE(mp6_mgmbattle_gameplay_reset,
                     MP6_MGMBATTLE_GAMEPLAY_BSS,
                     MP6_MGMBATTLE_GAMEPLAY_DATA)
