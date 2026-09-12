/* Game-thread AO admission. Missing camera boundaries are conservative. */
#ifndef MP6_AO_DEPTH_EPOCH_H
#define MP6_AO_DEPTH_EPOCH_H
#include <stdint.h>

typedef struct Mp6AoDepthEpoch {
    uint64_t serial;
    int valid;
} Mp6AoDepthEpoch;

static inline void mp6_ao_depth_epoch_reset(Mp6AoDepthEpoch *epoch, uint64_t serial)
{
    epoch->serial = serial;
    epoch->valid = 1;
}

static inline int mp6_ao_depth_epoch_has_writes(const Mp6AoDepthEpoch *epoch, uint64_t serial)
{
    return !epoch->valid || epoch->serial != serial;
}
#endif
