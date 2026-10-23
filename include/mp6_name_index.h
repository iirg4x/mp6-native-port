/* MP6 native port -- hash lookup of the first item with a given name.
 *
 * Model and motion loading resolve names (a track's target object, an
 * attribute, a cluster) against arrays of named records. The game does that
 * with a linear strcmp search per name, which goes quadratic when a motion
 * has hundreds of tracks. An index built once over the array answers each
 * lookup with the same first match the linear search returns.
 *
 *     Mp6NameIndex ix;
 *     mp6_name_index_build(&ix, objects, sizeof(*objects), offsetof(HSF_OBJECT, name), count);
 *     i = ix.slot ? mp6_name_index_find(&ix, name) : linear_search(name);
 *     mp6_name_index_free(&ix);
 *
 * Items whose name pointer is NULL never match. build leaves ix.slot NULL
 * (use the linear search) for arrays under 8 items or when out of memory.
 */
#ifndef MP6_NAME_INDEX_H
#define MP6_NAME_INDEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t *slot;  /* item index + 1; 0 = empty */
    uint32_t mask;
    const unsigned char *items;
    size_t stride, nameOffset;
} Mp6NameIndex;

void mp6_name_index_build(Mp6NameIndex *ix, const void *items, size_t stride, size_t nameOffset,
                          int32_t count);
/* The first item named `name`, or -1. */
int32_t mp6_name_index_find(const Mp6NameIndex *ix, const char *name);
void mp6_name_index_free(Mp6NameIndex *ix);

#ifdef __cplusplus
}
#endif

#endif /* MP6_NAME_INDEX_H */
