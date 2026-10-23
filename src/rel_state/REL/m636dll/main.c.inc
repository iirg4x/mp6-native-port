#include "mp6_rel_state.h"
#define MP6_M636_MAIN_BSS(X)
#define MP6_M636_MAIN_DATA(X) X(lbl_1_data_E0)
MP6_REL_RESET_DEFINE(mp6_m636_main_reset, MP6_M636_MAIN_BSS, MP6_M636_MAIN_DATA)

void mp6_m636_pattern_reset(void);
void mp6_m636_score_reset(void);
void mp6_m636_sequence_reset(void);
void mp6_m636_reset(void) {
    mp6_m636_main_reset();
    mp6_m636_pattern_reset();
    mp6_m636_score_reset();
    mp6_m636_sequence_reset();
}
