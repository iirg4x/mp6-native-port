#include "mp6_rel_state.h"
#define MP6_S03_S03_BSS(X) \
    X(lbl_1_bss_78) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_S03_S03_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_C) \
    X(lbl_1_data_18) \
    X(lbl_1_data_30) \
    X(lbl_1_data_3C) \
    X(lbl_1_data_48) \
    X(lbl_1_data_4C) \
    X(lbl_1_data_52) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_62) \
    X(lbl_1_data_6C) \
    X(lbl_1_data_84) \
    X(lbl_1_data_8C)
MP6_REL_RESET_DEFINE(mp6_s03_s03_reset, MP6_S03_S03_BSS, MP6_S03_S03_DATA)

void mp6_s03_s03_reset(void);
void mp6_s03_reset(void)
{
    mp6_s03_s03_reset();
}
