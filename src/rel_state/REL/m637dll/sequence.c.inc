#include "mp6_rel_state.h"
#define MP6_M637_SEQUENCE_BSS(X) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M637_SEQUENCE_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m637_sequence_reset, MP6_M637_SEQUENCE_BSS, MP6_M637_SEQUENCE_DATA)

void mp6_m637_game_reset(void);
void mp6_m637_reset(void)
{
    mp6_m637_sequence_reset();
    mp6_m637_game_reset();
}
