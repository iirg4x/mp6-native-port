#include "mp6_rel_state.h"
#define MP6_M646_SEQUENCE_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M646_SEQUENCE_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_4) \
    X(lbl_1_data_C)
MP6_REL_RESET_DEFINE(mp6_m646_sequence_reset, MP6_M646_SEQUENCE_BSS, MP6_M646_SEQUENCE_DATA)

void mp6_m646_actors_reset(void);
void mp6_m646_character_motion_reset(void);
void mp6_m646_collision_reset(void);
void mp6_m646_model_attachments_reset(void);
void mp6_m646_model_shadows_reset(void);
void mp6_m646_score_display_reset(void);
void mp6_m646_sequence_reset(void);
void mp6_m646_target_selection_reset(void);
void mp6_m646_reset(void) {
    mp6_m646_actors_reset();
    mp6_m646_character_motion_reset();
    mp6_m646_collision_reset();
    mp6_m646_model_attachments_reset();
    mp6_m646_model_shadows_reset();
    mp6_m646_score_display_reset();
    mp6_m646_sequence_reset();
    mp6_m646_target_selection_reset();
}
