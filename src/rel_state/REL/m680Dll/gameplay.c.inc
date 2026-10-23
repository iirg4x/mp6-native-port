#include "mp6_rel_state.h"
#define MP6_M680_GAMEPLAY_BSS(X) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M680_GAMEPLAY_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28) \
    X(lbl_1_data_370) \
    X(lbl_1_data_46C)
MP6_REL_RESET_DEFINE(mp6_m680_gameplay_reset, MP6_M680_GAMEPLAY_BSS, MP6_M680_GAMEPLAY_DATA)

void mp6_m680_character_reset(void);
void mp6_m680_scheduler_reset(void);
void mp6_m680_reset(void)
{
    mp6_m680_gameplay_reset();
    mp6_m680_character_reset();
    mp6_m680_scheduler_reset();
}
