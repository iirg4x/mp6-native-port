#include "mp6_rel_state.h"
#define MP6_MGMTOURNAMENT_SCENE_BSS(X)
#define MP6_MGMTOURNAMENT_SCENE_DATA(X) \
    X(lbl_1_data_848) \
    X(lbl_1_data_84C) \
    X(lbl_1_data_850) \
    X(lbl_1_data_854) \
    X(lbl_1_data_858)
MP6_REL_RESET_DEFINE(mp6_mgmtournament_scene_reset,
    MP6_MGMTOURNAMENT_SCENE_BSS,
    MP6_MGMTOURNAMENT_SCENE_DATA)

void mp6_mgmtournament_reset(void)
{
    mp6_mgmtournament_bracket_reset();
    mp6_mgmtournament_camera_reset();
    mp6_mgmtournament_characters_reset();
    mp6_mgmtournament_host_reset();
    mp6_mgmtournament_preview_reset();
    mp6_mgmtournament_scene_reset();
    mp6_mgmtournament_window_reset();
}
