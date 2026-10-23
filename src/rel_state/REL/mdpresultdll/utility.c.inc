#include "mp6_rel_state.h"
#define MP6_MDPRESULT_UTILITY_BSS(X) \
    X(lbl_1_bss_14C8) \
    X(lbl_1_bss_14C6) \
    X(lbl_1_bss_14C4) \
    X(lbl_1_bss_14C2) \
    X(lbl_1_bss_14B0) \
    X(lbl_1_bss_1490) \
    X(lbl_1_bss_1480) \
    X(lbl_1_bss_1320) \
    X(lbl_1_bss_131C) \
    X(lbl_1_bss_131A) \
    X(lbl_1_bss_1318)
#define MP6_MDPRESULT_UTILITY_DATA(X) \
    X(lbl_1_data_788)
MP6_REL_RESET_DEFINE(mp6_mdpresult_utility_reset, MP6_MDPRESULT_UTILITY_BSS, MP6_MDPRESULT_UTILITY_DATA)
