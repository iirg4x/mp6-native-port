#include "mp6_rel_state.h"
#define MP6_M670_SEQUENCE_BSS(X) \
    X(lbl_1_bss_8) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_M670_SEQUENCE_DATA(X) \
    X(lbl_1_data_28)
MP6_REL_RESET_DEFINE(mp6_m670_sequence_reset, MP6_M670_SEQUENCE_BSS, MP6_M670_SEQUENCE_DATA)

void mp6_m670_seqparam_reset(void);
void mp6_m670_init_reset(void);
void mp6_m670_pattern_reset(void);
void mp6_m670_pillar_reset(void);
void mp6_m670_reset(void)
{
    mp6_m670_sequence_reset();
    mp6_m670_seqparam_reset();
    mp6_m670_init_reset();
    mp6_m670_pattern_reset();
    mp6_m670_pillar_reset();
}
