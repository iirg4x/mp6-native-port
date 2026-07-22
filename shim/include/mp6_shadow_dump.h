/* MP6 native port -- debug lever: MP6_SHADOW_DUMP (docs reference: the
 * Mods-page Shadow Quality feature, shim/include/mp6_shadow_quality.h,
 * commit c31363a "shadow quality: offscreen shadow pass + mip chain").
 *
 * PURPOSE. Shadow Quality raises the resolution of the Hu3DShadow* real-
 * time projected shadow map (game/hsfman.c's Hu3DShadowExec resolves the
 * caster pass into Hu3DShadow->buf via GXCopyTex). On this renderer
 * (external_refs/repos/aurora) that resolve is GPU-only: aurora keys the
 * resolved copy texture by the copy-dest POINTER (Hu3DShadow->buf's own
 * value, since this is a native recompilation -- GX calls carry real
 * process pointers, not translated hardware addresses) and never writes
 * the CPU bytes at that pointer at all (aurora::gx::g_gxState.copyTextures,
 * lib/dolphin/gx/GXFrameBuffer.cpp's copy_tex()). So a CPU-side texdump of
 * Hu3DShadow->buf reads stale heap garbage -- it cannot prove the shadow
 * map actually contains a shadow silhouette. This lever proves it for
 * real: MP6_SHADOW_DUMP=1 makes mp6_shadow_dump_tick() (called once per
 * tick, see platform/gx/aurora_bridge.c's VIWaitForRetrace, right after
 * that tick's aurora_end_frame() -- the same hook point framescope.c
 * already uses) ask aurora for an actual GPU->CPU readback of the
 * resolved copy texture at Hu3DShadow->buf and write it straight to a PNG
 * (aurora-patches/0016's aurora_gx_debug_dump_copy_png(dest, path), which
 * looks up that same copyTextures entry, reads mip level 0 back from the
 * GPU, and writes an 8-bit grayscale PNG itself -- aurora already links
 * libpng for its own PNG texture loader, lib/gfx/png_io.cpp).
 *
 * OFF BY DEFAULT, ZERO COST UNARMED: unset, mp6_shadow_dump_tick() is one
 * cached getenv() and an early return -- no allocation, no GX/aurora call,
 * no output. Windowed (Aurora) build only -- headless has no renderer, no
 * GPU, no copy texture to read back, so mp6_shadow_dump_tick() is a
 * standing no-op there (mirrors mp6_shadow_quality.h's headless split).
 *
 * MP6_SHADOW_DUMP=<nonzero>: arms it. Every tick Hu3DShadow is non-NULL
 * and has a buffer, tries a readback+PNG write to
 * build/shadowdump_<tick>.png; stops after a small fixed number of
 * successful dumps (see platform/gx/shadow_dump.c) so a long run never
 * spams the build/ directory. `[SHADOWDUMP]` lines report each attempt. */
#ifndef MP6_SHADOW_DUMP_H
#define MP6_SHADOW_DUMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per tick (platform/gx/aurora_bridge.c's VIWaitForRetrace,
 * right after that tick's aurora_end_frame() -- the point at which any
 * shadow copy this tick made is guaranteed already resolved). No-op unless
 * MP6_SHADOW_DUMP is set to a nonzero value. */
void mp6_shadow_dump_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_SHADOW_DUMP_H */
