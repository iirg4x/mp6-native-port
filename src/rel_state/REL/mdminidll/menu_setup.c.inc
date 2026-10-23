#include "mp6_rel_state.h"
#define MP6_MDMINI_MENU_BSS(X) \
    X(lbl_1_bss_864) \
    X(lbl_1_bss_860) \
    X(lbl_1_bss_858) \
    X(lbl_1_bss_808) \
    X(lbl_1_bss_804) \
    X(lbl_1_bss_7CC) \
    X(lbl_1_bss_764) \
    X(lbl_1_bss_752) \
    X(lbl_1_bss_71C) \
    X(lbl_1_bss_6C8) \
    X(lbl_1_bss_230) \
    X(lbl_1_bss_1D0) \
    X(lbl_1_bss_170) \
    X(lbl_1_bss_B0) \
    X(lbl_1_bss_80) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
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
#define MP6_MDMINI_MENU_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_2E) \
    X(lbl_1_data_44) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_7C) \
    X(lbl_1_data_94) \
    X(lbl_1_data_98) \
    X(lbl_1_data_100) \
    X(lbl_1_data_114) \
    X(lbl_1_data_474) \
    X(lbl_1_data_4C8) \
    X(lbl_1_data_690) \
    X(lbl_1_data_694) \
    X(lbl_1_data_6B6) \
    X(lbl_1_data_6C2) \
    X(lbl_1_data_6F3) \
    X(lbl_1_data_704) \
    X(lbl_1_data_716) \
    X(lbl_1_data_728) \
    X(lbl_1_data_736) \
    X(lbl_1_data_738) \
    X(lbl_1_data_767) \
    X(lbl_1_data_798) \
    X(lbl_1_data_7C8) \
    X(lbl_1_data_7EA) \
    X(lbl_1_data_804) \
    X(lbl_1_data_81E) \
    X(lbl_1_data_82C) \
    X(lbl_1_data_830) \
    X(lbl_1_data_83C) \
    X(lbl_1_data_84E) \
    X(lbl_1_data_860) \
    X(lbl_1_data_867) \
    X(lbl_1_data_86E) \
    X(lbl_1_data_87A) \
    X(lbl_1_data_882) \
    X(lbl_1_data_88A) \
    X(lbl_1_data_892) \
    X(lbl_1_data_89A) \
    X(lbl_1_data_960)
MP6_REL_RESET_DEFINE(mp6_mdmini_menu_reset, MP6_MDMINI_MENU_BSS, MP6_MDMINI_MENU_DATA)

void mp6_mdmini_particles_reset(void);
void mp6_mdmini_reset_data(void) {
    mp6_mdmini_menu_reset();
    mp6_mdmini_particles_reset();
}
