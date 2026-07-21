/* MP6 native port -- Mods-page "Shadow Quality" (docs reference: the
 * Hu3DShadow* real-time projected shadow system, game/hsfman.c +
 * game/hsfdraw.c). Raises the GC-sized shadow MAP resolution port-side --
 * more texels sampled through the SAME projective texgen
 * (GXSetTexCoordGen(..., GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX9), a
 * shadowProj*shadowView*invCamera*model matrix producing normalized [0,1]
 * texture coords -- see hsfdraw.c's SetShadow()/FaceDrawShadow()) --
 * never a lighting or shadow-shape change. Receivers are unaffected by a
 * resolution swap because they sample by that same live-projected [0,1]
 * coordinate through a GX_CLAMP-wrapped GXInitTexObj, not by raw texel
 * index.
 *
 * mp6_shadow_quality_scale(baseSize) is the single origin-site chokepoint:
 * patches/decomp/src/game/hsfman.c.patch's Hu3DShadowMultiCreate (the only
 * Hu3DShadow* origin the shipped game actually exercises -- REL/mdpartydll/
 * mdparty.c and REL/fileseldll/filesel.c's own Hu3DShadowCreate calls) and
 * the dormant sibling Hu3DShadowMultiSizeSet both call this and store the
 * result into shadowP->size. Every consumer of that struct field --
 * Hu3DShadowExec's viewport/scissor/dataSize (hsfman.c), hsfdraw.c's
 * GXSetTexCopySrc/GXSetTexCopyDst and SetShadowTex's GXInitTexObj -- reads
 * shadowP->size FRESH at draw/texture-setup time, so scaling it once here
 * is sufficient; no other call site hardcodes the size independently.
 *
 * Two definitions exist (platform/hsf/mp6_shadow_quality.c, split
 * internally by #ifdef MP6_HEADLESS_BUILD -- mirroring mp6_widescreen.h's
 * own documented dual-definition precedent): the real one reads the
 * Mods-tab config (video.shadow_quality: 1/2/4/8/16, 1 = native) through
 * mp6_launcher_cfg_shadow_quality() (launcher_core.cpp, Aurora-only) and
 * clamps the request to what HEAP_MODEL's largest free block can actually
 * hold; --headless has no launcher/config at all (tools/build.py's
 * PLATFORM_AURORA_ONLY), so it always returns 1 (native, byte-identical)
 * -- the automated headless boot gate never sees a scaled shadow map.
 *
 * MP6_SHADOW_QUALITY=<1|2|4|8|16> env lever, checked first and winning over
 * the config when set (same shape as MP6_WIDESCREEN/MP6_TICK_HZ): needed
 * because docs/TESTING.md's "automation contract (sacred)" makes any
 * MP6_AUTO_START_TICKS/--input-script/tick-budget-argv run automation
 * mode, which by design never reads mp6_config.json -- without this lever
 * no scripted gate or screenshot drive could ever exercise a non-native
 * scale at all.
 *
 * Return value: the EFFECTIVE linear scale actually applied (always >=1).
 * Returns exactly 1 whenever the mod is off, the config is absent/
 * invalid, or the requested scale doesn't fit in HEAP_MODEL -- so
 * `shadowP->size = baseSize * mp6_shadow_quality_scale(baseSize)` reduces
 * to baseSize unchanged (byte-identical to the pristine decomp) in every
 * one of those cases, with zero extra heap queries or log output. */
#ifndef MP6_SHADOW_QUALITY_H
#define MP6_SHADOW_QUALITY_H

#ifdef __cplusplus
extern "C" {
#endif

int mp6_shadow_quality_scale(int baseSize);

/* The one number the mod actually changes: the shadow map's COPY-
 * DESTINATION texel count (GXSetTexCopyDst + SetShadowTex's GXInitTexObj +
 * the buffer alloc). The render pass itself stays pristine -- it is
 * EFB-unit-bounded (the pristine 240 size cap IS the 480px EFB box over
 * the 2x supersample) and aurora already renders AND resolves it at
 * window-scaled physical resolution, so the pass needs no help; growing
 * its viewport/scissor/copy-src past the EFB box just gets clamped to the
 * window and copies stale scene pixels back as shadow garbage (the giant-
 * blob failure). Returns size * min(heap-clamped requested scale, 2): 2x
 * is where dst texels equal the supersampled src region pixel-for-pixel;
 * higher settings clamp there (one [SHADOW] line) until the renderer
 * grows an offscreen shadow pass. Exact pass-through at native. */
int mp6_shadow_effective_size(int size);

#ifdef __cplusplus
}
#endif

#endif /* MP6_SHADOW_QUALITY_H */
