#include "mp6_rel_state.h"
#define MP6_M619_SCENE_BSS(X) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M619_SCENE_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_AC)
MP6_REL_RESET_DEFINE(mp6_m619_scene_reset, MP6_M619_SCENE_BSS, MP6_M619_SCENE_DATA)

void mp6_m619_motion_callbacks_reset(void);
void mp6_m619_player_reset(void);
void mp6_m619_reset(void)
{
    mp6_m619_scene_reset();
    mp6_m619_motion_callbacks_reset();
    mp6_m619_player_reset();
}
