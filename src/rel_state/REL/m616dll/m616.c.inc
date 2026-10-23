#include "mp6_rel_state.h"
#define MP6_M616_M616_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M616_M616_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m616_m616_reset, MP6_M616_M616_BSS, MP6_M616_M616_DATA)

void mp6_m616_utility_reset(void);
void mp6_m616_cpu_reset(void);
void mp6_m616_reset(void)
{
    mp6_m616_m616_reset();
    mp6_m616_utility_reset();
    mp6_m616_cpu_reset();
}
