#include "mp6_rel_state.h"
#define MP6_M609_TEXTURE_MATRIX_BSS(X) \
    X(lbl_1_bss_6E68)
#define MP6_M609_TEXTURE_MATRIX_DATA(X) \
    X(lbl_1_data_858)
MP6_REL_RESET_DEFINE(mp6_m609_texture_matrix_reset, MP6_M609_TEXTURE_MATRIX_BSS, MP6_M609_TEXTURE_MATRIX_DATA)
