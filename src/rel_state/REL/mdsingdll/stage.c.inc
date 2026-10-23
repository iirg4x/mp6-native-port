#include "mp6_rel_state.h"
#define MP6_MDSING_STAGE_BSS(X) \
    X(lbl_1_bss_1478) \
    X(lbl_1_bss_1428) \
    X(lbl_1_bss_1426) \
    X(lbl_1_bss_141E) \
    X(lbl_1_bss_141C) \
    X(lbl_1_bss_1414) \
    X(lbl_1_bss_1412) \
    X(lbl_1_bss_140E) \
    X(lbl_1_bss_140A) \
    X(lbl_1_bss_13CE) \
    X(lbl_1_bss_13CC) \
    X(lbl_1_bss_13C8) \
    X(lbl_1_bss_13C4) \
    X(lbl_1_bss_13C0) \
    X(lbl_1_bss_13BC) \
    X(lbl_1_bss_13B8)
#define MP6_MDSING_STAGE_DATA(X) \
    X(lbl_1_data_CD8)
MP6_REL_RESET_DEFINE(mp6_mdsing_stage_reset,
                     MP6_MDSING_STAGE_BSS,
                     MP6_MDSING_STAGE_DATA)
