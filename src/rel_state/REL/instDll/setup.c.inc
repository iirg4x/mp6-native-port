#include "mp6_rel_state.h"
#define MP6_INST_SETUP_BSS(X) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_36) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_26) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_A) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_6) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_INST_SETUP_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_8) \
    X(lbl_1_data_24) \
    X(lbl_1_data_40) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_80) \
    X(lbl_1_data_A8)
MP6_REL_RESET_DEFINE(mp6_inst_setup_reset, MP6_INST_SETUP_BSS, MP6_INST_SETUP_DATA)

void mp6_inst_stage_reset(void);
void mp6_inst_display_reset(void);
void mp6_inst_name_reset(void);
void mp6_inst_reset(void)
{
    mp6_inst_setup_reset();
    mp6_inst_stage_reset();
    mp6_inst_display_reset();
    mp6_inst_name_reset();
}
