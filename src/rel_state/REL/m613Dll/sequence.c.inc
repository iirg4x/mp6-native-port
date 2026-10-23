#include "mp6_rel_state.h"
#define MP6_M613_SEQUENCE_BSS(X) \
    X(lbl_1_bss_1260) \
    X(lbl_1_bss_125E) \
    X(lbl_1_bss_125C) \
    X(lbl_1_bss_1258)
#define MP6_M613_SEQUENCE_DATA(X) \
    X(lbl_1_data_270)
MP6_REL_RESET_DEFINE(mp6_m613_sequence_reset, MP6_M613_SEQUENCE_BSS, MP6_M613_SEQUENCE_DATA)

void mp6_m613_camera_reset(void);
void mp6_m613_player_reset(void);
void mp6_m613_geometry_reset(void);
void mp6_m613_bitmap_reset(void);
void mp6_m613_progress_reset(void);
void mp6_m613_reset(void)
{
    mp6_m613_sequence_reset();
    mp6_m613_camera_reset();
    mp6_m613_player_reset();
    mp6_m613_geometry_reset();
    mp6_m613_bitmap_reset();
    mp6_m613_progress_reset();
}
