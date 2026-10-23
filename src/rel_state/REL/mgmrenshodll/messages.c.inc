#include "mp6_rel_state.h"
#define MP6_MGMRENSHO_MESSAGES_BSS(X) \
    X(lbl_1_bss_B8)
#define MP6_MGMRENSHO_MESSAGES_DATA(X) \
    X(lbl_1_data_10) \
    X(lbl_1_data_30) \
    X(lbl_1_data_58) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_94) \
    X(lbl_1_data_1C0)
MP6_REL_RESET_DEFINE(mp6_mgmrensho_messages_reset,
                     MP6_MGMRENSHO_MESSAGES_BSS,
                     MP6_MGMRENSHO_MESSAGES_DATA)
