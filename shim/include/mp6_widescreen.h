/* MP6 native port -- dynamic true-widescreen.
 *
 * This is a TRUE-WIDE implementation, not a present-time stretch: the
 * actual GX render target widens, the 3D camera's aspect widens to match,
 * and a small set of 2D placement APIs get a live recentering shim. An
 * earlier anamorphic-projection-widen + present-stretch approach was
 * discarded because it visibly stretches every HUD/menu element.
 *
 * Every accessor below is computed FRESH on every call (never cached) --
 * "recomputed on resize" is achieved simply by never memoizing, not by a
 * resize-event hook -- so a 4:3 window reads back the exact native
 * constants (640 / 1.0f / 0.0f) byte-for-byte (default-OFF = existing
 * behavior, unchanged), a 16:9 window reads a proportionally wider render
 * than a 21:9 window, and dragging the window live converges within one
 * tick for every layer -- 2D, the GX render target, AND the 3D
 * backdrops/cameras (this last one used to converge only at the next
 * camera-(re)creation event; mp6_widescreen_reapply(), declared at the
 * bottom of this header, closes that gap by re-deriving every registered
 * 3D backdrop/camera from a cached native baseline once per tick, cheaply,
 * with no resize-event plumbing needed).
 *
 * Included from BOTH decomp-side patches (patches/decomp/**, which read
 * these to compute a runtime-adjusted value in place of a compile-time
 * literal -- gated by a RUNTIME check inside these functions, never a
 * compile-time #ifdef, so the decomp gates + default-OFF behavior are
 * byte-unchanged -- matching mp6_shim_log.h/mp6_gxarray_registry.h's own
 * existing precedent for a small decomp-facing port header) and port-side
 * glue (platform/gx/aurora_bridge.c, platform/null/shims_manual.c,
 * platform/main_native.c).
 *
 * Two definitions exist for the first four functions below: a real one in
 * platform/gx/aurora_bridge.c (Aurora/windowed build) and a fixed-native
 * one in platform/null/shims_manual.c (--headless build has no window at
 * all, so it always reports "disabled, native size") -- the same
 * dual-build-definition pattern aurora_bridge.c's own file header already
 * documents for VIGetRetraceCount/VIGetNextField. mp6_widescreen_apply_
 * render_width() is different: it is defined exactly ONCE, inside
 * patches/decomp/src/game/init.c.patch (it needs RenderMode/VIConfigure,
 * which only that decomp-compiled TU can touch directly without crossing
 * an ABI boundary) -- that single definition links into BOTH build modes
 * because game/init.c itself is shared, and is a provable no-op in
 * --headless (its arg always already matches RenderMode->fbWidth there,
 * since mp6_widescreen_render_width() unconditionally returns 640 in that
 * build).
 */
#ifndef MP6_WIDESCREEN_H
#define MP6_WIDESCREEN_H

#include <math.h> /* mp6_widescreen_cover_fov()'s atan/tan -- a plain,
                   * always-safe standard header; any REL .c file that also
                   * pulls in the decomp's own "dolphin/math.h" declares the
                   * exact same double atan(double)/tan(double) signatures,
                   * so there is no redeclaration conflict regardless of
                   * include order. */
