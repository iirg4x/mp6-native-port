#include "mp6_rel_state.h"
#define MP6_M626_TEXT_00000_BSS(X) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_0)
#define MP6_M626_TEXT_00000_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_1C) \
    X(lbl_1_data_28) \
    X(lbl_1_data_34) \
    X(lbl_1_data_38) \
    X(lbl_1_data_80) \
    X(lbl_1_data_C8) \
    X(lbl_1_data_110)
MP6_REL_RESET_DEFINE(mp6_m626_text_00000_reset, MP6_M626_TEXT_00000_BSS, MP6_M626_TEXT_00000_DATA)

void mp6_m626_collision_reset(void);
void mp6_m626_effects_reset(void);
void mp6_m626_player_reset(void);
void mp6_m626_scenery_reset(void);
void mp6_m626_support_reset(void);
void mp6_m626_reset(void)
{
    mp6_m626_text_00000_reset();
    mp6_m626_collision_reset();
    mp6_m626_effects_reset();
    mp6_m626_player_reset();
    mp6_m626_scenery_reset();
    mp6_m626_support_reset();
}
