#include "mp6_rel_state.h"
#define MP6_M608_AI_BSS(X)
#define MP6_M608_AI_DATA(X) \
    X(lbl_1_data_4C8)
MP6_REL_RESET_DEFINE(mp6_m608_ai_reset, MP6_M608_AI_BSS, MP6_M608_AI_DATA)
