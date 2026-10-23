#include "mp6_rel_state.h"
#define MP6_M613_GEOMETRY_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M613_GEOMETRY_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m613_geometry_reset, MP6_M613_GEOMETRY_BSS, MP6_M613_GEOMETRY_DATA)
