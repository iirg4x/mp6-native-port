#include "mp6_rel_state.h"
#define MP6_SEQUENCE_SEQUENCE_BSS(X) \
    X(objman) \
    X(charMdlId) \
    X(charMotId) \
    X(charNo) \
    X(padNo) \
    X(jumpNum)
#define MP6_SEQUENCE_SEQUENCE_DATA(X) \
    X(jumpTimer) \
    X(seqParam)
MP6_REL_RESET_DEFINE(mp6_sequence_sequence_reset, MP6_SEQUENCE_SEQUENCE_BSS, MP6_SEQUENCE_SEQUENCE_DATA)

void mp6_sequence_sequence_reset(void);
void mp6_sequence_reset(void)
{
    mp6_sequence_sequence_reset();
}
