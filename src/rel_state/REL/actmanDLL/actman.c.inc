#include "mp6_rel_state.h"
#define MP6_ACTMAN_ACTMAN_BSS(X) \
    X(player) \
    X(objman)
#define MP6_ACTMAN_ACTMAN_DATA(X) \
    X(seqParam)
MP6_REL_RESET_DEFINE(mp6_actman_actman_reset, MP6_ACTMAN_ACTMAN_BSS, MP6_ACTMAN_ACTMAN_DATA)

void mp6_actman_actman_reset(void);
void mp6_actman_reset(void)
{
    mp6_actman_actman_reset();
}
