#include "mp6_rel_state.h"
#define MP6_M675_GAMEPLAY_BSS(X) \
    X(lbl_1_bss_534) \
    X(lbl_1_bss_334) \
    X(lbl_1_bss_330) \
    X(lbl_1_bss_32C) \
    X(lbl_1_bss_328) \
    X(lbl_1_bss_324) \
    X(lbl_1_bss_320) \
    X(lbl_1_bss_31C) \
    X(lbl_1_bss_318)
#define MP6_M675_GAMEPLAY_DATA(X) \
    X(lbl_1_data_448) \
    X(lbl_1_data_454) \
    X(lbl_1_data_460) \
    X(lbl_1_data_46C) \
    X(lbl_1_data_4E4) \
    X(lbl_1_data_55C) \
    X(lbl_1_data_740) \
    X(lbl_1_data_750) \
    X(lbl_1_data_75A) \
    X(lbl_1_data_764) \
    X(lbl_1_data_794) \
    X(lbl_1_data_7A8) \
    X(lbl_1_data_7E4) \
    X(lbl_1_data_7F4) \
    X(lbl_1_data_800) \
    X(lbl_1_data_80C) \
    X(lbl_1_data_834)
MP6_REL_RESET_DEFINE(mp6_m675_gameplay_reset, MP6_M675_GAMEPLAY_BSS, MP6_M675_GAMEPLAY_DATA)
