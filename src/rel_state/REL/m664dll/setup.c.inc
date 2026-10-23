#include "mp6_rel_state.h"
#define MP6_M664_SETUP_BSS(X) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M664_SETUP_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m664_setup_reset, MP6_M664_SETUP_BSS, MP6_M664_SETUP_DATA)

void mp6_m664_game_reset(void);
void mp6_m664_reset(void) {
    mp6_m664_setup_reset();
    mp6_m664_game_reset();
}
