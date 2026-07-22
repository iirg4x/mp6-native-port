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

/* The shadow map's COPY-DESTINATION texel count (GXSetTexCopyDst +
 * SetShadowTex's GXInitTexObj + the buffer alloc). At 1x this is an exact
 * pass-through (byte-identical contract); at 2x only the dst texel count
 * grows -- the render pass stays pristine and EFB-bounded (the pristine
 * 240 size cap IS the 480px EFB box over the 2x supersample, and aurora
 * renders + resolves it window-scaled), 2x being exactly where dst texels
 * equal the supersampled src pixel-for-pixel. Above 2x the windowed
 * (aurora) build renders the pass into a DEDICATED OFFSCREEN target
 * instead (Hu3DShadowExec brackets the pristine viewport/scissor/copy
 * blocks -- scaled to offscreen units -- with mp6_shadow_offscreen_begin/
 * end below), so the returned size*scale is real detail there, resolved
 * through aurora's GX_AURORA_SET_COPY_MIP_GEN mip chain and sampled with
 * trilinear minification. Headless has no renderer at all, so its scale
 * is structurally 1 (native) and the old 2x EFB-detail clamp is kept
 * there purely as the documented fallback shape. */
int mp6_shadow_effective_size(int size);

/* Offscreen shadow-pass bracket, engaged by Hu3DShadowExec only when the
 * effective scale exceeds 2 (never at 1x/2x -- those keep the pristine
 * in-EFB pass exactly as before). Windowed build: begin emits aurora's
 * GXCreateFrameBuffer(sidePx, sidePx) (GX_AURORA_BEGIN_OFFSCREEN --
 * inside it GXSetViewport/GXSetScissor/GXSetTexCopySrc map 1:1 to
 * offscreen pixels, and GXSetTexCopyDst's texel count is honored
 * unscaled) plus GXSetTexCopyMipGen(1) so the resolve carries a full mip
 * chain; end reverses both and resumes the EFB pass with its prior
 * content and viewport/scissor. Headless build: both are no-ops (the
 * scale is fixed 1 there, so the bracket is unreachable -- these exist
 * so the shared hsfman.c patch links in both build modes). */
void mp6_shadow_offscreen_begin(int sidePx);
void mp6_shadow_offscreen_end(void);

/* Offscreen shadow-pass scissor, in the offscreen target's own physical
 * pixels. Sets aurora's RENDER scissor directly (GXSetScissorRender /
 * GX_AURORA_LOAD_SCISSOR_RENDER) instead of the logical GXSetScissor.
 *
 * Why this exists: GXSetScissor packs its rect into the GameCube's 11-bit
 * SU_SCIS register fields (max 2047, with GX's +342 guard-band offset), and
 * aurora reads them back the same 11-bit way (lib/gx/command_processor.cpp,
 * BP regs 0x20/0x21). That is faithful to real hardware -- where the EFB is
 * at most 640x528 so the register never overflows -- but the offscreen
 * shadow pass scales the scissor to (size*scale*2) units, and above 4x that
 * exceeds 2047: the 8x box (top-left+3040, max coord 3397) truncates to a
 * ~992px corner, and the 16x box (max coord 6453) wraps BELOW its own top
 * edge, collapsing to an empty rect -- the exact "8x corner sliver / 16x
 * empty" buffer-dump symptom. The render scissor is expressed in physical
 * target pixels and is NOT packed, so the full offscreen box survives at any
 * scale. Only the offscreen (>2x) branch uses this; 1x/2x keep the pristine
 * GXSetScissor byte-for-byte. Headless build: no-op (the offscreen bracket
 * is unreachable there, scale is fixed 1). */
void mp6_shadow_offscreen_scissor(int x, int y, int w, int h);

/* Per-shadow-lifetime effective-size latch (mp6_shadow_quality.c). The
 * hsfman.c create sites call mp6_shadow_latch_size() with the shadow's copy-
 * destination buffer and the effective size its allocation was sized for; the
 * Hu3DShadowExec / hsfdraw bind sites call mp6_shadow_latched_size() instead
 * of mp6_shadow_effective_size(), so a mid-run Shadow Quality change neither
 * resizes a live shadow (matching the "next scene" UI contract) nor resolves
 * more copy-dst texels than that buffer holds. An unlatched buffer falls back
 * to the live effective size. */
void mp6_shadow_latch_size(const void *buf, int effSize);
int mp6_shadow_latched_size(const void *buf, int nativeSize);

#ifdef __cplusplus
}
#endif

#endif /* MP6_SHADOW_QUALITY_H */
