/* MP6 native port -- the few SDK entry points the headless TU set leaves
 * undefined. The Android headless row links a shared object, where these
 * stay unresolved until the windowed link supplies them (aurora_bridge.c);
 * a wasm executable must resolve every symbol. Headless wasm only
 * (tools/build_wasm.py). No-ops, like the rest of the headless matrix
 * surface in shims_generated.c. */
#include <dolphin.h>
#include <dolphin/mtx.h>

#undef PSMTXTransApply
#undef PSMTXMultVecArraySR

void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT)
{
    (void)src; (void)dst; (void)xT; (void)yT; (void)zT;
}

void PSMTXMultVecArraySR(const Mtx m, const Vec *srcBase, Vec *dstBase, u32 count)
{
    (void)m; (void)srcBase; (void)dstBase; (void)count;
}

void GXSetLineWidth(u8 width, GXTexOffset texOffsets)
{
    (void)width;
    (void)texOffsets;
}
