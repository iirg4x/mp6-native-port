#include "mp6_rel_state.h"
#define MP6_MGMBATTLE_STAGE_BSS(X) \
    X(lbl_1_bss_3E0) \
    X(lbl_1_bss_3DC) \
    X(lbl_1_bss_3D8) \
    X(lbl_1_bss_3D4) \
    X(lbl_1_bss_3A4) \
    X(lbl_1_bss_374) \
    X(lbl_1_bss_364) \
    X(lbl_1_bss_354) \
    X(lbl_1_bss_34C) \
    X(lbl_1_bss_2AC) \
    X(lbl_1_bss_2A8) \
    X(lbl_1_bss_2A4) \
    X(lbl_1_bss_2A0) \
    X(lbl_1_bss_294) \
    X(lbl_1_bss_290) \
    X(lbl_1_bss_268) \
    X(lbl_1_bss_240) \
    X(lbl_1_bss_1F0) \
    X(lbl_1_bss_100)
#define MP6_MGMBATTLE_STAGE_DATA(X) \
    X(lbl_1_data_208) \
    X(lbl_1_data_224) \
    X(lbl_1_data_278) \
    X(lbl_1_data_284) \
    X(lbl_1_data_2D0) \
    X(lbl_1_data_30C) \
    X(lbl_1_data_354) \
    X(lbl_1_data_3A4) \
    X(lbl_1_data_3B0) \
    X(lbl_1_data_3BC)
MP6_REL_RESET_DEFINE(mp6_mgmbattle_stage_reset,
                     MP6_MGMBATTLE_STAGE_BSS,
                     MP6_MGMBATTLE_STAGE_DATA)
