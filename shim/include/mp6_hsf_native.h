/* MP6 native port -- native HSF (3D scene) deserializer.
 *
 * Real HSF model files are big-endian, 32-bit-pointer-width data designed to
 * be `memcpy`'d into RAM and used IN PLACE on real GameCube hardware (every
 * `*Load()` function in game/hsfload.c does `(HSF_SOMETHING*)((u32)fileptr+
 * ofs)` pointer surgery directly on the loaded bytes). That scheme cannot
 * work unmodified on this 64-bit little-endian port -- every pointer-typed
 * field in every HSF_* struct (game/hsfformat.h) is 8 bytes here vs. 4 on
 * the original 32-bit target, so struct strides/offsets computed by this
 * port's own `sizeof()` never match what the file actually contains, on
 * top of the endianness mismatch.
 *
 * This module (platform/hsf/hsf_load_native.c) is a DESERIALIZER, not an
 * in-place patch: it reads the big-endian, 32-bit-offset file bytes
 * explicitly (byte-at-a-time, alignment-agnostic, via shim/include/be.h)
 * and CONSTRUCTS a fresh graph of natively-laid-out HSF_* structs (real
 * 64-bit pointers, host-endian scalars) that game/hsfdraw.c, game/hsfman.c,
 * and friends consume completely unmodified -- the exact same architecture
 * the patched HuSprAnimRead (game/sprman.c.patch) uses for the (much
 * smaller) ANIM sprite format.
 *
 * patches/decomp/src/game/hsfload.c.patch's LoadHSF() calls
 * MP6_LoadHSFNative() for real work; the MP6_HSF_STUB=1 environment
 * variable (checked at runtime, once per call) selects the older
 * always-empty inert stub instead, for isolating whether a bug is in this
 * deserializer or somewhere else in the HSF consumption path.
 */
#ifndef MP6_HSF_NATIVE_H
#define MP6_HSF_NATIVE_H

#include "game/hsfformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as the original LoadHSF(void *data): `data` is a fully
 * decoded (decompressed), still-big-endian HSF file image, freshly
 * allocated by the caller (HuDataSelHeapReadNum et al.) and never freed by
 * this function or its caller -- consistent with the original's own
 * "the file buffer becomes part of the live model forever" contract, since
 * this deserializer's string fields point directly into it (see the .c
 * file's own header comment on why that's safe with no conversion step).
 * Returns a freshly HuMemDirectMalloc'd (HEAP_MODEL) HSF_DATA graph, ready
 * for game/hsfman.c's MakeDisplayList()/Hu3DModelCreate() to consume. Never
 * returns NULL for a non-NULL `data` (a defensively-inert, zeroed HSF_DATA
 * is returned instead of crashing if the header looks implausible -- see
 * the .c file's own sanity-check comment). */
HSF_DATA *MP6_LoadHSFNative(void *data);

#ifdef __cplusplus
}
#endif

#endif /* MP6_HSF_NATIVE_H */
