#include "mp6_rel_state.h"
#define MP6_M626_PLAYER_BSS(X) \
    X(lbl_1_bss_304) \
    X(lbl_1_bss_2F4) \
    X(lbl_1_bss_2D4) \
    X(lbl_1_bss_2D0)
#define MP6_M626_PLAYER_DATA(X) \
    X(lbl_1_data_148) \
    X(lbl_1_data_170) \
    X(lbl_1_data_200) \
    X(lbl_1_data_290) \
    X(lbl_1_data_2CC) \
    X(lbl_1_data_2E4) \
    X(lbl_1_data_364) \
    X(lbl_1_data_3C0) \
    X(lbl_1_data_3F4) \
    X(lbl_1_data_400) \
    X(lbl_1_data_460)
MP6_REL_RESET_DEFINE(mp6_m626_player_reset, MP6_M626_PLAYER_BSS, MP6_M626_PLAYER_DATA)
