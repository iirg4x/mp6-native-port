#include "mp6_rel_state.h"
#define MP6_M657_HUD_BSS(X) \
    X(lbl_1_bss_48)
#define MP6_M657_HUD_DATA(X) \
    X(lbl_1_data_280) \
    X(lbl_1_data_2A4) \
    X(lbl_1_data_2B8) \
    X(lbl_1_data_2F0) \
    X(lbl_1_data_338)
MP6_REL_RESET_DEFINE(mp6_m657_hud_reset, MP6_M657_HUD_BSS, MP6_M657_HUD_DATA)
