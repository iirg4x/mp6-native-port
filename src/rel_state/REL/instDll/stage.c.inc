#include "mp6_rel_state.h"
#define MP6_INST_STAGE_BSS(X)
#define MP6_INST_STAGE_DATA(X) \
    X(lbl_1_data_520)
MP6_REL_RESET_DEFINE(mp6_inst_stage_reset, MP6_INST_STAGE_BSS, MP6_INST_STAGE_DATA)
