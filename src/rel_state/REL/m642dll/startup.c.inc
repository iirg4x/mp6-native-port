#include "mp6_rel_state.h"
#define MP6_M642_STARTUP_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M642_STARTUP_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28)
MP6_REL_RESET_DEFINE(mp6_m642_startup_reset, MP6_M642_STARTUP_BSS, MP6_M642_STARTUP_DATA)

void mp6_m642_main_reset(void);
void mp6_m642_text_07878_reset(void);
void mp6_m642_reset(void)
{
    mp6_m642_startup_reset();
    mp6_m642_main_reset();
    mp6_m642_text_07878_reset();
}
