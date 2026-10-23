#include "mp6_rel_state.h"
#define MP6_M609_STARTUP_BSS(X) \
    X(lbl_1_bss_6D7C) \
    X(lbl_1_bss_6C34) \
    X(lbl_1_bss_6BF8) \
    X(lbl_1_bss_6BBC) \
    X(lbl_1_bss_6B6C) \
    X(lbl_1_bss_6B64) \
    X(lbl_1_bss_8BC) \
    X(lbl_1_bss_70C) \
    X(lbl_1_bss_24C) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M609_STARTUP_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_42)
MP6_REL_RESET_DEFINE(mp6_m609_startup_reset, MP6_M609_STARTUP_BSS, MP6_M609_STARTUP_DATA)

void mp6_m609_course_reset(void);
void mp6_m609_model_setup_reset(void);
void mp6_m609_player_reset(void);
void mp6_m609_texture_matrix_reset(void);
void mp6_m609_reset(void)
{
    mp6_m609_startup_reset();
    mp6_m609_course_reset();
    mp6_m609_model_setup_reset();
    mp6_m609_player_reset();
    mp6_m609_texture_matrix_reset();
}
