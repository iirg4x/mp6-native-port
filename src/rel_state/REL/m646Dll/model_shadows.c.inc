#include "mp6_rel_state.h"
#define MP6_M646_MODEL_SHADOWS_BSS(X) \
    X(lbl_1_bss_68)
#define MP6_M646_MODEL_SHADOWS_DATA(X) \
    X(lbl_1_data_140) \
    X(lbl_1_data_148)
MP6_REL_RESET_DEFINE(mp6_m646_model_shadows_reset, MP6_M646_MODEL_SHADOWS_BSS, MP6_M646_MODEL_SHADOWS_DATA)
