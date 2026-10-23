#include "mp6_rel_state.h"
#define MP6_OPTION_OPTION_BSS(X)
#define MP6_OPTION_OPTION_DATA(X) \
    X(lbl_1_data_1AB0) \
    X(lbl_1_data_2060)
MP6_REL_RESET_DEFINE(mp6_option_option_reset, MP6_OPTION_OPTION_BSS, MP6_OPTION_OPTION_DATA)
