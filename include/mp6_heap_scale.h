/* MP6 native port -- Enhancements "Expanded heaps": the HuMem capacity
 * ladder and the arena reservation that has to contain it.
 *
 * WHAT "RETAIL" MEANS IN THIS FILE, STATED FIRST BECAUSE IT IS EASY TO GET
 * WRONG. The GameCube's own table (src/game/malloc.c, decomp, read-only) is
 * `{ 0x220000, 0xC0000, 0xB00000, 0x500000, 0 }` -- 19.9 MB sized against
 * the console's 24 MB of physical RAM. This port has never shipped those
 * numbers and cannot: src/hsf/hsf_load_native.c builds a FRESH,
 * natively-laid-out copy of every model array/struct instead of fixing up
 * pointers inside the already-loaded file buffer, so bytes-per-model is
 * genuinely higher here (see malloc_direct.c's own header for the full
 * argument). The port's shipping baseline is
 * `{ 0x220000*2, 0xC0000, 0xB00000*8, 0x500000, 0 }` -- 98 MB -- and THAT
 * is what the constants below call retail: the capacities this port
 * produces with the enhancement off. Scale 1 reproduces that table
 * byte-for-byte, which is the property tools/heap_scale_selftest.c and
 * tools/test_heap_scale_contract.py exist to hold still.
 *
 * WHY THE ARENA IS IN THIS HEADER. The four fixed HuMem heaps are OSAlloc'd
 * out of the single OS-level heap that game/init.c's InitMem creates over
 * src/os/arena.c's reservation. Scaling the capacities without scaling
 * that reservation does not produce a bigger heap; it produces
 * `HuMem> Failed OSAlloc` at boot and an uninitialized HeapTbl. So the two
 * numbers are one decision, and they are derived here from one formula:
 *
 *     arena(scale) = MP6_HEAP_RETAIL_FIXED_TOTAL * scale + headroom
 *
 * where `headroom` is defined as exactly what the 256 MB retail arena had
 * left over after the four fixed heaps (158 MB: the game's own OS-level
 * workloads, the two framebuffers InitMem carves off arenaLo, and the
 * HEAP_SPACE remainder heap HuMemInitAll builds from whatever is left).
 * Holding the headroom CONSTANT rather than multiplying it is deliberate --
 * HEAP_SPACE's size is a consequence of the arena, not a configured
 * capacity, and the enhancement is "expanded HuMem heaps", not "quadruple
 * every allocator in the process". At scale 1 the formula returns exactly
 * 256 MB, so the reservation, the [BOOT] banner, and every savestate's
 * recorded arena size are unchanged with the enhancement off.
 *
 * EVERYTHING BELOW THE MACROS IS PURE. No decomp headers, no globals, no
 * I/O -- so tools/heap_scale_selftest.c can drive the exact arithmetic the
 * runtime uses with nothing else linked in. The stateful half (reading the
 * seam, latching the answer for the life of the process) is
 * src/os/heap_scale.c.
 */
#ifndef MP6_HEAP_SCALE_H
#define MP6_HEAP_SCALE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The port's shipping baseline capacities -- see the header comment for why
 * these, and not the GameCube's own numbers, are what "retail" names here.
 * Spelled with the same `*2`/`*8` factors src/os/malloc_direct.c used
 * before this file existed so the diff that introduced the ladder does not
 * also silently restate the baseline. */
#define MP6_HEAP_RETAIL_HEAP   (0x220000u * 2u)  /* HEAP_HEAP  */
#define MP6_HEAP_RETAIL_SOUND  (0xC0000u)        /* HEAP_SOUND */
#define MP6_HEAP_RETAIL_MODEL  (0xB00000u * 8u)  /* HEAP_MODEL */
#define MP6_HEAP_RETAIL_DVD    (0x500000u)       /* HEAP_DVD   */

/* HEAP_SPACE (index 4) is deliberately absent: HuMemInitAll does not
 * configure it, it takes whatever the OS heap has left after the four fixed
 * heaps and writes the result back into the table. It is a consequence, not
 * a capacity, and nothing here scales it. */
#define MP6_HEAP_FIXED_COUNT 4
#define MP6_HEAP_TABLE_LEN   5  /* == HEAP_MAX; asserted in malloc_direct.c */

#define MP6_HEAP_RETAIL_FIXED_TOTAL \
    (MP6_HEAP_RETAIL_HEAP + MP6_HEAP_RETAIL_SOUND + \
     MP6_HEAP_RETAIL_MODEL + MP6_HEAP_RETAIL_DVD)

/* The reservation src/os/arena.c made before this file existed. */
#define MP6_ARENA_RETAIL_BYTES (256u * 1024u * 1024u)
#define MP6_ARENA_HEADROOM_BYTES \
    (MP6_ARENA_RETAIL_BYTES - MP6_HEAP_RETAIL_FIXED_TOTAL)

/* The only two settings the seam may produce (mp6_enhancements.h:
 * "returns 1 or 4"). */
#define MP6_HEAP_SCALE_RETAIL   1u
#define MP6_HEAP_SCALE_EXPANDED 4u

/* The largest arena this port can request, and the base it is reserved at
 * (the first entry of the host reservation ladder, host_win32.c/
 * host_android.c -- the arena is the first reservation the process makes,
 * so it takes that entry). Named because src/host/coro_arena.c has to
 * place the coroutine stack pool ABOVE the arena's largest possible extent:
 * the pool used to be first-fitted after the arena, which made its base a
 * function of this setting, and a moving pool base makes every coroutine
 * stack in a savestate unrestorable. See that file's MP6_CORO_POOL_BASE. */
