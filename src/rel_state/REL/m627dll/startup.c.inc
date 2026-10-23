#include "mp6_rel_state.h"
#define MP6_M627_STARTUP_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M627_STARTUP_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28)
MP6_REL_RESET_DEFINE(mp6_m627_startup_reset, MP6_M627_STARTUP_BSS, MP6_M627_STARTUP_DATA)

void mp6_m627_gameplay_reset(void);
void mp6_m627_reset(void) {
    mp6_m627_startup_reset();
    mp6_m627_gameplay_reset();
}
