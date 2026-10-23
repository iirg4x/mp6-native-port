#include "mp6_rel_state.h"
#define MP6_M609_MODEL_SETUP_BSS(X) \
    X(lbl_1_bss_6D88)
#define MP6_M609_MODEL_SETUP_DATA(X)
MP6_REL_RESET_DEFINE(mp6_m609_model_setup_reset, MP6_M609_MODEL_SETUP_BSS, MP6_M609_MODEL_SETUP_DATA)
