#include "mp6_rel_state.h"
#define MP6_M646_ACTORS_BSS(X) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_28)
#define MP6_M646_ACTORS_DATA(X) \
    X(lbl_1_data_38) \
    X(lbl_1_data_44) \
    X(lbl_1_data_50) \
    X(lbl_1_data_60) \
    X(lbl_1_data_70) \
    X(lbl_1_data_80) \
    X(lbl_1_data_90) \
    X(lbl_1_data_A0) \
    X(lbl_1_data_B0) \
    X(lbl_1_data_C0)
MP6_REL_RESET_DEFINE(mp6_m646_actors_reset, MP6_M646_ACTORS_BSS, MP6_M646_ACTORS_DATA)
