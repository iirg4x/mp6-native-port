#include "mp6_rel_state.h"
#define MP6_M662_PUZZLE_BSS(X)
#define MP6_M662_PUZZLE_DATA(X) \
    X(lbl_1_data_448) \
    X(lbl_1_data_4E8)
MP6_REL_RESET_DEFINE(mp6_m662_puzzle_reset, MP6_M662_PUZZLE_BSS, MP6_M662_PUZZLE_DATA)
