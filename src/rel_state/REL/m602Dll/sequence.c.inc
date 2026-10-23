#include "mp6_rel_state.h"
#define MP6_M602_SEQUENCE_BSS(X) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_2) \
    X(lbl_1_bss_0)
#define MP6_M602_SEQUENCE_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_78) \
    X(lbl_1_data_9C)
MP6_REL_RESET_DEFINE(mp6_m602_sequence_reset, MP6_M602_SEQUENCE_BSS, MP6_M602_SEQUENCE_DATA)

void mp6_m602_support_reset(void);
void mp6_m602_stage_reset(void);
void mp6_m602_effect_reset(void);
void mp6_m602_player_reset(void);
void mp6_m602_ui_reset(void);
void mp6_m602_reset(void)
{
    mp6_m602_sequence_reset();
    mp6_m602_support_reset();
    mp6_m602_stage_reset();
    mp6_m602_effect_reset();
    mp6_m602_player_reset();
    mp6_m602_ui_reset();
}
