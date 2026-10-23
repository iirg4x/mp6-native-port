#include "mp6_rel_state.h"
#define MP6_INST_NAME_BSS(X)
#define MP6_INST_NAME_DATA(X) \
    X(lbl_1_data_F8) \
    X(lbl_1_data_410)
MP6_REL_RESET_DEFINE(mp6_inst_name_reset, MP6_INST_NAME_BSS, MP6_INST_NAME_DATA)
