#include "mp6_rel_state.h"
#define MP6_M638_GAME_BSS(X) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M638_GAME_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_31) \
    X(lbl_1_data_3A) \
    X(lbl_1_data_43) \
    X(lbl_1_data_4C) \
    X(lbl_1_data_55) \
    X(lbl_1_data_5E) \
    X(lbl_1_data_67) \
    X(lbl_1_data_70) \
    X(lbl_1_data_79) \
    X(lbl_1_data_82) \
    X(lbl_1_data_8B) \
    X(lbl_1_data_94) \
    X(lbl_1_data_9D) \
    X(lbl_1_data_A6) \
    X(lbl_1_data_AF) \
    X(lbl_1_data_B8) \
    X(lbl_1_data_C1) \
    X(lbl_1_data_CA) \
    X(lbl_1_data_D3) \
    X(lbl_1_data_DC) \
    X(lbl_1_data_E5) \
    X(lbl_1_data_EE) \
    X(lbl_1_data_F7) \
    X(lbl_1_data_100) \
    X(lbl_1_data_109) \
    X(lbl_1_data_112) \
    X(lbl_1_data_11B) \
    X(lbl_1_data_124) \
    X(lbl_1_data_12D) \
    X(lbl_1_data_136) \
    X(lbl_1_data_13F) \
    X(lbl_1_data_148) \
    X(lbl_1_data_151) \
    X(lbl_1_data_15A) \
    X(lbl_1_data_163) \
    X(lbl_1_data_16C) \
    X(lbl_1_data_175) \
    X(lbl_1_data_17E) \
    X(lbl_1_data_187) \
    X(lbl_1_data_190) \
    X(lbl_1_data_230) \
    X(lbl_1_data_235) \
    X(lbl_1_data_23E) \
    X(lbl_1_data_247) \
    X(lbl_1_data_250) \
    X(lbl_1_data_259) \
    X(lbl_1_data_262) \
    X(lbl_1_data_26B) \
    X(lbl_1_data_274) \
    X(lbl_1_data_27D) \
    X(lbl_1_data_286) \
    X(lbl_1_data_290) \
    X(lbl_1_data_330) \
    X(lbl_1_data_380) \
    X(lbl_1_data_3A8) \
    X(lbl_1_data_4CC) \
    X(lbl_1_data_4CE) \
    X(lbl_1_data_4D0) \
    X(lbl_1_data_4D7) \
    X(lbl_1_data_4DE) \
    X(lbl_1_data_4E4) \
    X(lbl_1_data_4EB) \
    X(lbl_1_data_4EF) \
    X(lbl_1_data_4F3) \
    X(lbl_1_data_4F7) \
    X(lbl_1_data_4FB) \
    X(lbl_1_data_502) \
    X(lbl_1_data_509) \
    X(lbl_1_data_510) \
    X(lbl_1_data_517)
MP6_REL_RESET_DEFINE(mp6_m638_game_reset, MP6_M638_GAME_BSS, MP6_M638_GAME_DATA)

void mp6_m638_player_reset(void);
void mp6_m638_scheduler_reset(void);
void mp6_m638_reset(void)
{
    mp6_m638_game_reset();
    mp6_m638_player_reset();
    mp6_m638_scheduler_reset();
}
