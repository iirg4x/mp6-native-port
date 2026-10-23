/* MP6 native port -- first-fit heap allocation without the linear walk.
 *
 * The game's allocator (src/game/memory.c in the decomp) scans a heap's
 * address-ordered block list from its head for the first free block that
 * fits. A board start allocates thousands of small blocks into heaps whose
 * front is full, so each allocation re-walked every live block: on PC that
 * walk was 773 of the 836 ms Towering Treetop's first tick took.
 *
 * Each heap also keeps an address-sorted index of its free blocks here, and
 * memory.c takes the first indexed block that fits: the same block the walk
 * returns, found by looking at a few dozen free blocks instead of every
 * block. The heap layout, every block header and every returned address are
 * unchanged; nothing is written into free memory.
 *
 * Heaps nest: a process heap is one allocated block of an outer heap. The
 * owner of a block is the innermost registered heap containing it, so an
 * entry is dropped as soon as the outer payload holding that heap is freed,
 * shrunk away or re-initialised. A heap whose index fills up stops being
 * indexed and uses the original walk; if the heap table itself fills, every
 * heap does. The index is ordinary game data (src/os/heap_index.c is not a
 * host-state TU), so a savestate captures and restores it with the heaps it
 * describes. MP6_HEAP_HINT_VERIFY=1 re-runs the original walk on every
 * allocation and checks the whole index after every change, aborting on any
 * difference; MP6_HEAP_INDEX=0 turns the index off for A/B timing.
 *
 * Blocks are plain addresses here; memory.c owns their layout.
 */
#ifndef MP6_HEAP_INDEX_H
#define MP6_HEAP_INDEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP6_HEAP_INDEX_MAX 128
#define MP6_HEAP_FREE_MAX 1024

typedef struct Mp6HeapIndex {
    void *head;
    char *end;
    int indexed;                        /* 0 once the free list overflowed */
    int32_t freeCount;
    void *freeBlocks[MP6_HEAP_FREE_MAX]; /* this heap's free blocks, by address */
} Mp6HeapIndex;

/* The index of the heap starting at heapHead, or NULL to use the walk. */
Mp6HeapIndex *mp6_heap_index_find(void *heapHead);
/* The indexed heap that owns block (the innermost one), or NULL. */
Mp6HeapIndex *mp6_heap_index_owner(void *block);
/* Forget every heap whose head lies in [start, end). */
void mp6_heap_index_drop_range(void *start, void *end);
/* A new heap of heapSize bytes, one free block at head. */
void mp6_heap_index_register(void *head, int32_t heapSize);
/* Add or remove a free block (NULL index: no-op). A lost entry marks the
 * heap unindexed rather than trusting the index. */
void mp6_heap_index_insert(Mp6HeapIndex *index, void *block);
void mp6_heap_index_remove(Mp6HeapIndex *index, void *block);
/* MP6_HEAP_HINT_VERIFY=1. */
int mp6_heap_index_verify(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_HEAP_INDEX_H */