#define MP6_ARENA_LOW_BASE 0x80000000u
#define MP6_ARENA_EXPANDED_BYTES \
    (MP6_HEAP_RETAIL_FIXED_TOTAL * MP6_HEAP_SCALE_EXPANDED + MP6_ARENA_HEADROOM_BYTES)

/* Anything that is not exactly one of the two supported settings resolves to
 * retail. Fail-safe direction: an unparseable config or a future value this
 * build does not know about must land on the byte-identical path, never on a
 * partially-applied one. Same tolerant-clamp shape
 * mp6_shadow_quality_scale() already uses for its own ladder. */
static inline unsigned int mp6_heap_scale_clamp(long requested)
{
    if (requested == (long)MP6_HEAP_SCALE_EXPANDED) return MP6_HEAP_SCALE_EXPANDED;
    return MP6_HEAP_SCALE_RETAIL;
}

/* The baseline capacity of one table slot. Out-of-range and HEAP_SPACE both
 * read back 0, matching the table's own initializer. */
static inline unsigned int mp6_heap_retail_capacity(int index)
{
    switch (index) {
    case 0: return MP6_HEAP_RETAIL_HEAP;
    case 1: return MP6_HEAP_RETAIL_SOUND;
    case 2: return MP6_HEAP_RETAIL_MODEL;
    case 3: return MP6_HEAP_RETAIL_DVD;
    default: return 0u;
    }
}

/* The ACTIVE capacity of one table slot at `scale`. At scale 1 this is the
 * baseline value unchanged -- not "the baseline rounded", not "the baseline
 * plus a guard" -- which is what makes the retail path byte-identical. */
static inline unsigned int mp6_heap_scaled_capacity(int index, unsigned int scale)
{
    return mp6_heap_retail_capacity(index) * mp6_heap_scale_clamp((long)scale);
}

/* The arena reservation that holds a `scale`-sized heap set. See the header
 * comment for why the headroom term is constant. */
static inline unsigned int mp6_heap_arena_bytes_for_scale(unsigned int scale)
{
    return MP6_HEAP_RETAIL_FIXED_TOTAL * mp6_heap_scale_clamp((long)scale) +
           MP6_ARENA_HEADROOM_BYTES;
}

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/* The baseline table, pinned. A later edit that changes one of these
 * capacities is a change to what the port ships with the enhancement OFF,
 * which is exactly the thing this lane promised not to do silently. */
_Static_assert(MP6_HEAP_RETAIL_HEAP  == 0x440000u,  "HEAP_HEAP baseline moved");
_Static_assert(MP6_HEAP_RETAIL_SOUND == 0x0C0000u,  "HEAP_SOUND baseline moved");
_Static_assert(MP6_HEAP_RETAIL_MODEL == 0x5800000u, "HEAP_MODEL baseline moved");
_Static_assert(MP6_HEAP_RETAIL_DVD   == 0x500000u,  "HEAP_DVD baseline moved");
_Static_assert(MP6_HEAP_RETAIL_FIXED_TOTAL == 0x6200000u, /* 98 MiB */
               "the four fixed heaps no longer total 98 MB");
/* Scale 1 is the identity on the arena too -- the property that keeps the
 * [BOOT] banner and every savestate's recorded arena size unchanged. */
_Static_assert(MP6_HEAP_RETAIL_FIXED_TOTAL * MP6_HEAP_SCALE_RETAIL +
                   MP6_ARENA_HEADROOM_BYTES == MP6_ARENA_RETAIL_BYTES,
               "scale 1 must reproduce the 256 MB arena exactly");
/* The expanded row must still be expressible. Both products are u32 values
 * the runtime feeds to OSAlloc/VirtualAlloc, and both stay comfortably below
 * the 4 GB the whole arena is pinned under (src/os/arena.c). */
_Static_assert(MP6_HEAP_RETAIL_MODEL <= 0xFFFFFFFFu / MP6_HEAP_SCALE_EXPANDED,
               "HEAP_MODEL x4 overflows u32");
_Static_assert(MP6_ARENA_EXPANDED_BYTES == 0x22600000u, /* 550 MiB */
               "the expanded arena is not the documented 550 MB");
_Static_assert((unsigned long long)MP6_ARENA_LOW_BASE +
                   (unsigned long long)MP6_ARENA_EXPANDED_BYTES <= 0x100000000ull,
               "the expanded arena must still end below 4GB");
#endif

/* ---------------------------------------------------------------------
 * The stateful half (src/os/heap_scale.c).
 * ------------------------------------------------------------------ */

/* The scale THIS process is running at, latched on first call and constant
 * afterwards. Latched rather than re-read because the arena reservation and
 * HuMemInitAll's table happen at different moments and must not be able to
 * disagree; see heap_scale.c for why the latch is host-owned state. */
unsigned int mp6_heap_scale_active(void);
/* Does not latch: prelaunch choices still apply to the first reservation. */
int mp6_heap_scale_restart_pending(void);

/* mp6_heap_arena_bytes_for_scale(mp6_heap_scale_active()) -- the size
 * src/os/arena.c actually reserved, and therefore the size
 * mp6_arena_size() reports to savestate capture. */
unsigned int mp6_heap_arena_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_HEAP_SCALE_H */
