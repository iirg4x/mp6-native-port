#include "mp6_rel_state.h"
#define MP6_M635_M635_BSS(X)
#define MP6_M635_M635_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_C) \
    X(lbl_1_data_12) \
    X(lbl_1_data_24) \
    X(lbl_1_data_78)
MP6_REL_RESET_DEFINE(mp6_m635_m635_reset, MP6_M635_M635_BSS, MP6_M635_M635_DATA)

void mp6_m635_logic_reset(void);
void mp6_m635_graphics_reset(void);
void mp6_m635_reset(void)
{
    mp6_m635_m635_reset();
    mp6_m635_logic_reset();
    mp6_m635_graphics_reset();
}
