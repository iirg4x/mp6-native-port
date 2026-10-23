#include "mp6_rel_state.h"
#define MP6_M608_SEQUENCE_BSS(X) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M608_SEQUENCE_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_60) \
    X(lbl_1_data_98) \
    X(lbl_1_data_D0) \
    X(lbl_1_data_D9) \
    X(lbl_1_data_E9) \
    X(lbl_1_data_F9) \
    X(lbl_1_data_109) \
    X(lbl_1_data_119) \
    X(lbl_1_data_127) \
    X(lbl_1_data_12C) \
    X(lbl_1_data_142) \
    X(lbl_1_data_147) \
    X(lbl_1_data_161) \
    X(lbl_1_data_179) \
    X(lbl_1_data_189) \
    X(lbl_1_data_1B4) \
    X(lbl_1_data_1D1) \
    X(lbl_1_data_1D6) \
    X(lbl_1_data_217) \
    X(lbl_1_data_227) \
    X(lbl_1_data_235) \
    X(lbl_1_data_242) \
    X(lbl_1_data_24E)
MP6_REL_RESET_DEFINE(mp6_m608_sequence_reset, MP6_M608_SEQUENCE_BSS, MP6_M608_SEQUENCE_DATA)

void mp6_m608_setup_reset(void);
void mp6_m608_helpers_reset(void);
void mp6_m608_score_reset(void);
void mp6_m608_sprite_reset(void);
void mp6_m608_ai_reset(void);
void mp6_m608_data_reset(void);
void mp6_m608_reset(void)
{
    mp6_m608_sequence_reset();
    mp6_m608_setup_reset();
    mp6_m608_helpers_reset();
    mp6_m608_score_reset();
    mp6_m608_sprite_reset();
    mp6_m608_ai_reset();
    mp6_m608_data_reset();
}
