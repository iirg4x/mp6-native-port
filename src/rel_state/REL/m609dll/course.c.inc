#include "mp6_rel_state.h"
#define MP6_M609_COURSE_BSS(X) \
    X(lbl_1_bss_6D84) \
    X(lbl_1_bss_6D80)
#define MP6_M609_COURSE_DATA(X) \
    X(lbl_1_data_58) \
    X(lbl_1_data_C8) \
    X(lbl_1_data_FC) \
    X(lbl_1_data_140) \
    X(lbl_1_data_290) \
    X(lbl_1_data_37E)
MP6_REL_RESET_DEFINE(mp6_m609_course_reset, MP6_M609_COURSE_BSS, MP6_M609_COURSE_DATA)
