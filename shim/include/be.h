/* MP6 native port -- big-endian accessor macros for CPU-parsed data.
 *
 * Aurora consumes GameCube-native GPU data (texture blobs, tiled formats,
 * display lists) UNCHANGED -- no byte-swapping needed there at all. The
 * swap burden is only the data the CPU itself parses: container/decode
 * headers, offsets, counts, sprite/anim fields, coordinates (see
 * docs/ARCHITECTURE.md, "Data endianness"). This header is force-included
 * by NOTHING --
 * it's `#include`d explicitly, per file, only by the small set of patched
 * decomp sources (see patches/decomp/ and tools/apply_patches.py) that
 * actually read such fields, so every read site stays visible/greppable
 * at its point of use rather than hiding behind a global macro.
 *
 * All three accessors take a `const void *` pointing at the first (lowest
 * address) byte of a big-endian value in memory and return it reinterpreted
 * as a native (host-endian) value. They never require the pointer to be
 * aligned -- each does a plain byte-at-a-time composition, never a typed
 * dereference -- since packed asset data routinely lands on odd offsets.
 */
#ifndef MP6_BE_H
#define MP6_BE_H

#include "dolphin/types.h"
#include <string.h>

static inline u16 mp6_be16(const void *p)
{
    const u8 *b = (const u8 *)p;
    return (u16)(((u32)b[0] << 8) | (u32)b[1]);
}

static inline u32 mp6_be32(const void *p)
{
    const u8 *b = (const u8 *)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

static inline f32 mp6_bef32(const void *p)
{
    u32 v = mp6_be32(p);
    f32 f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* Short names for use at patched read sites. Deliberately NOT prefixed
 * with mp6_ at the call site -- these are meant to read like a drop-in
 * replacement for the raw `*ptr` dereference they're replacing. */
#define be16(p) mp6_be16(p)
#define be32(p) mp6_be32(p)
#define bef32(p) mp6_bef32(p)

#endif /* MP6_BE_H */
