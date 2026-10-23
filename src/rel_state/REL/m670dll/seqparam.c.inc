#include "mp6_rel_state.h"
#define MP6_M670_SEQPARAM_BSS(X)
#define MP6_M670_SEQPARAM_DATA(X) \
    X(lbl_1_data_0)
MP6_REL_RESET_DEFINE(mp6_m670_seqparam_reset, MP6_M670_SEQPARAM_BSS, MP6_M670_SEQPARAM_DATA)
