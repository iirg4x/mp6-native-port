/* Game-thread AO admission. Missing camera boundaries are conservative. */
#ifndef MP6_AO_DEPTH_EPOCH_H
#define MP6_AO_DEPTH_EPOCH_H
#include <stdint.h>

typedef struct Mp6AoDepthEpoch {
    uint64_t serial;
    int valid;    /* camera/depth scope was explicitly started */
    int consumed; /* AO was emitted (or deliberately admitted as empty) */
    int excluded; /* semantic 2D/copy segment: never shade this epoch */
} Mp6AoDepthEpoch;

static inline void mp6_ao_depth_epoch_reset(Mp6AoDepthEpoch *epoch, uint64_t serial)
{
    epoch->serial = serial;
    epoch->valid = 1;
    epoch->consumed = 0;
    epoch->excluded = 0;
}

static inline int mp6_ao_depth_epoch_has_writes(const Mp6AoDepthEpoch *epoch, uint64_t serial)
{
    return epoch && epoch->valid && !epoch->consumed && !epoch->excluded &&
           epoch->serial != serial;
}

static inline void mp6_ao_depth_epoch_consume(Mp6AoDepthEpoch *epoch)
{
    if (epoch) epoch->consumed = 1;
}

static inline void mp6_ao_depth_epoch_exclude(Mp6AoDepthEpoch *epoch)
{
    if (epoch) {
        epoch->excluded = 1;
        epoch->consumed = 1;
    }
}
#endif
