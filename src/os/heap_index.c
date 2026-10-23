/* MP6 native port -- free-block index for the game's heaps; see
 * include/mp6_heap_index.h. Game state: no mp6_host_section.h here. */
#include "mp6_heap_index.h"

#include <stdlib.h>
#include <string.h>

/* aurora/aurora.h; game files do not see Aurora's headers. */
void aurora_texture_sources_released(const void *begin, const void *end);

static Mp6HeapIndex g_heapIndex[MP6_HEAP_INDEX_MAX];
static int32_t g_heapIndexCount;
static int g_heapIndexOverflow;

int mp6_heap_index_verify(void)
{
    static int state = -1;
    if (state < 0) {
        const char *e = getenv("MP6_HEAP_HINT_VERIFY");
        state = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return state;
}

Mp6HeapIndex *mp6_heap_index_find(void *heapHead)
{
    int32_t i;
    if (g_heapIndexOverflow) return NULL;
    for (i = 0; i < g_heapIndexCount; i++) {
        if (g_heapIndex[i].head == heapHead) return g_heapIndex[i].indexed ? &g_heapIndex[i] : NULL;
    }
    return NULL;
}

Mp6HeapIndex *mp6_heap_index_owner(void *block)
{
    Mp6HeapIndex *owner = NULL;
    int32_t i;
    if (g_heapIndexOverflow) return NULL;
    for (i = 0; i < g_heapIndexCount; i++) {
        Mp6HeapIndex *entry = &g_heapIndex[i];
        if ((char *)block >= (char *)entry->head && (char *)block < entry->end &&
            (owner == NULL || (char *)entry->head > (char *)owner->head)) {
            owner = entry;
        }
    }
    return (owner && owner->indexed) ? owner : NULL;
}

void mp6_heap_index_drop_range(void *start, void *end)
{
    int32_t i = 0;
    /* Every release of game memory comes through here, so it is also where
     * Aurora learns which converted textures lost their source. */
    aurora_texture_sources_released(start, end);
    while (i < g_heapIndexCount) {
        char *head = (char *)g_heapIndex[i].head;
        if (head >= (char *)start && head < (char *)end) {
            g_heapIndex[i] = g_heapIndex[--g_heapIndexCount];
        } else {
            i++;
        }
    }
}

/* Position of the first indexed free block at or after block. */
static int32_t lower_bound(const Mp6HeapIndex *entry, void *block)
{
    int32_t lo = 0, hi = entry->freeCount;
    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;
        if ((char *)entry->freeBlocks[mid] < (char *)block) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void mp6_heap_index_insert(Mp6HeapIndex *entry, void *block)
{
    int32_t at;
    if (entry == NULL) return;
    if (entry->freeCount == MP6_HEAP_FREE_MAX) {
        entry->indexed = 0;
        return;
    }
    at = lower_bound(entry, block);
    memmove(&entry->freeBlocks[at + 1], &entry->freeBlocks[at],
            (size_t)(entry->freeCount - at) * sizeof(entry->freeBlocks[0]));
    entry->freeBlocks[at] = block;
    entry->freeCount++;
}

void mp6_heap_index_remove(Mp6HeapIndex *entry, void *block)
{
    int32_t at;
    if (entry == NULL) return;
    at = lower_bound(entry, block);
    if (at == entry->freeCount || entry->freeBlocks[at] != block) {
        entry->indexed = 0; /* never trust an index that lost track */
        return;
    }
    memmove(&entry->freeBlocks[at], &entry->freeBlocks[at + 1],
            (size_t)(entry->freeCount - at - 1) * sizeof(entry->freeBlocks[0]));
    entry->freeCount--;
}

void mp6_heap_index_register(void *head, int32_t heapSize)
{
    Mp6HeapIndex *entry;
    mp6_heap_index_drop_range(head, (char *)head + heapSize);
    if (g_heapIndexCount == 0 && !g_heapIndexOverflow) {
        const char *e = getenv("MP6_HEAP_INDEX");
        g_heapIndexOverflow = (e && e[0] == '0'); /* A/B: every heap uses the walk */
    }
    if (g_heapIndexCount == MP6_HEAP_INDEX_MAX) {
        g_heapIndexOverflow = 1;
        return;
    }
    entry = &g_heapIndex[g_heapIndexCount++];
    entry->head = head;
    entry->end = (char *)head + heapSize;
    entry->indexed = 1;
    entry->freeCount = 1;
    entry->freeBlocks[0] = head;
}
