#include "mp6_rel_state.h"
#define MP6_M644_MENU_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M644_MENU_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_2) \
    X(lbl_1_data_A) \
    X(lbl_1_data_C) \
    X(lbl_1_data_10) \
    X(lbl_1_data_14)
MP6_REL_RESET_DEFINE(mp6_m644_menu_reset, MP6_M644_MENU_BSS, MP6_M644_MENU_DATA)

void mp6_m644_scene_reset(void);
void mp6_m644_camera_reset(void);
void mp6_m644_stage_reset(void);
void mp6_m644_routes_reset(void);
void mp6_m644_player_reset(void);
void mp6_m644_reset(void)
{
    mp6_m644_menu_reset();
    mp6_m644_scene_reset();
    mp6_m644_camera_reset();
    mp6_m644_stage_reset();
    mp6_m644_routes_reset();
    mp6_m644_player_reset();
}
