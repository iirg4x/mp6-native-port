#include "mp6_rel_state.h"
#define MP6_STAFF_STAFF_BSS(X) \
    X(lbl_1_bss_82C) \
    X(lbl_1_bss_82A) \
    X(lbl_1_bss_828) \
    X(lbl_1_bss_824) \
    X(lbl_1_bss_310) \
    X(lbl_1_bss_304) \
    X(lbl_1_bss_2AC) \
    X(lbl_1_bss_12C) \
    X(lbl_1_bss_12A) \
    X(lbl_1_bss_128) \
    X(lbl_1_bss_124) \
    X(lbl_1_bss_5C) \
    X(lbl_1_bss_50) \
    X(lbl_1_bss_48) \
    X(lbl_1_bss_44) \
    X(lbl_1_bss_40) \
    X(lbl_1_bss_3E) \
    X(lbl_1_bss_3A) \
    X(lbl_1_bss_36) \
    X(lbl_1_bss_34) \
    X(lbl_1_bss_30) \
    X(lbl_1_bss_2C) \
    X(lbl_1_bss_28) \
    X(lbl_1_bss_24) \
    X(lbl_1_bss_20) \
    X(lbl_1_bss_1C) \
    X(lbl_1_bss_18) \
    X(lbl_1_bss_14) \
    X(lbl_1_bss_10) \
    X(lbl_1_bss_C) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_STAFF_STAFF_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_68) \
    X(lbl_1_data_998) \
    X(lbl_1_data_9AA) \
    X(lbl_1_data_9AF) \
    X(lbl_1_data_9C1) \
    X(lbl_1_data_9F8)
MP6_REL_RESET_DEFINE(mp6_staff_staff_reset, MP6_STAFF_STAFF_BSS, MP6_STAFF_STAFF_DATA)

void mp6_staff_staff_reset(void);
void mp6_staff_reset(void)
{
    mp6_staff_staff_reset();
}
