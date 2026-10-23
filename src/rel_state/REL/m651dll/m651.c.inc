#include "mp6_rel_state.h"
#define MP6_M651_M651_BSS(X) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M651_M651_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_C) \
    X(lbl_1_data_10) \
    X(lbl_1_data_14)
MP6_REL_RESET_DEFINE(mp6_m651_m651_reset, MP6_M651_M651_BSS, MP6_M651_M651_DATA)

void mp6_m651_players_reset(void);
void mp6_m651_reset(void)
{
    mp6_m651_m651_reset();
    mp6_m651_players_reset();
}
