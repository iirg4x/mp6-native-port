#include "mp6_rel_state.h"
#define MP6_M657_M657_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M657_M657_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_24) \
    X(lbl_1_data_2C)
MP6_REL_RESET_DEFINE(mp6_m657_m657_reset, MP6_M657_M657_BSS, MP6_M657_M657_DATA)

void mp6_m657_arena_reset(void);
void mp6_m657_player_reset(void);
void mp6_m657_score_reset(void);
void mp6_m657_com_reset(void);
void mp6_m657_hud_reset(void);
void mp6_m657_reset(void)
{
    mp6_m657_m657_reset();
    mp6_m657_arena_reset();
    mp6_m657_player_reset();
    mp6_m657_score_reset();
    mp6_m657_com_reset();
    mp6_m657_hud_reset();
}
