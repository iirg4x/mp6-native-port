#include "mp6_rel_state.h"
#define MP6_M680_CHARACTER_BSS(X)
#define MP6_M680_CHARACTER_DATA(X) \
    X(lbl_1_data_6C0)
MP6_REL_RESET_DEFINE(mp6_m680_character_reset, MP6_M680_CHARACTER_BSS, MP6_M680_CHARACTER_DATA)
