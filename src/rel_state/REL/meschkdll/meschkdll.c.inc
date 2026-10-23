#include "mp6_rel_state.h"
#define MP6_MESCHK_MESCHKDLL_BSS(X) \
    X(objman)
#define MP6_MESCHK_MESCHKDLL_DATA(X) \
    X(lbl_1_data_494) \
    X(lbl_1_data_5C4)
MP6_REL_RESET_DEFINE(mp6_meschk_meschkdll_reset, MP6_MESCHK_MESCHKDLL_BSS, MP6_MESCHK_MESCHKDLL_DATA)

void mp6_meschk_meschkdll_reset(void);
void mp6_meschk_reset(void)
{
    mp6_meschk_meschkdll_reset();
}
