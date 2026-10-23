#include "mp6_rel_state.h"
#define MP6_M676_CAMERA_AUDIO_BSS(X) \
    X(lbl_1_bss_430) \
    X(lbl_1_bss_42C) \
    X(lbl_1_bss_428) \
    X(lbl_1_bss_424) \
    X(lbl_1_bss_420) \
    X(lbl_1_bss_2E0) \
    X(lbl_1_bss_1A0)
#define MP6_M676_CAMERA_AUDIO_DATA(X) \
    X(lbl_1_data_120) \
    X(lbl_1_data_12C)
MP6_REL_RESET_DEFINE(mp6_m676_camera_audio_reset, MP6_M676_CAMERA_AUDIO_BSS, MP6_M676_CAMERA_AUDIO_DATA)
