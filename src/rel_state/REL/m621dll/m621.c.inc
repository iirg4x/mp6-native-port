#include "mp6_rel_state.h"
#define MP6_M621_M621_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M621_M621_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28)
MP6_REL_RESET_DEFINE(mp6_m621_m621_reset, MP6_M621_M621_BSS, MP6_M621_M621_DATA)

void mp6_m621_gameplay_reset(void);
void mp6_m621_reset(void)
{
    mp6_m621_m621_reset();
    mp6_m621_gameplay_reset();
}
