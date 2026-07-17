/* MP6 native port -- GXSetArray real-size registry.
 *
 * decomp's own GXSetArray call sites (game/hsfdraw.c, game/hsfanim.c,
 * game/sprput.c, game/printfunc.c) only ever pass (attr, data, stride) --
 * real hardware addresses memory directly, there is no GPU buffer to size,
 * so the original SDK signature never needed one. Aurora's own GXSetArray
 * takes a REAL 5th argument (size) and genuinely uses it: lib/gx/
 * command_processor.cpp's push_gx_draw() does
 * `gfx::push_storage(array.data, array.size)` to upload exactly that many
 * bytes into a real GPU-visible storage buffer that the vertex shader then
 * indexes into (GX_INDEX16 addressing). platform/gx/aurora_bridge.c's
 * mp6_GXSetArray3 bridges the 3-arg decomp call to Aurora's real 5-arg
 * one, but has no way to know the real element count from a bare pointer
 * alone; without a registered size it passes size=0 (crash-safe:
 * push_storage no-ops on a 0-length push instead of dereferencing `data`
 * at all) -- at the cost of that indexed vertex attribute uploading ZERO
 * bytes, so every vertex fetch in the shader reads out-of-range/zeroed
 * data instead of the real mesh.
 *
 * This registry closes the gap without touching decomp source at all:
 * platform/hsf/hsf_load_native.c (which allocates every vertex/normal/st/
 * color buffer and knows each one's real element count and stride) calls
 * mp6_gxarray_register() right after each allocation; aurora_bridge.c's
 * mp6_GXSetArray3 looks the pointer up and uses the real size if found,
 * falling back to the (already-safe) size=0 behavior for any OTHER
 * GXSetArray call site this registry never saw.
 *
 * Deliberately dependency-free (no decomp or Aurora headers) so both
 * platform/hsf/hsf_load_native.c (compiled in both build modes, decomp
 * header universe) and platform/gx/aurora_bridge.c (Aurora-only, Aurora's
 * OWN header universe -- see that file's header comment on why it never
 * includes decomp headers) can include this without pulling either
 * universe into the other, matching mp6_shim_log.h's own precedent. */
#ifndef MP6_GXARRAY_REGISTRY_H
#define MP6_GXARRAY_REGISTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mp6_gxarray_register(const void *data, uint32_t byteSize);
uint32_t mp6_gxarray_lookup(const void *data);

#ifdef __cplusplus
}
#endif

#endif /* MP6_GXARRAY_REGISTRY_H */
