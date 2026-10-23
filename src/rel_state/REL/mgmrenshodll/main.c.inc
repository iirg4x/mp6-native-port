#include "mp6_rel_state.h"
#define MP6_MGMRENSHO_MAIN_BSS(X) \
    X(lbl_1_bss_158) \
    X(lbl_1_bss_154) \
    X(lbl_1_bss_152) \
    X(lbl_1_bss_150)
#define MP6_MGMRENSHO_MAIN_DATA(X) \
    X(lbl_1_data_500) \
    X(lbl_1_data_504) \
    X(lbl_1_data_508) \
    X(lbl_1_data_50C) \
    X(lbl_1_data_510) \
    X(lbl_1_data_514) \
    X(lbl_1_data_518) \
    X(lbl_1_data_51C)
MP6_REL_RESET_DEFINE(mp6_mgmrensho_main_reset,
                     MP6_MGMRENSHO_MAIN_BSS,
                     MP6_MGMRENSHO_MAIN_DATA)

void mp6_mgmrensho_preview_reset(void);
void mp6_mgmrensho_camera_reset(void);
void mp6_mgmrensho_messages_reset(void);
void mp6_mgmrensho_course_reset(void);
void mp6_mgmrensho_scenery_reset(void);
void mp6_mgmrensho_player_reset(void);

void mp6_mgmrensho_reset(void)
{
    mp6_mgmrensho_main_reset();
    mp6_mgmrensho_preview_reset();
    mp6_mgmrensho_camera_reset();
    mp6_mgmrensho_messages_reset();
    mp6_mgmrensho_course_reset();
    mp6_mgmrensho_scenery_reset();
    mp6_mgmrensho_player_reset();
}
