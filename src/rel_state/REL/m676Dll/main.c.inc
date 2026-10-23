#include "mp6_rel_state.h"
#define MP6_M676_MAIN_BSS(X) \
    X(lbl_1_bss_19C) \
    X(lbl_1_bss_198) \
    X(lbl_1_bss_194) \
    X(lbl_1_bss_190) \
    X(lbl_1_bss_18C) \
    X(lbl_1_bss_188) \
    X(lbl_1_bss_184) \
    X(lbl_1_bss_180) \
    X(lbl_1_bss_17C) \
    X(lbl_1_bss_178) \
    X(lbl_1_bss_170) \
    X(lbl_1_bss_168) \
    X(lbl_1_bss_160) \
    X(lbl_1_bss_158) \
    X(lbl_1_bss_150) \
    X(lbl_1_bss_14C) \
    X(lbl_1_bss_148) \
    X(lbl_1_bss_144) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M676_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_38) \
    X(lbl_1_data_44) \
    X(lbl_1_data_50) \
    X(lbl_1_data_84) \
    X(lbl_1_data_CC)
MP6_REL_RESET_DEFINE(mp6_m676_main_reset, MP6_M676_MAIN_BSS, MP6_M676_MAIN_DATA)

void mp6_m676_camera_audio_reset(void);
void mp6_m676_collision_reset(void);
void mp6_m676_effects_reset(void);
void mp6_m676_score_reset(void);
void mp6_m676_text_05A10_reset(void);
void mp6_m676_text_088C0_reset(void);
void mp6_m676_reset(void)
{
    mp6_m676_main_reset();
    mp6_m676_camera_audio_reset();
    mp6_m676_collision_reset();
    mp6_m676_effects_reset();
    mp6_m676_score_reset();
    mp6_m676_text_05A10_reset();
    mp6_m676_text_088C0_reset();
}
