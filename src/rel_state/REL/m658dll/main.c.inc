#include "mp6_rel_state.h"
#define MP6_M658_MAIN_BSS(X) \
    X(lbl_1_bss_0)
#define MP6_M658_MAIN_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_28)
MP6_REL_RESET_DEFINE(mp6_m658_main_reset, MP6_M658_MAIN_BSS, MP6_M658_MAIN_DATA)

void mp6_m658_gameplay_reset(void);
void mp6_m658_math_camera_reset(void);
void mp6_m658_reset(void) {
    mp6_m658_main_reset();
    mp6_m658_gameplay_reset();
    mp6_m658_math_camera_reset();
}
