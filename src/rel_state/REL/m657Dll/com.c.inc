#include "mp6_rel_state.h"
#define MP6_M657_COM_BSS(X) \
    X(lbl_1_bss_40)
#define MP6_M657_COM_DATA(X) \
    X(lbl_1_data_1F8)
MP6_REL_RESET_DEFINE(mp6_m657_com_reset, MP6_M657_COM_BSS, MP6_M657_COM_DATA)
