#include "mp6_rel_state.h"
#define MP6_M644_STAGE_BSS(X) \
    X(lbl_1_bss_7C0) \
    X(lbl_1_bss_764) \
    X(lbl_1_bss_760)
#define MP6_M644_STAGE_DATA(X) \
    X(lbl_1_data_3D8) \
    X(lbl_1_data_408) \
    X(lbl_1_data_468) \
    X(lbl_1_data_4B4)
MP6_REL_RESET_DEFINE(mp6_m644_stage_reset, MP6_M644_STAGE_BSS, MP6_M644_STAGE_DATA)
