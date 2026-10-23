#include "mp6_rel_state.h"
#define MP6_M675_MAIN_BSS(X) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3C) \
    X(lbl_1_bss_38) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M675_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_1C) \
    X(lbl_1_data_28) \
    X(lbl_1_data_34) \
    X(lbl_1_data_38) \
    X(lbl_1_data_68) \
    X(lbl_1_data_B0) \
    X(lbl_1_data_D8) \
    X(lbl_1_data_E4)
MP6_REL_RESET_DEFINE(mp6_m675_main_reset, MP6_M675_MAIN_BSS, MP6_M675_MAIN_DATA)

void mp6_m675_camera_reset(void);
void mp6_m675_collision_reset(void);
void mp6_m675_effects_reset(void);
void mp6_m675_gameplay_reset(void);
void mp6_m675_main_reset(void);
void mp6_m675_player_reset(void);
void mp6_m675_score_reset(void);
void mp6_m675_reset(void) {
    mp6_m675_camera_reset();
    mp6_m675_collision_reset();
    mp6_m675_effects_reset();
    mp6_m675_gameplay_reset();
    mp6_m675_main_reset();
    mp6_m675_player_reset();
    mp6_m675_score_reset();
}
