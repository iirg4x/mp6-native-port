#include "mp6_rel_state.h"
#define MP6_MGMRENSHO_COURSE_BSS(X) \
    X(lbl_1_bss_104) \
    X(lbl_1_bss_100) \
    X(lbl_1_bss_FC) \
    X(lbl_1_bss_CE) \
    X(lbl_1_bss_CC) \
    X(lbl_1_bss_CA) \
    X(lbl_1_bss_C8)
#define MP6_MGMRENSHO_COURSE_DATA(X) \
    X(lbl_1_data_1C8) \
    X(lbl_1_data_224) \
    X(lbl_1_data_22D) \
    X(lbl_1_data_237) \
    X(lbl_1_data_241) \
    X(lbl_1_data_24B) \
    X(lbl_1_data_255) \
    X(lbl_1_data_25F) \
    X(lbl_1_data_269) \
    X(lbl_1_data_273) \
    X(lbl_1_data_27D) \
    X(lbl_1_data_287)
MP6_REL_RESET_DEFINE(mp6_mgmrensho_course_reset,
                     MP6_MGMRENSHO_COURSE_BSS,
                     MP6_MGMRENSHO_COURSE_DATA)
