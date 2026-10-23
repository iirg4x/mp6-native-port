/* MP6 native port -- see include/mp6_name_index.h. The table is host
 * memory freed before the caller returns, so nothing here outlives a load. */
#include "mp6_name_index.h"

#include <stdlib.h>
#include <string.h>

static uint32_t name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    while (*name) h = (h ^ (unsigned char)*name++) * 16777619u;
    return h;
}

static const char *name_at(const Mp6NameIndex *ix, int32_t i)
{
    return *(const char *const *)(ix->items + (size_t)i * ix->stride + ix->nameOffset);
}

void mp6_name_index_build(Mp6NameIndex *ix, const void *items, size_t stride, size_t nameOffset,
                          int32_t count)
{
    uint32_t size = 16;
    int32_t i;
    memset(ix, 0, sizeof(*ix));
    if (count < 8) return;
    while (size < (uint32_t)count * 2u) size <<= 1;
    ix->slot = (int32_t *)calloc(size, sizeof(int32_t));
    if (!ix->slot) return;
    ix->mask = size - 1;
    ix->items = (const unsigned char *)items;
    ix->stride = stride;
    ix->nameOffset = nameOffset;
    for (i = 0; i < count; i++) {
        const char *name = name_at(ix, i);
        uint32_t h;
        if (!name) continue; /* never matches, as in the linear search */
        for (h = name_hash(name); ix->slot[h & ix->mask]; h++) {
            if (strcmp(name_at(ix, ix->slot[h & ix->mask] - 1), name) == 0) break;
        }
        if (!ix->slot[h & ix->mask]) ix->slot[h & ix->mask] = i + 1; /* keep the first */
    }
}

int32_t mp6_name_index_find(const Mp6NameIndex *ix, const char *name)
{
    uint32_t h;
    for (h = name_hash(name); ix->slot[h & ix->mask]; h++) {
        int32_t i = ix->slot[h & ix->mask] - 1;
        if (strcmp(name_at(ix, i), name) == 0) return i;
    }
    return -1;
}

void mp6_name_index_free(Mp6NameIndex *ix)
{
    free(ix->slot);
    ix->slot = NULL;
}
