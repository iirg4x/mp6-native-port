#include "mp6_rel_state.h"
#define MP6_M643_STARTUP_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M643_STARTUP_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m643_startup_reset, MP6_M643_STARTUP_BSS, MP6_M643_STARTUP_DATA)

void mp6_m643_game_reset(void);
void mp6_m643_reset(void) {
    mp6_m643_startup_reset();
    mp6_m643_game_reset();
}
