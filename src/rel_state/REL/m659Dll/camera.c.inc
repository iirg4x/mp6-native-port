#include "mp6_rel_state.h"
#define MP6_M659_CAMERA_BSS(X)
#define MP6_M659_CAMERA_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_C) \
    X(lbl_1_data_2C)
MP6_REL_RESET_DEFINE(mp6_m659_camera_reset, MP6_M659_CAMERA_BSS, MP6_M659_CAMERA_DATA)

void mp6_m659_collision_reset(void);
void mp6_m659_com_reset(void);
void mp6_m659_globals_reset(void);
void mp6_m659_player_reset(void);
void mp6_m659_scene_reset(void);
void mp6_m659_reset(void)
{
    mp6_m659_camera_reset();
    mp6_m659_collision_reset();
    mp6_m659_com_reset();
    mp6_m659_globals_reset();
    mp6_m659_player_reset();
    mp6_m659_scene_reset();
}
