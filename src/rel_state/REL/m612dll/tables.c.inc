#include "mp6_rel_state.h"
#define MP6_M612_TABLES_BSS(X)
#define MP6_M612_TABLES_DATA(X) \
    X(lbl_1_data_198) \
    X(lbl_1_data_16B0)
MP6_REL_RESET_DEFINE(mp6_m612_tables_reset, MP6_M612_TABLES_BSS, MP6_M612_TABLES_DATA)
