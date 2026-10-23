#ifndef MP6_CORO_LAYOUT_H
#define MP6_CORO_LAYOUT_H

#include <stddef.h>

/* Shared by the coroutine allocator and the Windows pre-graphics reservation.
 * Keep these addresses and extents stable for quick-save stack pointers. */
#define MP6_CORO_MAX_SLOTS  128
#define MP6_CORO_STACK_SIZE (1u * 1024u * 1024u)
#ifdef __EMSCRIPTEN__
/* Each wasm coroutine also carries a 256 KB asyncify buffer for its saved
 * call stack (src/host/coro_arena.c); LTO-inlined game frames are large. */
#define MP6_CORO_SLOT_SIZE  (MP6_CORO_STACK_SIZE + 0x50000u)
/* Dev builds reserve this many slots in one block at startup, so savestates
 * find them at fixed addresses (src/host/coro_arena.c); a board peaks at 16. */
#define MP6_WEB_CORO_FIXED_SLOTS 24
#else
#define MP6_CORO_SLOT_SIZE  (MP6_CORO_STACK_SIZE + 0x10000u)
#endif
#define MP6_CORO_POOL_SIZE  ((size_t)MP6_CORO_MAX_SLOTS * (size_t)MP6_CORO_SLOT_SIZE)
#define MP6_CORO_POOL_BASE  0xC0000000u

#endif
