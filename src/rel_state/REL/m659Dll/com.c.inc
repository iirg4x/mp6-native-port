#include "mp6_rel_state.h"
#define MP6_M659_COM_BSS(X) \
    X(lbl_1_bss_60)
#define MP6_M659_COM_DATA(X) \
    X(lbl_1_data_1D8)
MP6_REL_RESET_DEFINE(mp6_m659_com_reset, MP6_M659_COM_BSS, MP6_M659_COM_DATA)
