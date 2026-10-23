#include "mp6_rel_state.h"
#define MP6_MGMDECATHLON_MAIN_BSS(X) \
    X(lbl_1_bss_C0)
#define MP6_MGMDECATHLON_MAIN_DATA(X) \
    X(lbl_1_data_C70) \
    X(lbl_1_data_C74) \
    X(lbl_1_data_C78) \
    X(lbl_1_data_C8C) \
    X(lbl_1_data_C90) \
    X(lbl_1_data_C94) \
    X(lbl_1_data_C98) \
    X(lbl_1_data_CD0)
MP6_REL_RESET_DEFINE(mp6_mgmdecathlon_main_reset,
                     MP6_MGMDECATHLON_MAIN_BSS,
                     MP6_MGMDECATHLON_MAIN_DATA)
void mp6_mgmdecathlon_camera_reset(void);
void mp6_mgmdecathlon_players_reset(void);
void mp6_mgmdecathlon_player_intro_reset(void);
void mp6_mgmdecathlon_results_reset(void);
void mp6_mgmdecathlon_scenery_reset(void);
void mp6_mgmdecathlon_windows_reset(void);

void mp6_mgmdecathlon_reset(void)
{
    mp6_mgmdecathlon_camera_reset();
    mp6_mgmdecathlon_main_reset();
    mp6_mgmdecathlon_players_reset();
    mp6_mgmdecathlon_player_intro_reset();
    mp6_mgmdecathlon_results_reset();
    mp6_mgmdecathlon_scenery_reset();
    mp6_mgmdecathlon_windows_reset();
}
