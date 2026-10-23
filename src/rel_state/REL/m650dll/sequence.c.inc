#include "mp6_rel_state.h"
#define MP6_M650_SEQUENCE_BSS(X) \
    X(mp6WsBoneExtraX) \
    X(mp6WsOffstagePending) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_0)
#define MP6_M650_SEQUENCE_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m650_sequence_reset, MP6_M650_SEQUENCE_BSS, MP6_M650_SEQUENCE_DATA)

void mp6_m650_game_reset(void);
void mp6_m650_obstacle_reset(void);
void mp6_m650_reset(void)
{
    mp6_m650_sequence_reset();
    mp6_m650_game_reset();
    mp6_m650_obstacle_reset();
}
