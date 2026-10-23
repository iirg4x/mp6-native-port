#include "mp6_rel_state.h"
#define MP6_RESULT_PARTICLE_RENDERER_BSS(X) \
    X(lbl_1_bss_1530) \
    X(lbl_1_bss_14B0) \
    X(lbl_1_bss_14AC) \
    X(lbl_1_bss_14A8)
#define MP6_RESULT_PARTICLE_RENDERER_DATA(X) \
    X(lbl_1_data_8E0) \
    X(lbl_1_data_8E8) \
    X(lbl_1_data_918) \
    X(lbl_1_data_948) \
    X(lbl_1_data_968)
MP6_REL_RESET_DEFINE(mp6_result_particle_renderer_reset, MP6_RESULT_PARTICLE_RENDERER_BSS, MP6_RESULT_PARTICLE_RENDERER_DATA)
