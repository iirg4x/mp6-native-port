#include "mp6_rel_state.h"
#define MP6_M626_SUPPORT_BSS(X) \
    X(lbl_1_bss_2C8) \
    X(lbl_1_bss_2C4) \
    X(lbl_1_bss_2C0) \
    X(lbl_1_bss_2BC) \
    X(lbl_1_bss_2B8) \
    X(lbl_1_bss_178) \
    X(lbl_1_bss_38)
#define MP6_M626_SUPPORT_DATA(X) \
    X(lbl_1_data_138) \
    X(lbl_1_data_144)
MP6_REL_RESET_DEFINE(mp6_m626_support_reset, MP6_M626_SUPPORT_BSS, MP6_M626_SUPPORT_DATA)
