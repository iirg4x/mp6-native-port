#include "mp6_rel_state.h"
#define MP6_RESULT_BATTLE_RESULTS_BSS(X) \
    X(lbl_1_bss_10A4) \
    X(lbl_1_bss_1094) \
    X(lbl_1_bss_1084) \
    X(lbl_1_bss_1074) \
    X(lbl_1_bss_1070) \
    X(lbl_1_bss_106C) \
    X(lbl_1_bss_FAC) \
    X(lbl_1_bss_DAC) \
    X(lbl_1_bss_BAC) \
    X(lbl_1_bss_BA8) \
    X(lbl_1_bss_BA4) \
    X(lbl_1_bss_BA0)
#define MP6_RESULT_BATTLE_RESULTS_DATA(X) \
    X(lbl_1_data_590) \
    X(lbl_1_data_5B0) \
    X(lbl_1_data_5D0) \
    X(lbl_1_data_5F0) \
    X(lbl_1_data_620) \
    X(lbl_1_data_650) \
    X(lbl_1_data_680) \
    X(lbl_1_data_6B0) \
    X(lbl_1_data_6E0) \
    X(lbl_1_data_6F0) \
    X(lbl_1_data_700) \
    X(lbl_1_data_780) \
    X(lbl_1_data_78C) \
    X(lbl_1_data_79C) \
    X(lbl_1_data_7AC) \
    X(lbl_1_data_82C) \
    X(lbl_1_data_83C) \
    X(lbl_1_data_84C)
MP6_REL_RESET_DEFINE(mp6_result_battle_results_reset, MP6_RESULT_BATTLE_RESULTS_BSS, MP6_RESULT_BATTLE_RESULTS_DATA)

void mp6_result_battle_results_reset(void);
void mp6_result_particle_renderer_reset(void);
void mp6_result_player_results_reset(void);
void mp6_result_team_results_reset(void);
void mp6_result_reset(void)
{
    mp6_result_battle_results_reset();
    mp6_result_particle_renderer_reset();
    mp6_result_player_results_reset();
    mp6_result_team_results_reset();
}
