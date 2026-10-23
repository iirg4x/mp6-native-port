#include "mp6_rel_state.h"
#define MP6_M626_EFFECTS_BSS(X) \
    X(lbl_1_bss_6CC) \
    X(lbl_1_bss_6C8)
#define MP6_M626_EFFECTS_DATA(X) \
    X(lbl_1_data_8C0) \
    X(lbl_1_data_8C8)
MP6_REL_RESET_DEFINE(mp6_m626_effects_reset, MP6_M626_EFFECTS_BSS, MP6_M626_EFFECTS_DATA)
