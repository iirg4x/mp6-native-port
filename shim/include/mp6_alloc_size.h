#ifndef MP6_ALLOC_SIZE_H
#define MP6_ALLOC_SIZE_H

#include <stdint.h>

/* Normalize a HuMemDirect* request before both layers of legacy rounding:
 * OSRoundUp32B adds 31, then the native game/memory.c patch computes
 * size + sizeof(MEMORY_BLOCK) + 31 in signed s32 arithmetic.  The native
 * 64-bit MEMORY_BLOCK is 40 bytes, so 71 is the largest intermediate
 * addition (the final mask rounds that to 64).  Keep this header
 * conservatively safe for the 32-bit layout too.  Zero intentionally
 * becomes one byte (see malloc_direct.c); negatives and either overflow
 * boundary fail closed. */
static inline int mp6_humem_checked_request(int32_t requested, int32_t *roundedOut)
{
    uint32_t normalized;
    uint32_t rounded;

    if (roundedOut == 0 || requested < 0) return 0;
    normalized = requested == 0 ? 1u : (uint32_t)requested;
    if (normalized > (uint32_t)INT32_MAX - 31u) return 0;
    rounded = (normalized + 31u) & ~31u;
    if (rounded == 0 || rounded > (uint32_t)INT32_MAX - 71u) return 0;
    *roundedOut = (int32_t)rounded;
    return 1;
}

/* SDK OSAllocFromHeap requires a strictly positive u32 request. */
static inline int mp6_os_heap_checked_request(uint32_t requested, uint32_t *roundedOut)
{
    if (roundedOut == 0 || requested == 0 || requested > UINT32_MAX - 31u) return 0;
    *roundedOut = (requested + 31u) & ~31u;
    return *roundedOut != 0;
}

#endif /* MP6_ALLOC_SIZE_H */
