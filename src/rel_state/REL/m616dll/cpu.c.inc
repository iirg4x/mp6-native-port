#include "mp6_rel_state.h"
#define MP6_M616_CPU_BSS(X)
#define MP6_M616_CPU_DATA(X) \
    X(lbl_1_data_1F0) \
    X(lbl_1_data_234) \
    X(lbl_1_data_24C) \
    X(lbl_1_data_280) \
    X(lbl_1_data_2B0) \
    X(lbl_1_data_2D0)
MP6_REL_RESET_DEFINE(mp6_m616_cpu_reset, MP6_M616_CPU_BSS, MP6_M616_CPU_DATA)