#include <stdint.h> /* mp6_widescreen_extrude_model()'s modelId param --
                     * int16_t, not decomp's own HU3D_MODELID, so this
                     * header stays includable from a pure port-side TU
                     * (e.g. platform/main_native.c) that never pulls in
                     * any decomp header at all (matching
                     * mp6_gxarray_registry.h's own "no decomp/Aurora
                     * headers" precedent). Always safe: the decomp's own
                     * dolphin/types.h already needs <stdint.h> itself for
                     * this exact typedef on this toolchain (`typedef
                     * int16_t s16;`), so this is never a fresh dependency,
                     * just an explicit one -- and int16_t/HU3D_MODELID
                     * (itself `typedef s16 HU3D_MODELID`, game/hu3d.h)
                     * both resolve to the same underlying 16-bit integer
                     * type either way, so the prototype here and
                     * platform/hsf/mp6_widescreen_extrude.c's own
                     * HU3D_MODELID-typed definition are fully compatible,
                     * not just ABI-coincidentally the same size. */

#ifdef __cplusplus
extern "C" {
#endif

/* Set once, right before GameMain() (platform/main_native.c, mirroring
 * mp6_bridge_apply_content_aspect_policy()'s own call timing exactly) --
 * never called at all in --headless (no window, no setting: that call
 * site lives inside main_native.c's #else / !MP6_HEADLESS_BUILD branch),
 * so no headless stub is needed for this particular setter. */
void mp6_widescreen_set_enabled(int enabled);

/* 0 = native/disabled (the default; everything below then reads back
 * exactly the pre-widescreen constants), nonzero = dynamic-wide active. */
int mp6_widescreen_enabled(void);

/* The GX render-target width in pixels. 640 (native, unchanged) when
 * disabled; else align16(clamp(480 * liveWindowAspect, 640, gpuMax)) --
 * 480*aspect because height is pinned at 480 (never changes) and
 * liveWindowAspect = current real window's widthPx/heightPx, so this is
 * exactly 640 * scale_factor() below, rounded to a 16-px boundary
 * (matching the reference widescreen hack's own rounding convention,
 * generalized to any aspect instead of hardcoded to 16:9). The clamp's
 * upper bound is the REAL GPU device's own maxTextureDimension2D (gpuMax
 * above) -- investigated and confirmed there is no other texture/array
 * bound at this layer to blow out (GXCopyDisp/GXSetDispCopySrc/
 * GXSetDispCopyDst are no-op stubs under Aurora), so the only genuine
 * hardware limit is that one, queried live for THIS run via
 * mp6_aurora_queried_max_texture_dimension_2d() (shim/include/mp6_boot.h
 * -- captured from Aurora's own startup log line, no Aurora source
 * touched or rebuilt), falling back to 8192 (the WebGPU spec's own
 * guaranteed-minimum default for this limit, not a guess) only if that
 * line was somehow never seen. No policy-level upper cap remains -- a
 * legitimate ultra-wide window renders exactly as wide as it is, all the
 * way up to what the real GPU can actually allocate. See
 * platform/gx/aurora_bridge.c's own implementation comment for the full
 * investigation. */
int mp6_widescreen_render_width(void);

/* render_width()/640.0f -- 1.0f when disabled. The ONE ratio used for
 * every proportional rescale this feature needs: HU3D_CAMERA.aspect
 * (hsfman.c), mbWinSizeSet's sizeX (board/window.c), HuSprDispInit's 2D
 * ortho/viewport/scissor extent (game/sprput.c). Deliberately the SAME
 * ratio at every one of those sites -- keeps the 3D camera's aspect, the
 * physical render width, and the 2D logical canvas width all in lockstep
 * by construction. */
float mp6_widescreen_scale_factor(void);

/* The 2D HUD-repositioning shim's additive re-center offset, in the SAME
 * 0..576 logical coordinate space HuSprDispInit's own MTXOrtho projection
 * already uses (game/sprput.c) -- i.e. (576.0f*scale_factor() - 576.0f) /
 * 2.0f, 0.0f when disabled. This is what turns a screen-anchored literal
 * X coordinate (e.g. board/opening.c's `mbWinTopPosSet(228, 392)`) that
 * was centered-ish for the old 576-wide logical canvas into one correctly
 * centered-ish for the new, wider one -- added once, centrally, inside
 * game/window.c's HuWinPosSet / game/esprite.c's espPosSet / game/sprman.c's
 * HuSprGrpPosSet / game/gamemes.c's GameMesPosSet, instead of hand-editing
 * every affected data-table constant individually. */
float mp6_widescreen_half_width_delta(void);

/* Decomp-side (patches/decomp/src/game/init.c.patch, called from inside
 * HuSysInit -- see that patch for the full mechanism): overwrites the
 * live RenderMode->fbWidth (a plain runtime struct field several call
 * sites already read dynamically -- Hu3DCameraCreate's viewportW/
 * scissorW, InitGX's GXSetViewport/GXSetScissor) to newFbWidth and
 * re-runs VIConfigure() so Aurora's logical-fb/target-fb ratio (aurora
 * lib/gx/gx.cpp map_logical_viewport) stays exactly 1:1 at the new width
 * -- no present-time stretch, because the render itself is genuinely
 * that wide, and AuroraSetViewportPolicy stays FIT the whole time (never
 * switched to STRETCH -- widening what FIT fits TO, rather than fighting
 * FIT/STRETCH, is the whole trick). A true no-op (early-returns) when
 * newFbWidth already equals RenderMode->fbWidth, so calling this every
 * tick (as platform/gx/aurora_bridge.c's VIWaitForRetrace does, to track a
 * live interactive resize) is cheap. */
void mp6_widescreen_apply_render_width(int newFbWidth);

/* "Cover" camera framing for a 3D scene whose backdrop is fixed-size
 * geometry with no single scalable plane (fallback rule: scale a 2D
 * backdrop plane where one exists, otherwise adjust the camera so nothing
 * past the backdrop's own edge is ever revealed). Every such scene already
 * widens Hu3DCameraPerspectiveSet's `aspect` argument by
 * mp6_widescreen_scale_factor() (aspect must track the viewport's real
 * physical shape or 3D content stretches) and widens the viewport to
 * mp6_widescreen_render_width() (fills the wide render target, no
 * pillarbox bars). That combination alone is a "contain"-style Hor+
 * widen: fov (vertical) stays fixed, so the horizontal world-space extent
 * at any given depth grows by the same factor the aspect widened by --
 * which is exactly what reveals a backdrop's own edge once that extra
 * horizontal extent exceeds what the backdrop's art actually covers.
 *
 * This function turns that into "cover" instead: given the scene's NATIVE
 * (4:3) vertical FOV in degrees, it returns a SMALLER vertical FOV so that,
 * once combined with the SAME widened aspect, the horizontal world-space
 * extent at any depth stays EXACTLY what it was at native aspect -- i.e.
 * zero extra horizontal reveal past whatever the backdrop already safely
 * covered -- at the cost of a vertical zoom-in (crops a little top/bottom,
 * "cover" rather than "contain", so the corners stay backdrop rather than
 * showing empty space).
 *
 * Derivation: for a symmetric perspective frustum, width_at_Z =
 * height_at_Z * aspect, and height_at_Z = 2*Z*tan(fov/2). Holding
 * width_at_Z constant while aspect grows by scale_factor() means
 * height_at_Z (and so tan(fov/2)) must shrink by that same factor:
 * new_fov = 2 * atan(tan(old_fov/2) / scale_factor()). An exact no-op
 * (returns nativeFovDeg unchanged) whenever widescreen is disabled or the
 * live window is already native aspect (scale_factor() <= 1.0f). */
static inline float mp6_widescreen_cover_fov(float nativeFovDeg)
{
    float k = mp6_widescreen_scale_factor();
    if (k <= 1.0f) {
        return nativeFovDeg;
    }
    {
        const double deg2rad = 0.017453292519943295;
        const double rad2deg = 57.29577951308232;
        double halfRad = (double)nativeFovDeg * 0.5 * deg2rad;
        double newHalfRad = atan(tan(halfRad) / (double)k);
        return (float)(newHalfRad * 2.0 * rad2deg);
    }
}

/* Grows a 3D backdrop model to cover a wide frame by scaling it about its
 * OWN authored visual center -- not about its local (0,0,0), which is all
 * a plain Hu3DModelScaleSet ever scales about (game/hsfman.c's Hu3DExec
 * builds world = Translate(pos) * Scale(scale) * localVertex -- scale
 * lands in LOCAL space, before pos's translation, so a model whose own
 * geometry is authored off-center from local origin visibly
 * shifts/reveals background when plain-scaled; see the .c file's own
 * header for the full derivation and the generalized formula). Hoisted
 * here from mdpartydll/mdparty.c's own file-local static function so
 * every widescreen-extruded scene (Party Mode, title, file-select,
 * mode-select, opening) calls ONE shared definition instead of
 * duplicating the ~90-line body once per REL patch.
 *
 * `modelId` is spelled as plain `int16_t` here (HU3D_MODELID's own exact
 * underlying representation -- `typedef s16 HU3D_MODELID`, game/hu3d.h,
 * and `typedef int16_t s16` on this toolchain, dolphin/types.h -- so
 * `int16_t` and `HU3D_MODELID` are fully compatible types, not just
 * ABI-coincidentally the same size) -- not as `HU3D_MODELID` itself -- so
 * this header stays dependency-free of any decomp header, matching
 * mp6_gxarray_registry.h's own established "no decomp/Aurora headers"
 * precedent for a shared port header any REL patch's own decomp-header
 * universe can include freely, AND any pure port-side TU that never pulls
 * in decomp headers at all (e.g. platform/main_native.c -- confirmed by
 * the build itself: a first cut spelling this `s16` compiled fine from
 * every REL patch's own decomp-header-rich TU but failed exactly there,
 * "unknown type name 's16'"). The real implementation (platform/hsf/
 * mp6_widescreen_extrude.c) DOES include game/hu3d.h, exactly like
 * platform/hsf/hsf_load_native.c already does, to reach Hu3DData[]/
 * HSF_DATA/Hu3DModelPosGet/ScaleGet/Set for real.
 *
 * CLAMP (non-tiling) backdrops only -- this is the scale-about-center
 * mechanism (softens a little at ultra-wide, an accepted texture-aware
 * tradeoff). A REPEAT (tiling) backdrop needs the OTHER mechanism instead
 * (mesh+UV edge extrude, stays sharp) -- see mp6_widescreen_extrude_model_
 * repeat() below.
 *
 * An exact no-op (returns immediately, zero extra calls, zero registry
 * churn) when widescreen is disabled (mp6_widescreen_enabled() == false --
 * NOT merely scale_factor() <= 1.0f, which can legitimately be true even
 * while the feature is ON, e.g. the window's initial shape happens to be
 * native 4:3; gating on the feature flag instead of the current scale
 * ensures this model still gets registered and tracks a LATER resize-up in
 * that case), matching every other mp6_widescreen_* accessor's default-off
 * contract. Assumes an UNROTATED backdrop (world = pos + scale*local, no
 * rotation term) -- every call site this project has needed so far
 * confirmed unrotated by direct read of its own Hu3DModelRotGet/no
 * Hu3DModelRotSet call; a future rotated backdrop would need the center
 * offset rotated into world space first, not handled here.
 *
 * No longer a one-shot bake: this call REGISTERS the model (caching its
 * native pos/scale/center once) and applies once immediately (unchanged
 * observable effect at the call site); mp6_widescreen_reapply() (declared
 * at the bottom of this header) then re-derives it from that cache every
 * frame, so a live window resize converges continuously instead of
 * freezing at whatever scale_factor() read at scene-load time. Safe to
 * call more than once on the same live model instance (not something any
 * current call site does, but no longer a hazard if one ever does) -- see
 * the .c file's own header comment for the full mechanism. */
void mp6_widescreen_extrude_model(int16_t modelId);

/* An XY-ONLY sibling of mp6_widescreen_extrude_model() above -- grows
 * scale.x/scale.y by scale_factor() exactly as the isotropic function
 * would, but leaves scale.z (and its position component) completely
 * untouched. For a backdrop with real Z-depth across its own sub-objects
 * (file-select's storybook swirl; platform/hsf/mp6_widescreen_extrude.c
 * has the full finding), the isotropic function's own Z growth has no
 * coverage benefit (aspect-widening is an X/Y screen-space concern) but
 * DOES push depth toward the camera's own far clip plane -- confirmed by
 * direct recapture, not assumed, to cause this engine's own per-object
 * cull check to drop whole sub-objects once k grows enough. Freezing Z
 * removes that risk entirely while keeping the identical X/Y coverage the
 * isotropic function already provided. Every other call site keeps
 * calling the plain isotropic function above, unaffected. Same
 * no-op/default-off contract as mp6_widescreen_extrude_model() above
 * (gated on mp6_widescreen_enabled(), not the current scale_factor()) and
 * the same dynamic-registry treatment -- registers once, re-derives every
 * frame. */
void mp6_widescreen_extrude_model_xy(int16_t modelId);

/* An XZ-ONLY sibling, for a flat HORIZONTAL ground-plane backdrop whose
 * Y-extent is ~0 (mdpartydll/mdparty.c's Party-floor model, "yuka" --
 * whose left/right edges become visible past the camera's own wide FOV).
 * Grows scale.x/scale.z (the two axes that widen this plane's own
 * footprint) by scale_factor(), with the matching position compensation
 * on both; scale.y (and its position component) is left completely
 * untouched, so the floor's own world-height and its computed center (the
 * play-circle) stay exactly where they were while its edges push further
 * out of frame. Same CLAMP-only, unrotated-top-level-transform contract
 * as every other variant in this header. Same no-op contract: returns
 * immediately when widescreen is disabled, or modelId/its hsf is
 * invalid. */
void mp6_widescreen_extrude_model_xz(int16_t modelId);

/* A HORIZONTAL-ONLY sibling of mp6_widescreen_extrude_model() above, for
 * a flat/2D-style "cover collage" asset that has no separable background
 * layer and should widen without ever zooming vertically (platform/hsf/
 * mp6_widescreen_extrude.c has the full derivation and the concrete case
 * -- bootDll/boot.c's title screen -- that made this needed for real).
 * Grows ONLY scale.x by scale_factor() (with the matching position
 * compensation on X alone); scale.y/z and their position components are
 * left completely untouched, so the model's own vertical framing stays
 * byte-identical to native while its horizontal extent grows exactly as
 * mp6_widescreen_extrude_model() would have grown ALL three axes. Same
 * CLAMP-only, unrotated-top-level-transform preconditions as that
 * function, PLUS an additional one this variant alone needs: the
 * camera's own "up" vector must not be rolled away from (0,1,0), so world
 * X genuinely is the screen's horizontal axis (confirmed, not assumed,
 * for boot.c's own title camera -- see the .c file's comment). Same
 * no-op/default-off contract and dynamic-registry treatment as
 * mp6_widescreen_extrude_model() above. */
void mp6_widescreen_extrude_model_horizontal(int16_t modelId);

/* The REPEAT (tiling) counterpart to mp6_widescreen_extrude_model() above.
 * Confirmed needed for real (not speculative) -- mode-select's ocean plane
 * ("sea"/"sea1", mdsel.c's fn_1_6F40 mdlId[1]) uses GX_REPEAT wrapping
 * (wrapS=wrapT=1, HSF diagnostic dump), and a whole-piece scale-about-
 * center (the CLAMP mechanism) would stretch its tile pattern -- more
 * world-space distance per repeat, the same softening problem as CLAMP,
 * which is fine for a one-off painted image but wrong for a texture whose
 * whole visual point is a uniform repeating pattern.
 *
 * Mechanism: for every HSF_OBJ_MESH sub-object of `modelId`, scales BOTH
 * the mesh's own raw LOCAL vertex positions AND its raw ST (UV) texture
 * coordinates about their respective bbox centers by scale_factor() --
 * mutating the live HSF_MESH.vertex->data/.st->data arrays in place
 * (confirmed safe: game/hsfdraw.c's objMesh binds GX_VA_POS/GX_VA_TEX0 via
 * GX_INDEX16 + GXSetArray(..., mesh.vertex->data/mesh.st->data, ...)
 * EVERY draw, i.e. GX always fetches from whatever these pointers
 * CURRENTLY hold -- the same live-buffer-mutation mechanism this engine's
 * own skinning/morph systems already rely on every frame, not a
 * display-list-baked snapshot). Scaling position AND UV by the SAME
 * factor about their own centers keeps "world units per texture repeat"
 * exactly unchanged (new_world_extent / new_uv_extent = k*world/k*uv =
 * world/uv, the original ratio) -- MORE of the same native-size tiles
 * become visible over the grown extent, instead of the same tile COUNT
 * being resampled larger (blurry) the way the CLAMP mechanism's whole-
 * piece scale would be for a tiling texture. Unlike
 * mp6_widescreen_extrude_model(), this does NOT touch the model's
 * top-level Hu3DModelPosSet/ScaleSet at all (those stay native) -- the
 * "grow about center" happens entirely inside local/authored vertex
 * space instead, and each touched sub-object's own authored min/max bbox
 * is updated to match (so cull tests against it, ObjCullCheck, stay
 * correct for the new, larger extent).
 *
 * Idempotency: this mechanism's original in-place mutation was NOT safe
 * to call twice on the same live buffer (a second call would scale the
 * ALREADY-grown positions again) -- that hazard is now removed. Every
 * touched vertex/UV buffer and per-object min/max pair is registered with
 * a cached NATIVE (pre-extrude) snapshot the first time it's seen, and
 * re-derived from that snapshot on every call (including calls made once
 * per frame by mp6_widescreen_reapply(), declared below) -- so this is
 * now safe to call repeatedly, and a live window resize re-tiles the
 * ocean continuously instead of freezing at whatever scale_factor() read
 * when the scene loaded. Every real call site still only calls this once
 * per model instance (immediately after Hu3DModelCreate/CreateData) --
 * that's just no longer a hard requirement.
 *
 * Same default-off contract as mp6_widescreen_extrude_model(): returns
 * immediately when widescreen is disabled (mp6_widescreen_enabled() ==
 * false), or when modelId/its hsf is invalid. Same unrotated-backdrop
 * assumption (see that function's own comment) -- world = pos +
 * scale*local, no rotation term. */
void mp6_widescreen_extrude_model_repeat(int16_t modelId);

/* ==========================================================================
 * Per-sub-object mechanisms, for a model that mixes content needing
 * different treatment in ONE HSF file
 * ========================================================================== */

/* Name-match predicate for mp6_widescreen_extrude_model_selective_horizontal()
 * below -- returns nonzero for a sub-object name that should be grown, zero
 * to leave it byte-identical to native. Plain `int`, not decomp's own BOOL,
 * so this header stays dependency-free of any decomp type (matching this
 * file's own established int16_t-not-HU3D_MODELID precedent). */
typedef int (*MP6WSNameMatchFn)(const char *name);

/* Grows ONLY the sub-objects of `modelId` whose own name satisfies `match`,
 * about the model's own shared hierarchical bbox center (the SAME center
 * mp6_widescreen_extrude_model() itself would use), along the horizontal
 * axis only -- every other sub-object's own vertex data, and the model's
 * OWN top-level pos/scale, are left completely untouched (this call does
 * NOT touch Hu3DModelPosSet/ScaleSet at all).
 *
 * For a scene like the title collage (TitleMdlId[2] -- 18 character-
 * portrait billboards, the copyright line, an internal logo duplicate, and
 * a dozen color-split/confetti decoration quads, ALL in ONE model) this is
 * what lets the color-split/confetti BACKGROUND widen to fill the frame
 * while Mario, every character portrait, the logo, and the copyright line
 * stay exactly native size and position -- something no whole-MODEL
 * mechanism above can express.
 *
 * Rotation-aware: a matched sub-object's own `base.rot` (if non-zero -- the
 * title's own diagonal-confetti pieces have one) is correctly accounted for
 * via the engine's own transform-and-invert matrix math, not a hand-derived
 * 2D formula -- see platform/hsf/mp6_widescreen_extrude.c's own comment for
 * the full derivation and WHY vertex-buffer mutation (not this object's own
 * base.pos/scale fields) is required for a model whose OWNING model has an
 * active baked motion (confirmed true for TitleMdlId[2] specifically).
 * Precondition: each matched sub-object is a DIRECT child of hsf->root (no
 * intermediate joint) -- true for every real call site so far; a future,
 * differently-nested caller is not generically handled (flagged, not
 * silently mis-transformed).
 *
 * Same default-off contract as every other function here (returns
 * immediately, zero registry churn, when widescreen is disabled) and the
 * same dynamic-registry treatment (registers each touched vertex buffer
 * once, re-derives it every frame from a cached native snapshot, so a live
 * window resize keeps converging). */
void mp6_widescreen_extrude_model_selective_horizontal(int16_t modelId, MP6WSNameMatchFn match);

/* On-device report on mode-select's sky: leaving clouds/stars fully
 * native-position left them mismatched against the widening checkered
 * day/night split -- some clouds ended up over the night half and some
 * stars over the day half once the boundary widened out from center while
 * the decorations stayed put. This is the REPOSITION-only sibling of
 * mp6_widescreen_extrude_model_selective_horizontal() above -- scales
 * ONLY each matched sub-object's own REST POSITION (`mesh.base.pos.x`)
 * outward from the model's shared bbox center by scale_factor() (the exact
 * same "scale about center" relationship the split plane's own geometry
 * grows by, so a decoration that started on one side of the boundary stays
 * on the same side as it widens).
 *
 * Deliberately does NOT touch vertex data (unlike every other selective/
 * border/split mechanism in this header) and NEVER writes `mesh.curr` --
 * a first attempt at this exact fix used the vertex-mutation approach and
 * was confirmed, on-device, to break each matched object's own independent
 * per-root drift animation (all objects appeared to move together as one
 * group instead of each drifting on its own track). Root-caused via direct
 * source reading of game/hsfmotion.c's Hu3DMotionExec(): `mesh.curr` is
 * reset from `mesh.base` every frame for any channel that ISN'T driven by
 * that object's own keyframe track (`GetObjTRXPtr()` resolves exclusively to
 * `&object->mesh.curr.*` fields -- animation playback never writes `base`
 * at all), so a permanent write to `base.pos.x` is the semantically correct
 * "shift this object's own resting position" operation: any Y-bob/rotation
 * drift its own per-root track independently animates every frame survives
 * completely undisturbed, while its X position permanently tracks the
 * split's own widening. No matrix math needed -- `base.pos.x` is already in
 * this object's own parent-relative frame, the same frame the shared model
 * center is computed in, so `newBaseX = centerX + k*(nativeBaseX-centerX)`
 * applies directly. Same default-off contract and dynamic-registry
 * treatment (caches each object's own native base.pos.x and the native
 * shared center once, re-derives every frame from the live k) as every
 * other function in this header. Precondition: the matched object's own X
 * position is not ALSO independently keyframed by its own motion data (true
 * for mode-select's own cloud/star drift, confirmed on-device after this
 * fix -- each one still drifts independently, per its own root, on the
 * correct day/night side, at native size).
 *
 * A later on-device report found mode-select's day-side clouds still sat
 * too far right of the stretched checkered split, not spread evenly across
 * the widened day region: added a `bias` multiplier on top of the plain
 * `(k-1)` term above, so a caller matching only the clouds can push them
 * further from center than the night-side stars (whose own predicate keeps
 * `bias=1.0f`, byte-identical to the original formula) without a second,
 * parallel mechanism -- `newBaseX = nativeBaseX + bias*(k-1)*(nativeBaseX-
 * centerX)`, which is algebraically identical to the original `centerX +
 * k*(nativeBaseX-centerX)` at `bias=1.0f`, and still an exact no-op at
 * native `k=1` regardless of `bias` (nothing to bias when there is no
 * widescreen extrusion to begin with).
 *
 * That same change also turned the `centerX` this function uses from an
 * internally-(re)computed value into an explicit CALLER-supplied parameter.
 * Mode-select's own sky model needs this function called TWICE (once for
 * stars, once for clouds, different biases) -- `mp6_ws_accum_bbox_r()`'s own
 * transform composition reads each object's CURRENT `mesh.base.pos`, so the
 * FIRST call's writes (to whichever group it matched) are visible to a
 * SECOND call's own from-scratch center recomputation, silently handing it
 * a DIFFERENT reference point than the first call used -- confirmed
 * on-device to actually flip the sign of the day-side push for the cloud
 * closest to center, not just a cosmetic discrepancy. Callers needing this
 * function only once (there are none left today, but the shape is kept
 * general) get `mp6_widescreen_model_center_x(modelId)`'s own return value
 * as `centerX` and behave exactly as before this split. */
void mp6_widescreen_extrude_model_selective_reposition_x(int16_t modelId, MP6WSNameMatchFn match, float bias, float centerX);

/* Exactly the function above, plus a hard lower bound `clampMinX` applied
 * to the resulting `base.pos.x`, so that clouds straddling the day/night
 * divide can be pushed fully onto the day (+X) half rather than barely
 * moving. Needed because the formula above scales each object's own offset
 * FROM `centerX`, so an object sitting ON the divide has a `(nativeBaseX -
 * centerX)` term of ~0 and barely moves at ANY bias -- only a floor can
 * push mode-select's own divide-straddling clouds fully across. The clamp
 * is gated on `k > 1`, so the native aspect keeps exactly-native rest
 * positions, same default-off contract as every other mechanism here; the
 * unclamped name above is unchanged and still byte-identical for its own
 * callers (e.g. the night-side stars). */
void mp6_widescreen_extrude_model_reposition_x_clamped(int16_t modelId, MP6WSNameMatchFn match, float bias, float centerX, float clampMinX);

/* Snapshots `mp6_ws_model_center()`'s own x component for `modelId` AT THE
 * MOMENT OF THE CALL -- the same shared-bbox-center value
 * mp6_widescreen_extrude_model_selective_reposition_x() used to compute
 * internally on every call, before that became an explicit parameter (see
 * that function's own updated comment for why: a caller invoking it more
 * than once for the same model must snapshot ONE center and pass the
 * IDENTICAL value to every call, or each call's own fresh recomputation
 * will disagree once an earlier call has already mutated a matched
 * object's `base.pos`). Returns 0.0f if widescreen is disabled or
 * modelId/its hsf is invalid -- safe/inert, since a caller only ever
 * consults this when about to make a widescreen-gated call anyway. */
float mp6_widescreen_model_center_x(int16_t modelId);

/* A coordinator-caught regression in the XZ-scale treatment of
 * mdpartydll/mdparty.c's Party floor ("yuka"): a plain whole-piece
 * scale-about-center visibly enlarges this floor's own BAKED diamond-tile
 * texture and its center play-circle decal, both painted into one
 * authored CLAMP image -- wrong for a tile pattern/game-logic decal that
 * should read at a fixed, native world scale regardless of window shape.
 * This is a "9-slice"/border-image-style extend for a regular subdivided
 * CLAMP grid mesh instead: finds the sub-object named `objName` within
 * `modelId` and, using ONLY that object's own already-authored min/max
 * (no separate configuration needed), pushes OUTER-boundary vertices
 * (whose native x or z already sits exactly at this object's own
 * authored min/max on that axis) further out about this object's own bbox
 * center by scale_factor() -- independently per axis, so an edge-midpoint
 * vertex slides straight outward rather than diagonally. Every INNER
 * vertex's position (and every vertex's own UV/ST data, on every vertex) is
 * never touched at all, so the mesh's own interior tiles/decal stay
 * byte-identical to native while only its outer ring of border quads
 * stretches to close the gap. Y is never touched on any vertex.
 *
 * This is the CLAMP-grid counterpart to mp6_widescreen_extrude_model_repeat()
 * above (that one is for a genuinely-tiling REPEAT texture's own mesh+UV;
 * this one is for a single authored CLAMP image that must not resample
 * larger AT ALL, only extend at its own unseen edge) -- pick whichever
 * matches the asset's own confirmed wrap mode and content, per this
 * header's own texture-aware contract (see mp6_widescreen_extrude_model()'s
 * own comment). Precondition: `objName`'s own base transform is unrotated
 * and at local origin (true for "yuka"; a future rotated/offset caller is
 * not generically handled here, matching this header's own established
 * pattern of flagging rather than guessing).
 *
 * Same default-off contract and dynamic-registry treatment as every other
 * function in this header. */
void mp6_widescreen_extrude_model_border_xz(int16_t modelId, const char *objName);

/* On-device report: stop SCALING the party pillar's cloud-border art --
 * split it down the middle and separate the halves instead, and stop
 * shoving it in Z/Y, which was dipping it into the floor. For every
 * HSF_OBJ_MESH sub-object of `modelId`, classifies its own NATIVE vertices
 * by whether their local x sits left or right of that object's own native
 * bbox center x, and TRANSLATES (never scales) every left vertex by
 * -halfWidthDelta and every right vertex by +halfWidthDelta, where
 * halfWidthDelta = (that object's own native half-width in x) *
 * (scale_factor()-1) -- exactly the amount needed to keep each half's own
 * outer edge flush with the live frame edge, given this object's own native
 * bbox already reaches the native 4:3 edge at k=1 (true for the Party
 * pillar, an already-established precondition for this exact asset). Y and
 * Z are never read or touched on any vertex -- this mechanism has NO
 * position-compensation term at all (unlike every scale-based mechanism
 * above, which all fold a `(1-k)*scale*center` offset into the model's own
 * top-level Hu3DModelPosSet precisely because scaling about an off-center
 * bbox needs one): nothing scales here, so nothing needs compensating, and
 * the model's own top-level Hu3DModelPosSet/ScaleSet is NEVER called by
 * this function -- the direct fix for a reported Z-shove that dipped this
 * backdrop into the floor plane. A center gap opens between the two
 * separated halves by construction (the whole point of "split apart"
 * instead of "scale up") -- expected to be hidden behind whatever scene
 * content/logo sits centered in front of this backdrop. Same default-off
 * contract and dynamic-registry treatment (registers each touched vertex
 * buffer once, re-derives it every frame from a cached native snapshot) as
 * every other function in this header. Precondition: each touched
 * sub-object's own base transform is unrotated and at local origin (true
 * for the Party pillar's own single "pillar" mesh object, confirmed by
 * direct HSF diagnostic dump) -- a future rotated/offset caller is not
 * generically handled here, matching this header's own established
 * pattern of flagging rather than guessing.
 *
 * NOTE: this mechanism was later retired at its one call site in favor of
 * mp6_widescreen_extend_pillar_edge() below, which avoids the Z-parallax
 * problem this split approach caused entirely (see that function's own
 * comment) -- kept here, still functional, for its own history and any
 * future caller. */
void mp6_widescreen_split_separate_x(int16_t modelId);

/* User's own diagnosis, confirmed by pixel-crop comparison against a
 * circled screenshot: "move the pillars back so the floor does not show
 * up behind the pillar." The party pillar sits ~50-150 world units NEARER
 * the camera than the golden-swirl backdrop it is meant to fully overlap
 * on screen -- sliding the pillar's own outer edge sideways (the split
 * above) sweeps a fixed camera's view ray across this real depth gap,
 * opening a parallax window onto whatever sits between/behind the two.
 * This SIBLING entry point does everything `mp6_widescreen_split_separate_x`
 * does, PLUS translates the object's own Z uniformly (every vertex, both
 * halves alike -- a pure rigid shift, so the art's own local shape is
 * exactly as untouched as the X-only split leaves it) toward that
 * background's depth, and boosts the X split delta to compensate for the
 * perspective foreshortening a Z push away from a fixed camera otherwise
 * causes (derived from the party camera's own source-confirmed pose,
 * game/objsysobj.c's omOutView() + mdparty.c's fn_1_33A0 -- see the .c
 * file's own comment for the full derivation) -- so the pillar's outer edge
 * still lands flush with the live frame edge, not short of it. `zBackAtRefK`
 * is the Z shift (world units, expected negative -- further FROM the
 * camera, the opposite direction from the rejected Z-shove mentioned
 * above) to apply at a caller-chosen reference `refK`
 * (`mp6_widescreen_scale_factor()` value, typically the widest aspect the
 * caller verified this at); both the Z shift and its X-delta compensation
 * scale linearly with `(scale_factor()-1)`, so this is exactly 0
 * (byte-identical to the plain split above) at native k=1, matching every
 * other mechanism's default-off/k=1 contract. Y is never read or touched,
 * and the shift is capped by the CALLER's own choice of `zBackAtRefK` --
 * pushing this object's own farthest native vertex behind the backdrop's
 * own depth would let the (Z-tested by default) backdrop win the depth
 * test and show through the pillar's own body, a real regression this
 * mechanism does NOT protect against by itself -- callers must choose a
 * `zBackAtRefK` that keeps a safety margin, verified on-device, not just
 * asserted.
 *
 * NOTE: also retired in favor of mp6_widescreen_extend_pillar_edge()
 * below, which sidesteps the whole Z-parallax problem by never moving or
 * splitting the pillar at all -- kept here for history and any future
 * caller. */
void mp6_widescreen_split_separate_x_zback(int16_t modelId, float zBackAtRefK, float refK);

/* Fixes the mushroom-house/fence PROJECTED shadow (a real-time,
 * GX_TG_POS-projected decal, game/hsfdraw.c's SetShadow()) stretching
 * across "yuka"'s own border_xz-extended outer ring -- root cause is that
 * GX_TG_POS reads the SAME raw vertex-position stream the ordinary render
 * uses, so any mechanism that moves the floor's own outer vertices to
 * close the widescreen gap unavoidably also moves what the shadow decal
 * projects onto. Creates a SECOND, independent instance of the SAME
 * on-disk floor asset (`Hu3DModelCreateData(dataNum)` a second time --
 * confirmed safe: it always allocates a brand new HSF_DATA/vertex buffer,
 * never a cached or shared one, see the .c file's own comment) and applies
 * the EXISTING, unchanged `mp6_widescreen_extrude_model_border_xz()` to
 * THAT duplicate instead of to the original. The ORIGINAL floor instance
 * is left completely untouched (100% native vertex data) and stays the
 * sole `HU3D_CONST_SHADOW_MAP` receiver (unchanged call site), so its own
 * projected shadow is pixel-native over the full region the native 4:3
 * game ever rendered; the new duplicate is deliberately NEVER given
 * `Hu3DModelShadowMapSet`, so its own moved outer-ring vertices never feed
 * a shadow texcoord generator at all. Trade-off (stated in the .c file's
 * own comment, not hidden): a shadow that would fall further out than the
 * native footprint (in screen space the native game never rendered at all)
 * is simply ABSENT there rather than distorted -- judged more honest than
 * a decal that visibly deforms on resize, but a real trade-off, not a full
 * elimination of every shadow ray. No shared decomp file (game/hsfdraw.c,
 * game/hsfman.c) is touched by this mechanism at all. Same default-off
 * contract as every other function in this header (a disabled build never
 * calls `Hu3DModelCreateData` a second time, so the duplicate never
 * exists); the existing border-registry/reapply machinery picks up the
 * duplicate for free (it registers by modelId exactly like any other
 * border_xz caller), so dynamic resize needs no new wiring here.
 *
 * Companion fix (user report: "baked shadow leakage in widescreen floor"):
 * before the border extension runs, the duplicate's outer ring cells are
 * retargeted -- once, STs only -- onto the measured shadow-free bands of
 * the same ys77_floor texture, so the stretched margins continue the
 * clean stripe pattern instead of smearing the art's baked mushroom-
 * house/fence shadow blobs across the widescreen ring (far cells pin t at
 * a clean row -- the stripe art is t-constant, so this is the pillar
 * strips' own pinned-UV continuation; side cells clamp their inner loop s
 * into the measured clean edge-column runs). The ORIGINAL floor instance
 * -- the shadow receiver -- keeps its authored STs and its baked shadows
 * exactly where the artist put them; the rewrite is validated against the
 * exact 25-vert/16-quad/64-ST "yuka" layout and refuses anything else.
 * See the .c helper's own comment for the full texel census. */
void mp6_widescreen_floor_border_fill_dup(int32_t dataNum, const char *objName);

/* Companion teardown for mp6_widescreen_floor_border_fill_dup() above --
 * call once per scene teardown (mdpartydll's own fn_1_87BC), alongside the
 * existing Hu3DModelKill loop. Kills every live registered duplicate and
 * resets the registry; a true no-op when widescreen was never enabled. */
void mp6_widescreen_floor_border_fill_dup_kill_all(void);

/* ==========================================================================
 * Natural scene extension -- the curated backdrop stays PIXEL-NATIVE and
 * the margins are generated from the scene's own art, as quads appended
 * (once, at registration, to a fixed capacity) into the object's OWN
 * vertex/ST/face buffers -- NOT a companion model: the title model draws
 * from `mesh.curr` under its baked reveal motion, which only faces riding
 * the object's own buffers can follow, and Hu3DModelCreateData can only
 * re-decode an on-disc asset, not host generated mesh data. Appending
 * faces post-load requires ONE display-list re-bake (MakeDisplayList --
 * the engine bakes face indices at model create; see the .c section
 * header for the full discovery + safety argument); afterwards the
 * per-tick reapply only rewrites buffer CONTENTS, which the baked indices
 * fetch live, so a window resize re-derives everything with no further
 * bakes. At k=1 every appended quad is zero-area (tiles/mirror collapse
 * to the seam edge; the ring's outer loop equals the rim verbatim) and
 * the object's live min/max read back exactly native -- and a disabled
 * build never registers/grows/re-bakes at all: default-off stays
 * byte-identical, the same contract every mechanism in this header keeps.
 * Grown buffers are allocated under the model's own HEAP_MODEL mallocNo
 * tag, so Hu3DModelKill's tag bulk-free reclaims them -- NO explicit
 * teardown call site exists or is needed (unlike the floor dup above);
 * dead registry entries are detected by the same hsf-identity liveness
 * check every registry in this header uses and recycled. Registration is
 * gated on mp6_widescreen_enabled() (never on the current k) and every
 * layout expectation is validated against the live data first (violations
 * degrade to "no extension", loudly under MP6_WS_HSF_DIAG, never to a
 * guess).
 * ========================================================================== */

/* Periodic-TILE border strips for mode-select's "sora" checker sky
 * (mdsel[2]): per side, ceil(halfW*(k-1)/tileW) strip quads on the
 * object's own plane continue the checker outward at native scale --
 * `periodPxL`/`periodPxR` are the measured per-half x-periods in texels
 * (day 93 / night 95 on the 512-wide bitmap) and every strip's UV window
 * stays inside [0,1] ([0, pxL/texW] / [1-pxR/texW, 1] -- the CLAMP sampler
 * needs no override; the only texels bilinear can pull past a window edge
 * are the physical texture edge's own columns). Strip positions/windows
 * are constants; only the live COUNT varies with k (k=1.625 -> 2+2, 1.775
 * -> 3+3, k=1 -> 0). `kCap` sizes the fixed appended capacity (4.0 covers a
 * ~5.3:1 window; wider saturates). Replaces an earlier selective stretch of
 * sora's own verts, restoring native diamond proportions; the measured
 * seam residual is invisible at 1:1 -- proven by the design A/B renders
 * this call realizes (tools/mp6scene/build/wsdesign_modeselect_*).
 * NOTE the load-bearing companion: the star/cloud reposition centre must
 * come from mp6_widescreen_model_center_x_widened() below, snapshotted
 * BEFORE this call -- see its comment. */
void mp6_widescreen_extend_tiles(int16_t modelId, const char *objName,
                                 float halfW, int32_t texW,
                                 int32_t periodPxL, int32_t periodPxR, float kCap);

/* MIRROR border quads for the title's "grid5" (back_ex day/night panel)
 * and "grid14" (back_house town strip) in title[0x14]: ONE quad per side
 * spanning halfW*(k-1), u running BACKWARD from the fold (u=0 at x=-halfW
 * -- C0-exact, the native edge texel; outer u = (k-1)/2, clamped at 1.0:
 * a single fold is valid to k=3 / a 3.6:1 window, past which the margin
 * holds the full mirrored texture -- no double fold is specified). v
 * carries the object's own span unchanged, and the object's own base TRS
 * (grid14's scale (1,0.2,1) / pos (0,-230,10)) applies to the appended
 * quads exactly as to the source since they live in the same buffers.
 * Chosen over tiling because neither texture has ANY measurable
 * x-periodicity. Replaces an earlier selective stretch, restoring round
 * balloons/stars; the one visible artifact (a twinned balloon at the
 * day-side fold) was judged acceptable on the A/B renders
 * (wsdesign_title_*). */
void mp6_widescreen_extend_mirror(int16_t modelId, const char *objName, float halfW);

/* Pinned-UV rim RING for the party golden-swirl disc ("CP", mdparty[3]):
 * one quad per fan chord (exactly 16 -- validated against the live data),
 * inner vertices the rim vertices VERBATIM (watertight), outer the same
 * positions scaled by k about the disc's own origin, both loops carrying
 * each chord's own rim ST indices -- the ring samples, radially constant,
 * the exact UV chord the disc's outer edge samples (C0 pixel-exact; the
 * chord dips at most 1-cos(11.25 deg) = 0.0192 UV into the measured flat
 * opaque gold band). Register ONCE, on the BASE model, BEFORE its
 * Hu3DModelLink twin is created (mdparty's fn_1_9218 i==0 iteration): the
 * link's Hu3DObjDuplicate then copies the already-grown bake and shares
 * the buffers, so both instances ride the one per-tick rewrite -- exactly
 * how they already share the HSF. Replaces an earlier whole-model
 * isotropic extrude; two deliberate, user-accepted consequences: the
 * authored gold surround becomes visible in wide corners (the 4:3 frame
 * corner already grazes it at native -- the authored composition
 * continuing), and panel1/panel2 (the live board-map render targets) no
 * longer scale with the window (native foreground size). */
void mp6_widescreen_extend_ring(int16_t modelId, const char *objName);

/* Pinned-UV EDGE EXTRUSION for the party proscenium border ("pillar",
 * mdparty[0]) -- REPLACES mp6_widescreen_split_separate_x_zback at that
 * call site (mdpartydll's fn_1_861C i==0; the split/zback functions above
 * stay for their history and any future caller, but the party pillar no
 * longer uses them). User-directed ("extrude the vertice instead of
 * straign pull to avoid stretch"): the pillar stays fully NATIVE -- no
 * split, no Z-back, centre-x untouched, arch corner pieces continuous,
 * zero native verts/faces/UVs/normals written -- and the 7 outermost verts
 * per side (|x| = 800 exactly, in three measured chains: column front
 * z=-750, arch front z=-700, arch rim pair z=-700/-800) are DUPLICATED
 * into the object's own grown buffers; only the 16 duplicates move (x =
 * +-800k, y/z copied), bridged by 10 fixed-capacity quads whose loops all
 * carry the SOURCE edge loops' own STs (pinned -- horizontally constant,
 * the rim ring's trick), continuing the border art outward at native
 * texel density with zero stretch. Reach +-800k is algebraically the
 * shipped split's own outer-edge reach, >20% past the live frame edge at
 * both proven aspects. With the border unmoved the floor-parallax window
 * the earlier split/zback mechanisms needed to compensate for is
 * structurally absent -- an extended floor-gap row scan measures 0/0 at
 * 2560x1180 AND 3440x1440 where the shipped split still leaked hundreds of
 * pixels below the historically-scanned band. Registration anchors on the
 * measured layout (half width 800, 7 edge verts/side, 3/3/1 z census,
 * mirror symmetry, agreeing sheet loops, one rim-wall face per side) and
 * refuses -- loudly under MP6_WS_HSF_DIAG -- anything else; strip winding
 * is derived from the native faces' own emission cycles, not assumed. Same
 * engine mechanics throughout as the rest of this header: one grow +
 * display-list re-bake at registration (the grow now also preserves
 * constData's API-set SHADOW_MAP receiver bits across the re-bake --
 * fn_1_861C flags this very model via Hu3DModelShadowMapSet before this
 * runs), per-tick pure-arithmetic reapply (16 x-writes + the box), k=1
 * collapses every strip to zero area with min/max byte-native, disabled
 * builds never register/grow at all, buffers die with the model's own
 * tag bulk-free. */
void mp6_widescreen_extend_pillar_edge(int16_t modelId, const char *objName);

/* The load-bearing closed form behind the tile treatment above: the value
 * the SHIPPED star/cloud repositions actually consumed was model_center_x
 * taken AFTER sora's own (now-retired) selective widen -- k-DEPENDENT
 * (+702.95 at k=1, -439.35 at k=1.625, -544.79 at k=1.775) because the
 * widen is about the hierarchical centre, not 0. Under the tile treatment
 * sora never widens, so this reproduces that exact value arithmetically
 * instead:
 *     c0      = model_center_x(native model)
 *     objBox' = [c0 + k(objMin-c0), c0 + k(objMax-c0)]
 *     result  = center_x(union(every OTHER mesh's composed box, objBox'))
 * Verified bit-equal against the shipped-order computation on the real
 * mdsel[2] data at k=1/1.625/1.775 (tools/mp6scene mirrors). Collapses to
 * the plain hierarchical centre at k=1, so the caller uses it
 * unconditionally where mp6_widescreen_model_center_x() stood before.
 * Snapshot it BEFORE mp6_widescreen_extend_tiles() (the tiles extend the
 * object's live min/max, so order matters) and pass the ONE frozen value
 * to both reposition calls, exactly as established for the star/cloud
 * split above. Returns 0.0f when widescreen is disabled or the model is
 * invalid, same contract as mp6_widescreen_model_center_x(). */
float mp6_widescreen_model_center_x_widened(int16_t modelId, const char *objName);

/* ==========================================================================
 * Camera + master per-frame re-apply
 * ========================================================================== */

/* Replaces the two-call Hu3DCameraPerspectiveSet(cameraBit, fov, nearZ,
 * farZ, aspectBase * mp6_widescreen_scale_factor()) + Hu3DCameraViewportSet
 * (cameraBit, 0, 0, mp6_widescreen_render_width(), vpH, 0, 1) pair every
 * widened per-scene 3D camera in this project used to call ONCE, at scene
 * setup (mdpartydll/mdparty.c's fn_1_33A0, mdseldll/mdsel.c's fn_1_1734,
 * bootDll/boot.c's ObjectSetup/BootTitleExec, fileseldll/filesel.c's
 * ObjectSetup) -- same "frozen at scene load" defect the model extrude
 * functions above have, root-caused by the exact same pattern: each
 * scene's own per-frame camera-position updater (Party's fn_1_32D0, mode-
 * select's fn_1_1370, boot's CameraOutView, file-select/mode-select's
 * shared omOutViewMulti) re-issues Hu3DCameraPosSet every frame but NEVER
 * Hu3DCameraPerspectiveSet/ViewportSet, so the aspect/viewport baked in at
 * setup never tracks a later resize.
 *
 * ALSO issues the matching Hu3DCameraScissorSet(cameraBit, 0, 0,
 * mp6_widescreen_render_width(), vpH) -- a camera's SCISSOR rect is a
 * separate HU3D_CAMERA field from its viewport (game/hsfman.c), defaulted
 * from RenderMode->fbWidth by Hu3DCameraCreate ONCE, at creation time, same
 * as viewportW used to be before the per-frame camera-widen mechanism
 * existed -- but no widened camera call site ever called
 * Hu3DCameraScissorSet itself (confirmed: not even on real hardware, where
 * RenderMode->fbWidth is a process-lifetime constant, so this was always a
 * non-issue there). Without this, GXSetScissor (re-issued every frame from
 * the camera's own cached scissorW, game/hsfman.c) clips the frame to
 * whatever width was live when the scene/camera was CREATED, regardless of
 * how correctly the viewport itself tracks a later resize -- this was the
 * actual, confirmed cause of the black-bar/boxing symptom that survived
 * the viewport-only fix (ground-truthed by live capture + pixel
 * measurement: the viewport-only fix alone still boxed at exactly
 * oldRenderWidth/newRenderWidth of the true width, matching a stale
 * scissor exactly).
 *
 * This call applies immediately (byte-identical to the two-call pair it
 * replaces, including the disabled case, since render_width()==640 there
 * matches Hu3DCameraCreate's own native default) AND registers the camera
 * bit so mp6_widescreen_reapply() re-issues all three calls every frame
 * with the LIVE scale_factor()/render_width() -- safe with NO compounding
 * risk (Hu3DCameraPerspectiveSet/ViewportSet/ScissorSet are plain
 * absolute-value struct writes, game/hsfman.c, not relative like model
 * pos/scale). vpX/vpY/vpNearZ/vpFarZ (viewport) and scissorX/scissorY are
 * always (0, 0, 0, 1) / (0, 0) at every call site this project has needed
 * -- hardcoded inside the implementation rather than parameterized here; a
 * future site needing different values would need those added explicitly,
 * not silently assumed. */
void mp6_widescreen_camera_widen(int cameraBit, float fov, float nearZ, float farZ, float aspectBase, float vpH);

/* Investigative diagnostics (platform/hsf/mp6_widescreen_extrude.c has the
 * real implementation and full comment) -- reusable, non-scene-specific
 * HSF census tools every widescreen mechanism re-uses to re-verify a
 * model's own sub-object list (name/vertex-buffer pointer identity/bbox/
 * base transform/wrap mode) before designing a new extrude mechanism.
 * Declared here so a decomp-side call site (any REL patch) can call them
 * without a throwaway per-file `extern` -- previously each ad hoc
 * temporary call site apparently used exactly that (never committed, per
 * this project's own "remove diagnostic calls before the final commit"
 * convention; only the definitions themselves ever landed). Zero cost /
 * zero output when MP6_WS_HSF_DIAG is unset -- safe to leave declared
 * permanently, matching the functions' own already-permanent
 * definitions. */
void mp6_widescreen_debug_dump_model(int16_t modelId, const char *label);
void mp6_widescreen_debug_dump_verts(int16_t modelId, const char *objName, const char *label);

/* Per-object COMPOSED world/model-space bbox dump (walks the full
 * hierarchy, unlike mp6_widescreen_debug_dump_model()'s per-object RAW
 * base.pos, which reads (0,0,0) for a mesh placed via a separate parent
 * joint). See the .c file's own comment. */
void mp6_widescreen_debug_dump_world_bbox(int16_t modelId, const char *label);

/* The single per-frame entry point for everything in this header's
 * dynamic-registry mechanism (models, the REPEAT buffer registry, and
 * cameras) -- call once per tick. Hooked from platform/gx/aurora_bridge.c's
 * VIWaitForRetrace, the same per-tick site mp6_widescreen_apply_render_
 * width() already uses (game/init.c.patch), so a continuous interactive
 * drag-resize converges every single frame. A true no-op (a for-loop over
 * permanently-empty registries -- every registration path above is gated
 * on mp6_widescreen_enabled()) whenever widescreen is disabled, matching
 * every other accessor's default-off contract; headless builds never call
 * this at all (there is no window to resize, and mp6_widescreen_enabled()
 * is always false there regardless). */
void mp6_widescreen_reapply(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_WIDESCREEN_H */
