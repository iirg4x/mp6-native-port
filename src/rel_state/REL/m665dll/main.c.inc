#include "mp6_rel_state.h"
#define MP6_M665_MAIN_BSS(X) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_2) \
    X(lbl_1_bss_0)
#define MP6_M665_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28)
MP6_REL_RESET_DEFINE(mp6_m665_main_reset, MP6_M665_MAIN_BSS, MP6_M665_MAIN_DATA)

void mp6_m665_scene_reset(void);
void mp6_m665_player_reset(void);
void mp6_m665_opponent_reset(void);
void mp6_m665_reset(void)
{
    mp6_m665_main_reset();
    mp6_m665_scene_reset();
    mp6_m665_player_reset();
    mp6_m665_opponent_reset();
}
