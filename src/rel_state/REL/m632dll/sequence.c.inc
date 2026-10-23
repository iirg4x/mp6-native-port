#include "mp6_rel_state.h"
#define MP6_M632_SEQUENCE_BSS(X)
#define MP6_M632_SEQUENCE_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_35) \
    X(lbl_1_data_42) \
    X(lbl_1_data_4F) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_69)
MP6_REL_RESET_DEFINE(mp6_m632_sequence_reset, MP6_M632_SEQUENCE_BSS, MP6_M632_SEQUENCE_DATA)

void mp6_m632_game_reset(void);
void mp6_m632_reset(void)
{
    mp6_m632_sequence_reset();
    mp6_m632_game_reset();
}
