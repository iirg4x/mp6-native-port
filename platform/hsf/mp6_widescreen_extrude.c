/* MP6 native port -- the shared backdrop-extrude helpers. See
 * shim/include/mp6_widescreen.h for the public contract; this is the real
 * implementation, hoisted out of an earlier file-local static in
 * mdpartydll/mdparty.c so every widescreen-extruded scene (Party Mode,
 * title, file-select, mode-select, opening) shares ONE definition. Lives
 * here, not in the decomp patch tree, because it is genuinely port-original
 * logic with no decomp counterpart to diff against -- and living in
 * platform/hsf/ (alongside hsf_load_native.c, the real HSF deserializer
 * this reads the OUTPUT of) means it can #include "game/hu3d.h" directly
 * to reach Hu3DData[]/HSF_DATA/Hu3DModelPosGet/ScaleGet/Set for real.
 *
 * ==========================================================================
 * LIVE re-derivation on window resize
 * ==========================================================================
 * Every function below used to apply EXACTLY ONCE per scene, at setup --
 * fine for a fixed window size, but a live resizable window found the 3D
 * backdrops (this file) and per-scene 3D cameras stay frozen at whatever
 * mp6_widescreen_scale_factor() read when the scene loaded, unlike the 2D
 * layer (game/sprput.c's HuSprDispInit), which already re-derives every
 * frame.
 *
 * The fix is NOT "call the existing functions again every frame" -- each
 * one reads the model's CURRENT pos/scale and multiplies by k, so a second
 * call would compound. Instead: every call below REGISTERS the model (or
 * camera, or touched vertex/UV buffer) into a small, persistent registry
 * that caches its NATIVE (pre-extrude) baseline ONCE -- the expensive part
 * (the recursive bbox walk, or the REPEAT mechanism's own hierarchy walk)
 * still runs only once per model instance. A per-frame entry point,
 * mp6_widescreen_reapply(), walks every registry and re-derives EVERY
 * registered model/buffer/camera from its cached native baseline using the
 * CURRENT scale_factor() -- cheap arithmetic, no re-walk. Hooked from
 * platform/gx/aurora_bridge.c's VIWaitForRetrace, so a continuous
 * drag-resize converges every frame.
 *
 * Model-ID-reuse safety: HU3D_MODELID slots ARE reused across a play
 * session (Hu3DModelCreate scans for the first NULL-hsf slot), so a
 * registry entry is only trusted by the per-frame walker while
 * Hu3DData[modelId].hsf still equals the EXACT hsf pointer captured at
 * registration -- a dead/reused slot is silently skipped and its registry
 * slot recycled by the next call that needs one. A repeat registration on
 * the SAME live instance just refreshes center/mode and re-applies from
 * the already-cached native values, never re-reading (by then
 * already-extruded) live pos/scale as if it were native.
 *
 * Default-off contract: every registration path is gated on
 * mp6_widescreen_enabled(), NOT on scale_factor() > 1.0f -- gating on the
 * current scale would skip registering a model whose scene loads while
 * the window happens to be native-shaped, which would then never track a
 * later resize-up. When the feature is off every registry stays
 * permanently empty and mp6_widescreen_reapply() is a for-loop over zero
 * entries, matching every accessor's byte-identical-when-disabled
 * contract.
 *
 * ==========================================================================
 * THE MECHANISM
 * ==========================================================================
 * game/hsfman.c's Hu3DExec builds world = Translate(pos) * Scale(scale) *
 * localVertex -- scale lands in LOCAL space, BEFORE pos's translation, so a
 * plain Hu3DModelScaleSet always scales about local (0,0,0), never about
 * wherever the geometry is actually centered. That's a visual no-op for
 * symmetric geometry, but mdpartydll's "pillar" backdrop has a local-space
 * visual center of (0,500,-750), nowhere near origin -- plain-scaling it
 * grows the geometry away from its own middle, which is exactly what an
 * early attempt revealed as a mismatched flat-white patch.
 *
 * This computes each model's own real visual center and folds a
 * compensating position offset into the same Hu3DModelPosSet/ScaleSet call
 * pair every widescreen site uses, so the center stays put while the
 * extent grows by scale_factor():
 *   newPos   = oldPos + (1 - k) * scale * bboxCenter
 *   newScale = oldScale * k
 * The per-frame reapply re-derives this from a CACHED (oldPos, oldScale)
 * -- always the model's NATIVE values -- so repeated re-application never
 * compounds.
 *
 * ==========================================================================
 * The center must be computed through each sub-object's OWN base
 * transform, not from a flat min/max union
 * ==========================================================================
 * An early version of the bbox pass unioned every sub-object's raw
 * `mesh.min`/`mesh.max` directly, without composing that object's own
 * `mesh.base.pos/rot/scale` -- only correct if every sub-object's base
 * transform is identity, which is NOT a general property of HSF data.
 * game/hsfdraw.c's own reference bbox walker (`PGObjCalc`) instead
 * concatenates each visited object's own base transform onto the
 * accumulated parent matrix BEFORE reading `mesh.min`/`mesh.max` -- i.e.
 * those bounds are defined in the object's PRE-base-transform local frame,
 * not already model-relative. This matters for real: bootDll/boot.c's
 * title screen has a single-mesh model with `base.pos=(0,-104,25)`, a
 * Y-offset larger than its own Y-extent -- naively unioning raw min/max
 * would miscompute its center by that same offset, reproducing the exact
 * "grows away from its own middle" defect this mechanism exists to fix,
 * inside the fix itself.
 *
 * Fixed by `mp6_ws_accum_bbox_r()` below: a recursive walker that mirrors
 * `PGObjCalc`'s own transform composition exactly, starting from
 * `hsf->root` with an IDENTITY parent matrix (the goal is the bbox center
 * relative to the model's own origin, the same frame Hu3DModelPosGet/
 * ScaleGet read/write). Every mesh leaf's own min/max corners are
 * transformed by its own fully-accumulated matrix before being merged into
 * the running bbox -- exact for any depth of hierarchy and rotation, not
 * an approximation. Re-verified against Party Mode's own near-identity
 * base transforms: the computed center is unchanged from the flat-union
 * result there, so this is a strict superset of correctness. This walk
 * runs once per model instance (at first registration); the per-frame
 * reapply reuses the cached center, never re-walks.
 *
 * ==========================================================================
 * TEXTURE-AWARE CONTRACT (CLAMP here; REPEAT is the OTHER mechanism, below)
 * ==========================================================================
 * This whole-piece scale-about-center mechanism is for a CLAMP (non-
 * tiling) backdrop -- it enlarges the art, which softens a little at
 * ultra-wide aspects, an accepted tradeoff. A REPEAT (tiling) backdrop
 * needs mp6_widescreen_extrude_model_repeat() below instead.
 *
 * ==========================================================================
 * ROTATION CAVEAT (of the MODEL's own top-level transform, not sub-objects)
 * ==========================================================================
 * The final Hu3DModelPosSet/ScaleSet step assumes the MODEL's own
 * top-level transform is unrotated: world = pos + scale*local, so
 * `center`'s world-space displacement under a scale change is exactly
 * `(1-k)*scale*center` with no basis change (sub-object rotations,
 * handled by the hierarchical bbox walk above, are a different and
 * already-handled concern). A rotated top-level transform would need that
 * offset rotated into world space first -- not handled here. Every call
 * site this project has needed so far was confirmed unrotated at the top
 * level by direct evidence. */
#include "game/hu3d.h"
#include "mp6_widescreen.h"
#include "mp6_savestate.h" /* mp6_widescreen_savestate_rehydrate() */
#include "mp6_gxarray_registry.h" /* grown vertex/ST buffers re-register their real byte size */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Investigative diagnostic: dumps a model's own HSF_OBJ_MESH sub-objects
 * (name/vertex-buffer count/bbox/base transform) and its attribute pool
 * (wrapS/wrapT/linked bitmap name) -- the facts needed to find an
 * off-center bbox and confirm a backdrop piece's wrap mode before writing
 * a new extrude call for it. Zero output and one getenv (uncached -- this
 * runs at most a handful of times per process, at scene setup, never
 * per-frame, so caching isn't worth the complexity) when MP6_WS_HSF_DIAG
 * is unset. Diagnostic only: prints state, never changes behavior. Every
 * temporary call site that uses this is removed again once the scene's
 * wrap-mode/bbox facts are confirmed and the real extrude call is
 * written. */
void mp6_widescreen_debug_dump_model(HU3D_MODELID modelId, const char *label);

/* ==========================================================================
 * Shared recursive hierarchy walk (Pass 1 for BOTH mechanisms below):
 * mirrors game/hsfdraw.c's own PGObjCalc exactly (mtxRot + PSMTXScale +
 * PSMTXConcat + mtxTransCat per node, then parent-concat), starting from
 * an IDENTITY matrix at hsf->root (not the model's own top-level
 * transform -- this computes the bbox center relative to the MODEL's own
 * origin, the same frame Hu3DModelPosGet/ScaleGet read/write). Skips
 * HSF_OBJ_CAMERA/HSF_OBJ_LIGHT entirely (no case in PGObjCall's own
 * dispatch switch either) and does not recurse into HSF_OBJ_REPLICA's
 * own children (its mesh.min/max slot is a union with a `replica`
 * pointer, not real bbox data, and this project's actual call sites never
 * exercise replicas -- see the .h file's own note on this). Called
 * exactly once per model instance (from the registry upsert below), never
 * per-frame -- the per-frame reapply reuses the cached result.
 * ========================================================================== */
static void mp6_ws_accum_bbox_r(HSF_OBJECT *object, Mtx parentMtx, HuVecF *bmin, HuVecF *bmax, BOOL *any)
{
    Mtx rotMtx, mtx;
    HSF_TRANSFORM *t;
    u32 i;

    if (object == NULL || object->type == HSF_OBJ_CAMERA || object->type == HSF_OBJ_LIGHT) {
        return;
    }

    t = &object->mesh.base;
    mtxRot(rotMtx, t->rot.x, t->rot.y, t->rot.z);
    PSMTXScale(mtx, t->scale.x, t->scale.y, t->scale.z);
    PSMTXConcat(rotMtx, mtx, mtx);
    mtxTransCat(mtx, t->pos.x, t->pos.y, t->pos.z);
    PSMTXConcat(parentMtx, mtx, mtx); /* mtx: this object's own model-local accumulated transform */

    if (object->type == HSF_OBJ_MESH && object->mesh.vertex != NULL) {
        HuVecF p;
        PSMTXMultVec(mtx, &object->mesh.mesh.min, &p);
        if (!*any) {
            *bmin = p;
            *bmax = p;
            *any = TRUE;
        } else {
            if (p.x < bmin->x) bmin->x = p.x;
            if (p.y < bmin->y) bmin->y = p.y;
            if (p.z < bmin->z) bmin->z = p.z;
            if (p.x > bmax->x) bmax->x = p.x;
            if (p.y > bmax->y) bmax->y = p.y;
            if (p.z > bmax->z) bmax->z = p.z;
        }
        PSMTXMultVec(mtx, &object->mesh.mesh.max, &p);
        if (p.x < bmin->x) bmin->x = p.x;
        if (p.y < bmin->y) bmin->y = p.y;
        if (p.z < bmin->z) bmin->z = p.z;
        if (p.x > bmax->x) bmax->x = p.x;
        if (p.y > bmax->y) bmax->y = p.y;
        if (p.z > bmax->z) bmax->z = p.z;
    }
    if (object->type != HSF_OBJ_REPLICA) {
        for (i = 0; i < object->mesh.childNum; i++) {
            mp6_ws_accum_bbox_r(object->mesh.child[i], mtx, bmin, bmax, any);
        }
    }
}

/* Model-local visual-center lookup shared by both mechanisms. Returns
 * FALSE (center left unset) if the model has no real mesh anywhere in its
 * hierarchy. */
static BOOL mp6_ws_model_center(HSF_DATA *hsf, HuVecF *outCenter)
{
    Mtx identity;
    HuVecF bmin, bmax;
    BOOL any = FALSE;

    if (hsf == NULL || hsf->root == NULL) {
        return FALSE;
    }
    PSMTXIdentity(identity);
    mp6_ws_accum_bbox_r(hsf->root, identity, &bmin, &bmax, &any);
    if (!any) {
        return FALSE;
    }
    outCenter->x = (bmin.x + bmax.x) * 0.5f;
    outCenter->y = (bmin.y + bmax.y) * 0.5f;
    outCenter->z = (bmin.z + bmax.z) * 0.5f;
    return TRUE;
}

/* ==========================================================================
 * Model pos/scale registry -- CLAMP mechanisms (isotropic/XY/XZ/
 * horizontal). Caches each extruded model's NATIVE pos/scale/center ONCE;
 * mp6_widescreen_reapply() (bottom of this file) re-derives from that cache
 * every frame using the CURRENT scale_factor(), so a live resize converges
 * without ever compounding (see the file header's "live re-derivation"
 * section for the full "why not just re-call the existing function"
 * reasoning).
 * ========================================================================== */
typedef enum {
    MP6_WS_MODE_ISO = 0,        /* all three axes -- mp6_widescreen_extrude_model() */
    MP6_WS_MODE_XY,              /* X+Y only -- mp6_widescreen_extrude_model_xy() */
    MP6_WS_MODE_XZ,              /* X+Z only -- mp6_widescreen_extrude_model_xz() (the party floor) */
    MP6_WS_MODE_HORIZONTAL       /* X only -- mp6_widescreen_extrude_model_horizontal() */
} MP6WSExtrudeMode;

typedef struct {
    HU3D_MODELID modelId;
    HSF_DATA *hsf;              /* identity/liveness snapshot -- see file header */
    HuVecF nativePos;
    HuVecF nativeScale;
    HuVecF center;
    MP6WSExtrudeMode mode;
} MP6WSModelEntry;

/* Realistic distinct call sites this project has today: Party (pillar,
 * floor, ring x2 = 4), title (3), file-select (1), mode-select (1 CLAMP +
 * 1 REPEAT, the latter tracked separately below), opening (1) -- 9 total,
 * and normally far fewer than that are simultaneously LIVE (only one scene
 * is ever on-screen at a time). 32 gives comfortable headroom across a
 * long session's worth of scene re-entries before a dead slot is ever
 * needed but unavailable to recycle. */
#define MP6_WS_MODEL_MAX 32
static MP6WSModelEntry g_mp6WsModels[MP6_WS_MODEL_MAX];
static s16 g_mp6WsModelNum = 0;

static MP6WSModelEntry *mp6_ws_model_find(HU3D_MODELID modelId)
{
    s16 i;
    for (i = 0; i < g_mp6WsModelNum; i++) {
        if (g_mp6WsModels[i].modelId == modelId) {
            return &g_mp6WsModels[i];
        }
    }
    return NULL;
}

static MP6WSModelEntry *mp6_ws_model_alloc(void)
{
    s16 i;
    /* Recycle a dead slot first: its OWN modelId's CURRENT Hu3DData[].hsf
     * no longer matches what was live when this slot was registered, i.e.
     * that model instance was killed (or its modelId slot was reused by a
     * different model this registry never touched) -- either way this
     * slot's cached baseline is stale and safe to overwrite, keeping the
     * registry bounded across a long session instead of only ever
     * growing. */
    for (i = 0; i < g_mp6WsModelNum; i++) {
        if (Hu3DData[g_mp6WsModels[i].modelId].hsf != g_mp6WsModels[i].hsf) {
            return &g_mp6WsModels[i];
        }
    }
    if (g_mp6WsModelNum < MP6_WS_MODEL_MAX) {
        return &g_mp6WsModels[g_mp6WsModelNum++];
    }
    return NULL; /* exhausted -- see the .h file's own note; not expected
                  * to be hit given this project's real scene count. */
}

static void mp6_ws_apply_model_from_native(MP6WSModelEntry *e, float k)
{
    HuVecF pos = e->nativePos;
    HuVecF scale = e->nativeScale;

    switch (e->mode) {
        case MP6_WS_MODE_ISO:
            pos.x += (1.0f - k) * scale.x * e->center.x;
            pos.y += (1.0f - k) * scale.y * e->center.y;
            pos.z += (1.0f - k) * scale.z * e->center.z;
            scale.x *= k;
            scale.y *= k;
            scale.z *= k;
            break;
        case MP6_WS_MODE_XY:
            pos.x += (1.0f - k) * scale.x * e->center.x;
            pos.y += (1.0f - k) * scale.y * e->center.y;
            scale.x *= k;
            scale.y *= k;
            break;
        case MP6_WS_MODE_XZ:
            pos.x += (1.0f - k) * scale.x * e->center.x;
            pos.z += (1.0f - k) * scale.z * e->center.z;
            scale.x *= k;
            scale.z *= k;
            break;
        case MP6_WS_MODE_HORIZONTAL:
            pos.x += (1.0f - k) * scale.x * e->center.x;
            scale.x *= k;
            break;
    }
    Hu3DModelPosSet(e->modelId, pos.x, pos.y, pos.z);
    Hu3DModelScaleSet(e->modelId, scale.x, scale.y, scale.z);
}

static void mp6_ws_extrude_generic(HU3D_MODELID modelId, MP6WSExtrudeMode mode)
{
    HSF_DATA *hsf;
    HuVecF center;
    MP6WSModelEntry *e;
    BOOL freshInstance;
    float k;

    if (!mp6_widescreen_enabled() || modelId < 0) {
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        return;
    }
    if (!mp6_ws_model_center(hsf, &center)) {
        return;
    }

    e = mp6_ws_model_find(modelId);
    freshInstance = (e == NULL) || (e->hsf != hsf);
    if (e == NULL) {
        e = mp6_ws_model_alloc();
    }
    if (e == NULL) {
        /* Registry exhausted: degrade to a one-shot apply for just this
         * call (once from the model's own current --
         * at this point in every real call site, still native -- pos/
         * scale) so THIS frame is still correct; just not tracked for a
         * later resize. Flagged in the .h file's own doc comment. */
        MP6WSModelEntry tmp;
        tmp.modelId = modelId;
        tmp.hsf = hsf;
        tmp.center = center;
        tmp.mode = mode;
        Hu3DModelPosGet(modelId, &tmp.nativePos);
        Hu3DModelScaleGet(modelId, &tmp.nativeScale);
        mp6_ws_apply_model_from_native(&tmp, mp6_widescreen_scale_factor());
        return;
    }
    if (freshInstance) {
        /* First time this EXACT (modelId, hsf) pairing has been seen --
         * Hu3DModelPosGet/ScaleGet right now are genuinely NATIVE (every
         * call site in this project calls the extrude functions exactly
         * once, immediately after Hu3DModelCreate/CreateData, before
         * anything else touches pos/scale). Cache them as the baseline
         * every future re-apply (this call AND every later resize via
         * mp6_widescreen_reapply()) derives from -- never re-read after
         * this point, so a hypothetical repeat call on the SAME instance
         * can't mistake an already-extruded value for native. */
        e->modelId = modelId;
        e->hsf = hsf;
        Hu3DModelPosGet(modelId, &e->nativePos);
        Hu3DModelScaleGet(modelId, &e->nativeScale);
    }
    e->center = center; /* pure function of the model's own static mesh data -- safe to refresh */
    e->mode = mode;
    k = mp6_widescreen_scale_factor();
    mp6_ws_apply_model_from_native(e, k);
}

void mp6_widescreen_extrude_model(HU3D_MODELID modelId)
{
    mp6_ws_extrude_generic(modelId, MP6_WS_MODE_ISO);
}

/* ==========================================================================
 * XY-only variant, for a backdrop with real Z-depth that the isotropic
 * mechanism pushes toward the camera's own far clip plane
 * ==========================================================================
 * A first attempt at file-select's own residual black notch (see
 * filesel.c.patch's own call-site comment for the measured-in-pixels
 * finding) tried mp6_widescreen_extrude_model() with an extra multiplier
 * on top of scale_factor() (more of the SAME isotropic x/y/z growth) --
 * confirmed WRONG by direct recapture, not assumed: it made the backdrop
 * mostly VANISH (replaced by plain black clear, a single soft glow left
 * over) instead of closing the gap. Root-caused via the HSF diagnostic
 * dump's own per-sub-object bbox data: this backdrop is not a flat plane
 * -- its 5 sub-objects span real local-Z depth (roughly -3444..+3454
 * relative to the model's own computed center, comparable to or exceeding
 * its own X/Y extent), and the isotropic formula grows Z by the identical
 * factor it grows X/Y. Hand-checked against this scene's own camera
 * (fileseldll/filesel.c's camera bit 2, zoom/distance 2800, FOV 10 deg)
 * and its far clip (15000.0f, same call): the deepest sub-object's own
 * farthest local-Z point is ALREADY within roughly 100 units of the far
 * clip at the plain, unmultiplied scale_factor() this project already
 * ships -- any extra multiplier on Z pushes it past 15000, and this
 * engine's own per-object cull check (ObjCullCheck, referenced by this
 * same file's REPEAT-mechanism comment below) then drops the WHOLE
 * sub-object, not just its far edge -- explaining both the catastrophic
 * result of adding more isotropic scale, and quite plausibly the ORIGINAL
 * small, animation-position-dependent notch this fix targets (this
 * backdrop's own slow ambient motion can plausibly walk a marginal
 * sub-object across that same ~15000 threshold as it plays).
 *
 * Fix: grow ONLY X and Y by scale_factor() (matching what the isotropic
 * mechanism already contributes on those two axes); leave Z completely
 * untouched, so this backdrop's own Z depth never grows past whatever
 * margin it already has against the far clip, regardless of how wide the
 * window gets. Coverage-wise this loses nothing: the camera never needed
 * MORE depth to fill a wider frame in the first place (aspect widening is
 * purely a screen-space/X-Y concern for a backdrop that already sat at a
 * fixed, safe distance) -- only Z's own incidental, coverage-irrelevant
 * growth (and its clipping risk) is removed. */
void mp6_widescreen_extrude_model_xy(HU3D_MODELID modelId)
{
    mp6_ws_extrude_generic(modelId, MP6_WS_MODE_XY);
}

/* ==========================================================================
 * XZ-only variant, for a horizontal ground-plane backdrop whose Y-extent
 * is ~0
 * ==========================================================================
 * mdpartydll/mdparty.c's Party-floor model ("yuka", DATA_mdparty+1,
 * fn_1_861C's obj->mdlId[1]) is a flat horizontal plane the play-circle and
 * character-stand positions sit on -- left untouched at first, so at
 * wide/ultra-wide aspects its own native-authored left/right edges become
 * visible past the camera's own Hor+-widened FOV (an accepted tradeoff of
 * this exact camera's own widen). The isotropic mechanism is the wrong
 * tool here: growing Y too would lift/lower the floor's own world-height
 * for no reason (its Y-extent is ~0 -- there is no "vertical coverage"
 * concern, unlike a wall-style backdrop). Growing only X and Z -- the two
 * axes that actually widen this plane's own footprint -- pushes its edges
 * further out while its Y-height and its own computed center (the
 * play-circle) stay exactly where they were, keeping every character-stand
 * position registered correctly. Same CLAMP-only, unrotated-top-level-
 * transform contract as every other variant in this file (confirmed:
 * GX_CLAMP per this model's own HSF diagnostic dump, and fn_1_861C never
 * calls Hu3DModelRotSet on this model, leaving Hu3DModelCreateData's own
 * default rot=(0,0,0)). */
void mp6_widescreen_extrude_model_xz(HU3D_MODELID modelId)
{
    mp6_ws_extrude_generic(modelId, MP6_WS_MODE_XZ);
}

/* ==========================================================================
 * Horizontal-only variant, for a FLAT/2D-style "cover collage" asset
 * ==========================================================================
 * mp6_widescreen_extrude_model() above is ISOTROPIC -- it grows x/y/z by
 * the identical scale_factor(), which is exactly right for a genuine 3D
 * backdrop (Party Mode's ring/pillar, title/file-select/opening's own
 * distant scenery) where "more of the same picture, undistorted" is the
 * goal. It is the WRONG tool for an asset that is really a flat, 2D-style
 * "cover" composite meant to fill the SCREEN rather than occupy 3D space:
 * growing every axis together also zooms the vertical framing by the same
 * factor, cropping top/bottom exactly as much as it widens -- confirmed
 * needed for real (not speculative) by bootDll/boot.c's own title screen
 * (TitleMdlId[0..2], the "MARIO PARTY 6" cast-collage): the isotropic
 * extrude visibly zoomed it (Mario's own hat clipping the top edge at
 * 2.17:1), a regression that was explicitly rejected: "just scale the
 * backdrop horizontally, don't zoom." Investigation confirmed the title
 * collage has no separable background layer to treat differently (no
 * GX_REPEAT anywhere in its attribute pool either, so the REPEAT mesh+UV
 * path doesn't apply): it is one flat composite of ~33 real HSF_OBJ_MESH
 * quads (character-portrait billboards + rotated decoration/color-split
 * pieces, all at similar shallow Z) with Mario/characters/logo text all
 * baked together -- see boot.c.patch's own call-site comment for the full
 * per-object breakdown. For exactly this shape of asset, the fix is to
 * widen ONLY the horizontal axis and leave vertical framing byte-identical
 * to native.
 *
 * Same derivation as mp6_widescreen_extrude_model()'s own formula (this
 * file's top header comment has the full walkthrough), just applied to X
 * alone -- Y and Z are left completely untouched (not even re-written with
 * their own unchanged value's worth of math; genuinely skipped):
 *   newPos.x   = oldPos.x + (1 - k) * scale.x * center.x
 *   newScale.x = oldScale.x * k
 *   newPos.y/z, newScale.y/z: unchanged
 * so a model whose live scale/pos is already native on Y/Z (true for every
 * call site this project has needed so far) stays EXACTLY native on those
 * two axes while X grows and re-centers exactly as
 * mp6_widescreen_extrude_model() would have grown ALL three. Requires the
 * same unrotated-top-level-transform precondition as the isotropic
 * function (see its own comment) -- additionally requires world X to
 * genuinely be the camera's own horizontal screen axis (i.e. the camera's
 * own "up" vector is not rotated away from (0,1,0) either -- a roll would
 * mix X/Y on screen); confirmed true for boot.c's title camera specifically
 * -- not asserted generically for a future caller.
 *
 * Same no-op contract as mp6_widescreen_extrude_model(): returns
 * immediately (zero calls) when widescreen is disabled, or modelId/its hsf
 * is invalid. */
void mp6_widescreen_extrude_model_horizontal(HU3D_MODELID modelId)
{
    mp6_ws_extrude_generic(modelId, MP6_WS_MODE_HORIZONTAL);
}

/* ==========================================================================
 * Camera registry -- every scene's own 3D camera setup (Party's
 * fn_1_33A0, mode-select's fn_1_1734, boot's ObjectSetup/BootTitleExec,
 * file-select's ObjectSetup) calls Hu3DCameraPerspectiveSet+ViewportSet
 * EXACTLY ONCE too, same "frozen at scene load" defect as the models
 * above. Hu3DCameraPerspectiveSet/ViewportSet (game/hsfman.c) are plain
 * ABSOLUTE-value struct writes -- not relative/multiplicative like the
 * model pos/scale mechanism -- so re-issuing them is safe with NO
 * compounding risk and NO native-baseline caching needed: the registry
 * here just remembers each camera bit's own literal (fov, near, far,
 * aspectBase, vpH) arguments so mp6_widescreen_reapply() can recompute
 * `aspectBase * scale_factor()` / render_width() fresh every frame.
 *
 * Camera bits are reused across scenes exactly like model IDs (every scene
 * calls Hu3DCameraCreate(1) or (2), i.e. the SAME bit values 1 and 2, for
 * its own unrelated camera) -- registering "by bit" and always overwriting
 * on a fresh mp6_widescreen_camera_widen() call handles this the same way
 * the model registry's "always refresh on a genuinely new instance" does:
 * whichever scene most recently called it owns that bit's registry entry.
 * Liveness for the PER-FRAME walker is checked via HU3D_CAMERA's own kill
 * sentinel (Hu3DCameraKill sets cameraP->fov = -1 -- game/hsfman.c) so a
 * camera bit killed by its own scene's teardown is never resurrected by a
 * stale registry entry.
 * ========================================================================== */
typedef struct {
    int bit;
    float fov, nearZ, farZ, aspectBase, vpH;
} MP6WSCameraEntry;

/* Only bit values 1 and 2 are used by any call site this project has
 * (Party/mode-select/boot each use bit 1 only; file-select uses both 1 and
 * 2) -- 8 slots is generous headroom, matching HU3D_CAM_MAX's own low bit
 * count (16) without needing to size this to the full camera-bit space. */
#define MP6_WS_CAMERA_MAX 8
static MP6WSCameraEntry g_mp6WsCameras[MP6_WS_CAMERA_MAX];
static s16 g_mp6WsCameraNum = 0;

/* HU3D_CAMERA/Hu3DCamera[]/Hu3DCameraKill all index by BIT POSITION (i.e.
 * bit value 1<<i lives at Hu3DCamera[i]), matching Hu3DCameraPerspectiveSet
 * /ViewportSet/Kill's own `for(i=0,bit=1;i<HU3D_CAM_MAX;i++,bit<<=1)` loop
 * -- every call site in this project passes a single-bit value (1 or 2,
 * never a combined mask), so a plain right-shift recovers that index. */
static int mp6_ws_camera_bit_index(int bit)
{
    int idx = 0;
    while (bit > 1) {
        bit >>= 1;
        idx++;
    }
    return idx;
}

static void mp6_ws_apply_camera(MP6WSCameraEntry *e, float k)
{
    int rw = mp6_widescreen_render_width();
    Hu3DCameraPerspectiveSet(e->bit, e->fov, e->nearZ, e->farZ, e->aspectBase * k);
    Hu3DCameraViewportSet(e->bit, 0.0f, 0.0f, (float)rw, e->vpH, 0.0f, 1.0f);
    /* The SCISSOR rect is a SEPARATE camera field from the viewport
     * (game/hsfman.c's HU3D_CAMERA has independent viewportW/H and
     * scissorW/H, written by two DIFFERENT setters -- Hu3DCameraViewportSet
     * above only ever touches the former). Hu3DCameraCreate's own
     * defCamera template sets BOTH viewportW and scissorW from
     * RenderMode->fbWidth, but only ONCE, at creation time -- and every
     * frame, Hu3DExec's real draw call re-issues a live
     * GXSetScissor(cameraP->scissorX,...,cameraP->scissorW,...) using
     * whatever was cached there, the same "frozen at scene load" shape as
     * the viewport bug fixed above, just on the scissor field that fix
     * never touched. Confirmed to be the actual cause of a black-bar/
     * boxing symptom that survived the viewport-only fix: the viewport was
     * already tracking the live render_width every tick, but GXSetScissor
     * was clipping the frame to the STALE, creation-time scissor rect
     * regardless, producing exactly the observed cutoff. Keeping scissor
     * identical to viewport (both (0,0,renderWidth,vpH), the same
     * relationship Hu3DCameraCreate's own defCamera establishes at native)
     * closes that gap with no behavior change for a NATIVE/disabled camera
     * (renderWidth==640==the original literal, vpH unchanged). */
    Hu3DCameraScissorSet(e->bit, 0, 0, (unsigned int)rw, (unsigned int)e->vpH);
}

void mp6_widescreen_camera_widen(int cameraBit, float fov, float nearZ, float farZ, float aspectBase, float vpH)
{
    float k = mp6_widescreen_scale_factor();
    int rw = mp6_widescreen_render_width();

    /* Apply directly first, unconditionally -- byte-identical to the
     * original two-call pair (Hu3DCameraPerspectiveSet + ViewportSet) this
     * replaces at every call site, including when widescreen is disabled
     * (k==1.0f, render_width()==640 exactly -- the original unwidened
     * literal call). This ALSO means the very first frame of every scene
     * is correct even before mp6_widescreen_reapply() ever runs. The
     * matching Hu3DCameraScissorSet call (see mp6_ws_apply_camera's own
     * comment) is redundant with Hu3DCameraCreate's own defCamera copy on a
     * genuinely fresh camera (both already agree, since RenderMode->fbWidth
     * is already live by the time any scene creates its camera), but not
     * redundant -- and load-bearing -- for camera bit REUSE across scenes
     * (Hu3DCameraCreate is NOT called again when a new scene reuses an
     * existing bit via this same widen() call, so without this, a reused
     * bit's scissor would carry over the PREVIOUS scene's stale rect). */
    Hu3DCameraPerspectiveSet(cameraBit, fov, nearZ, farZ, aspectBase * k);
    Hu3DCameraViewportSet(cameraBit, 0.0f, 0.0f, (float)rw, vpH, 0.0f, 1.0f);
    Hu3DCameraScissorSet(cameraBit, 0, 0, (unsigned int)rw, (unsigned int)vpH);

    if (!mp6_widescreen_enabled()) {
        return; /* zero registry churn when the feature is off -- see file header */
    }
    {
        s16 i;
        MP6WSCameraEntry *e = NULL;
        for (i = 0; i < g_mp6WsCameraNum; i++) {
            if (g_mp6WsCameras[i].bit == cameraBit) {
                e = &g_mp6WsCameras[i];
                break;
            }
        }
        if (e == NULL && g_mp6WsCameraNum < MP6_WS_CAMERA_MAX) {
            e = &g_mp6WsCameras[g_mp6WsCameraNum++];
        }
        if (e != NULL) {
            e->bit = cameraBit;
            e->fov = fov;
            e->nearZ = nearZ;
            e->farZ = farZ;
            e->aspectBase = aspectBase;
            e->vpH = vpH;
        }
        /* e == NULL (registry exhausted) is not expected given this
         * project's real camera-bit count (1 and 2 only) -- if it ever
         * happens, this camera just stays at whatever the direct apply
         * above set, un-tracked for a later resize, same graceful
         * degradation as the model registry's own exhaustion path. */
    }
}

/* ==========================================================================
 * REPEAT (tiling) mechanism -- mesh+UV edge extrude
 * ==========================================================================
 * Confirmed needed for real by mode-select's ocean plane ("sea"/"sea1",
 * mdsel.c's fn_1_6F40 mdlId[1]) -- GX_REPEAT wrapping (wrapS=wrapT=1),
 * where a whole-piece scale-about-center (the CLAMP mechanism) would
 * stretch the tile pattern -- fine for a one-off painted image, wrong for
 * a texture whose entire visual point is a uniform repeating pattern.
 *
 * Mechanism: scales BOTH the raw LOCAL vertex positions AND the raw ST
 * (UV) texture coordinates of every HSF_OBJ_MESH sub-object about their
 * own centers by scale_factor() -- mutating the live HSF_MESH.vertex->
 * data/.st->data arrays in place. Confirmed safe: game/hsfdraw.c's
 * objMesh re-binds GX_VA_POS/GX_VA_TEX0 from those exact live pointers on
 * every draw (the same live-buffer-mutation mechanism this engine's own
 * skinning/morph systems already rely on every frame, not a
 * display-list-baked snapshot).
 *
 * Scaling position AND UV by the SAME factor about their own centers
 * keeps "world units per texture repeat" exactly unchanged -- MORE of the
 * same native-size tiles become visible over the grown extent, instead of
 * the same tile COUNT being resampled larger (blurry). Unlike
 * mp6_widescreen_extrude_model(), this does NOT touch the model's
 * top-level Hu3DModelPosSet/ScaleSet at all -- the "grow about center"
 * happens entirely inside local/authored vertex space, and each touched
 * sub-object's own authored min/max bbox is updated to match so culling
 * stays correct.
 *
 * PER-OBJECT LOCAL CENTER: the model-level `center` is in MODEL-local
 * space, but each sub-object's own raw vertex buffer is in THAT OBJECT's
 * own PRE-base-transform local space (same finding as the CLAMP
 * mechanism's own fix, above) -- so mutating a sub-object's vertices
 * "about `center`" directly would mix incompatible frames whenever that
 * object's base transform isn't identity. Resolved by mapping `center`
 * back into each object's own local frame via its accumulated
 * translation/scale, tracked as plain per-axis running products/sums
 * rather than a full matrix -- exact whenever no rotation appears
 * anywhere from the root down (true for mode-select's "sea"/"sea1"). A
 * sub-object reached through any rotation is left UNTOUCHED (skipped, not
 * guessed) rather than silently mis-transformed.
 *
 * ==========================================================================
 * Dynamic re-tiling on resize -- native-snapshot registry
 * ==========================================================================
 * Direct in-place mutation is NOT safe to call twice on the same live
 * buffer -- a second pass would scale the already-grown positions again,
 * compounding. So every touched vertex buffer, ST buffer, and per-object
 * min/max pair is registered ONCE (a malloc'd copy of its native,
 * pre-extrude contents snapshotted the first time that pointer is seen)
 * and re-derived every frame from that cached snapshot -- the same
 * "cache native once, recompute every frame" strategy as the CLAMP model
 * registry, applied per buffer instead of per model. The hierarchy walk
 * itself runs only once per model instance; the per-frame pass is a flat
 * loop over the already-flattened registry, no re-walk. Registering the
 * SAME buffer pointer twice (e.g. two sub-objects sharing one buffer) is
 * safe -- the second visit finds it already registered and skips
 * re-snapshotting, but still re-applies correctly. Liveness for the
 * per-frame walker: each entry stores the owning modelId and hsf pointer
 * at registration time, checked exactly like the CLAMP model registry --
 * a dead or reused slot is skipped, never resurrected. */
typedef struct {
    void *ptr;             /* live target: vb->data, stb->data, or &object->mesh.mesh.min/.max */
    float *native;         /* malloc'd once: pristine (pre-extrude) snapshot, same layout as *ptr */
    s32 count;              /* number of `stride`-sized groups */
    s16 stride;             /* 3 (HuVecF: position/bbox) or 2 (HuVec2f: UV) */
    HuVecF center3;         /* valid when stride == 3 */
    HuVec2f center2;        /* valid when stride == 2 */
    HU3D_MODELID modelId;   /* owner, for the liveness check */
    HSF_DATA *hsf;          /* owner's hsf pointer at registration time */
} MP6WSRepeatEntry;

/* mode-select's ocean ("sea"/"sea1") is the only confirmed REPEAT user
 * today -- 2 sub-objects, each contributing up to 4 entries (a vertex
 * buffer + a min + a max + an ST buffer, some possibly shared/deduped) --
 * comfortably under 32 with headroom for re-entering the scene many times
 * across a session (dead slots are recycled the same way the model
 * registry's are). */
#define MP6_WS_REPEAT_MAX 32
static MP6WSRepeatEntry g_mp6WsRepeat[MP6_WS_REPEAT_MAX];
static s16 g_mp6WsRepeatNum = 0;

typedef struct {
    HuVecF modelCenter;
    HuVec2f stCenter;
    BOOL anySt;
    HU3D_MODELID modelId;
    HSF_DATA *hsf;
} MP6WSRepeatCtx;

/* Matches by raw pointer AND liveness (Hu3DData[modelId].hsf must still
 * equal the hsf captured at registration) -- a heap allocator can and does
 * hand the SAME address to a NEW, unrelated model after an old one is
 * killed (game/hsfman.c's LoadHSF/HuMemDirectFree; a repeatedly-revisited
 * scene like mode-select's own ocean plausibly recreates its buffers at
 * the same freed address each time). Without the liveness check, a brand
 * new model's first-ever touch of such a reused address would be
 * mistaken for "already registered" and skip re-snapshotting -- applying
 * a STALE native snapshot (captured from the unrelated old model) to the
 * new model's vertex data, corrupting it. Returning NULL here for a
 * pointer-match-but-dead-owner makes the caller treat it exactly like a
 * fresh pointer (mp6_ws_repeat_alloc's own recycling may even hand back
 * this SAME now-dead slot, which is correct and intended). */
static MP6WSRepeatEntry *mp6_ws_repeat_find(void *ptr)
{
    s16 i;
    for (i = 0; i < g_mp6WsRepeatNum; i++) {
        if (g_mp6WsRepeat[i].ptr == ptr && Hu3DData[g_mp6WsRepeat[i].modelId].hsf == g_mp6WsRepeat[i].hsf) {
            return &g_mp6WsRepeat[i];
        }
    }
    return NULL;
}

static MP6WSRepeatEntry *mp6_ws_repeat_alloc(void)
{
    s16 i;
    for (i = 0; i < g_mp6WsRepeatNum; i++) {
        if (Hu3DData[g_mp6WsRepeat[i].modelId].hsf != g_mp6WsRepeat[i].hsf) {
            return &g_mp6WsRepeat[i]; /* recycle a dead slot -- see MP6WSRepeatEntry's own comment */
        }
    }
    if (g_mp6WsRepeatNum < MP6_WS_REPEAT_MAX) {
        return &g_mp6WsRepeat[g_mp6WsRepeatNum++];
    }
    return NULL;
}

static void mp6_ws_repeat_apply(MP6WSRepeatEntry *e, float k)
{
    /* A savestate restore leaves this registry's GAME half valid
     * (ptr/hsf/modelId are arena addresses and ids, all restored) but
     * nulls its HOST half -- `native` pointed into the capturing
     * process's C runtime heap, which does not exist in the restoring
     * process. mp6_widescreen_savestate_rehydrate() sets it NULL; this
     * early-out keeps the per-frame re-apply from dereferencing it before
     * the registrar re-snapshots. Skipping a frame's re-apply is
     * harmless: the geometry in the arena is already extruded (that is
     * what was captured), and the next register call re-takes the
     * snapshot. */
    if (e->native == NULL || e->count <= 0) {
        return;
    }
    s32 j;
    if (e->stride == 3) {
        HuVecF *dst = (HuVecF *)e->ptr;
        HuVecF *src = (HuVecF *)e->native;
        for (j = 0; j < e->count; j++) {
            dst[j].x = e->center3.x + k * (src[j].x - e->center3.x);
            dst[j].y = e->center3.y + k * (src[j].y - e->center3.y);
            dst[j].z = e->center3.z + k * (src[j].z - e->center3.z);
        }
    } else {
        HuVec2f *dst = (HuVec2f *)e->ptr;
        HuVec2f *src = (HuVec2f *)e->native;
        for (j = 0; j < e->count; j++) {
            dst[j].x = e->center2.x + k * (src[j].x - e->center2.x);
            dst[j].y = e->center2.y + k * (src[j].y - e->center2.y);
        }
    }
}

/* Registers `ptr` (if not already registered) with a fresh malloc'd native
 * snapshot of its CURRENT contents, then applies it (from that snapshot)
 * at the given k -- safe whether this is the first-ever touch (current ==
 * native) or a repeat visit within the same walk (native already cached,
 * re-applies from it instead of re-snapshotting the by-then-scaled live
 * data). Silently skips tracking (falls back to leaving `ptr` at whatever
 * it already held) on allocation/registry exhaustion, which this project's
 * real ocean-plane-sized meshes are not expected to hit. */
static void mp6_ws_repeat_register_and_apply(HU3D_MODELID modelId, HSF_DATA *hsf, void *ptr, s32 count, s16 stride,
                                              const HuVecF *center3, const HuVec2f *center2, float k)
{
    MP6WSRepeatEntry *e = mp6_ws_repeat_find(ptr);
    if (e == NULL) {
        size_t bytes = (size_t)count * (size_t)stride * sizeof(float);
        float *native = (float *)malloc(bytes > 0 ? bytes : 1);
        if (native == NULL) {
            return; /* OOM on a tiny alloc -- extremely unlikely; degrade to "not tracked" */
        }
        e = mp6_ws_repeat_alloc();
        if (e == NULL) {
            free(native);
            return; /* registry exhausted -- see MP6WSRepeatEntry's own doc comment */
        }
        if (e->native != NULL && e->count == count && e->stride == stride) {
            free(native); /* recycled slot already has a right-sized buffer -- reuse it */
        } else {
            free(e->native);
            e->native = native;
        }
        memcpy(e->native, ptr, bytes); /* snapshot CURRENT contents -- native, since this is the first touch */
        e->ptr = ptr;
        e->count = count;
        e->stride = stride;
        e->modelId = modelId;
        e->hsf = hsf;
    }
    if (stride == 3) {
        e->center3 = *center3;
    } else {
        e->center2 = *center2;
    }
    mp6_ws_repeat_apply(e, k);
}

static void mp6_ws_repeat_walk_r(HSF_OBJECT *object, HuVecF accumScale, HuVecF accumTrans, BOOL hasRot, MP6WSRepeatCtx *ctx, float k)
{
    HSF_TRANSFORM *t;
    HuVecF newScale, newTrans;
    BOOL newHasRot;
    u32 i;

    if (object == NULL || object->type == HSF_OBJ_CAMERA || object->type == HSF_OBJ_LIGHT) {
        return;
    }
    t = &object->mesh.base;
    newScale.x = accumScale.x * t->scale.x;
    newScale.y = accumScale.y * t->scale.y;
    newScale.z = accumScale.z * t->scale.z;
    newTrans.x = accumTrans.x + accumScale.x * t->pos.x;
    newTrans.y = accumTrans.y + accumScale.y * t->pos.y;
    newTrans.z = accumTrans.z + accumScale.z * t->pos.z;
    newHasRot = hasRot || t->rot.x != 0.0f || t->rot.y != 0.0f || t->rot.z != 0.0f;

    if (object->type == HSF_OBJ_MESH && object->mesh.vertex != NULL
        && !newHasRot && newScale.x != 0.0f && newScale.y != 0.0f && newScale.z != 0.0f) {
        HuVecF objLocalCenter;
        HSF_BUFFER *vb, *stb;

        objLocalCenter.x = (ctx->modelCenter.x - newTrans.x) / newScale.x;
        objLocalCenter.y = (ctx->modelCenter.y - newTrans.y) / newScale.y;
        objLocalCenter.z = (ctx->modelCenter.z - newTrans.z) / newScale.z;

        vb = object->mesh.vertex;
        mp6_ws_repeat_register_and_apply(ctx->modelId, ctx->hsf, vb->data, vb->count, 3, &objLocalCenter, NULL, k);

        /* This object's own authored bbox extends by the identical
         * transform applied to its vertices (a uniform per-axis scale-
         * about-center commutes with an axis-aligned bbox's own corners,
         * so this is exact) -- tracked as two independent 1-element
         * "buffers" (min, max), each object's own fields, never shared
         * with another object, so no dedup is needed for these two. */
        mp6_ws_repeat_register_and_apply(ctx->modelId, ctx->hsf, &object->mesh.mesh.min, 1, 3, &objLocalCenter, NULL, k);
        mp6_ws_repeat_register_and_apply(ctx->modelId, ctx->hsf, &object->mesh.mesh.max, 1, 3, &objLocalCenter, NULL, k);

        if (ctx->anySt) {
            stb = object->mesh.st;
            if (stb != NULL && stb->data != NULL) {
                mp6_ws_repeat_register_and_apply(ctx->modelId, ctx->hsf, stb->data, stb->count, 2, NULL, &ctx->stCenter, k);
            }
        }
    }
    /* A mesh reached through a rotated chain is left untouched above (the
     * `!newHasRot` guard) -- a safe no-op for that one sub-object, not a
     * guess -- see this function's own header comment. */
    if (object->type != HSF_OBJ_REPLICA) {
        for (i = 0; i < object->mesh.childNum; i++) {
            mp6_ws_repeat_walk_r(object->mesh.child[i], newScale, newTrans, newHasRot, ctx, k);
        }
    }
}

void mp6_widescreen_extrude_model_repeat(HU3D_MODELID modelId)
{
    HSF_DATA *hsf;
    HuVecF center;
    HuVec2f stmin, stmax, stcenter;
    s16 i;
    s32 j;
    BOOL anySt = FALSE;
    MP6WSRepeatCtx ctx;
    HuVecF rootScale, rootTrans;
    float k;

    if (!mp6_widescreen_enabled() || modelId < 0) {
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        return;
    }
    if (!mp6_ws_model_center(hsf, &center)) {
        return;
    }

    /* UV bbox union across every HSF_OBJ_MESH sub-object's own ST buffer
     * -- UV space has no hierarchy/base-transform concept of its own
     * (texture coordinates are flat 2D data, not scene-graph nodes), so
     * this stays a plain flat union, unlike the position bbox above. */
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *o = &hsf->object[i];
        HSF_BUFFER *st;
        HuVec2f *stData;
        if (o->type != HSF_OBJ_MESH) {
            continue;
        }
        st = o->mesh.st;
        if (st == NULL || st->data == NULL) {
            continue;
        }
        stData = (HuVec2f *)st->data;
        for (j = 0; j < st->count; j++) {
            if (!anySt) {
                stmin = stmax = stData[j];
                anySt = TRUE;
            } else {
                if (stData[j].x < stmin.x) stmin.x = stData[j].x;
                if (stData[j].y < stmin.y) stmin.y = stData[j].y;
                if (stData[j].x > stmax.x) stmax.x = stData[j].x;
                if (stData[j].y > stmax.y) stmax.y = stData[j].y;
            }
        }
    }
    if (anySt) {
        stcenter.x = (stmin.x + stmax.x) * 0.5f;
        stcenter.y = (stmin.y + stmax.y) * 0.5f;
    }

    ctx.modelCenter = center;
    ctx.stCenter = stcenter;
    ctx.anySt = anySt;
    ctx.modelId = modelId;
    ctx.hsf = hsf;

    rootScale.x = rootScale.y = rootScale.z = 1.0f;
    rootTrans.x = rootTrans.y = rootTrans.z = 0.0f;
    k = mp6_widescreen_scale_factor();
    mp6_ws_repeat_walk_r(hsf->root, rootScale, rootTrans, FALSE, &ctx, k);

    /* Matching this engine's own established "mutate live vertex buffer,
     * then invalidate" pattern -- game/EnvelopeExec.c's per-frame skinning
     * write (SetEnvelop) is followed by exactly this same call
     * (game/hsfman.c's per-frame EnvelopeProc dispatch). Cheap and always
     * correct to include here too. mp6_widescreen_reapply()'s own
     * per-frame REPEAT pass (below) calls this again after every
     * registered buffer is re-derived. */
    GXInvalidateVtxCache();
}

/* ==========================================================================
 * Selective per-sub-object horizontal CLAMP extrude, rotation-aware -- for
 * a model that MIXES foreground content (that must stay native) with
 * background content (that should widen) in ONE HSF model, at the
 * sub-object level.
 * ==========================================================================
 * Confirmed needed for real (not speculative) by bootDll/boot.c's title
 * screen: TitleMdlId[2] ("STAR")'s 65 sub-objects are ~33 real HSF_OBJ_MESH
 * quads -- 18 character-portrait billboards ("chara01..18"), the copyright
 * line ("copy"), an internal "6"-numeral duplicate ("rogo"), and ~12
 * "grid*"/"hikari" color-split/confetti decoration quads -- ALL living in
 * ONE model (HSF diagnostic dump, this session). Every mechanism above
 * operates on a whole MODEL (top-level Hu3DModelPosSet/ScaleSet) -- there is
 * no way to widen only SOME of a model's own sub-objects with those. This
 * mechanism instead mutates the chosen sub-objects' own raw LOCAL vertex
 * buffer (never the model's top-level transform, never any non-chosen
 * sub-object's data), selected by a caller-supplied name predicate (e.g.
 * "starts with grid, or is hikari").
 *
 * WHY VERTEX-BUFFER MUTATION, NOT THE OBJECT'S OWN base.pos/scale FIELDS:
 * `game/hsfdraw.c`'s `PGObjCalc` (the same reference transform-composer
 * `mp6_ws_accum_bbox_r` above mirrors) reads `object->mesh.curr` instead of
 * `object->mesh.base` whenever the OWNING MODEL has an active motion
 * (`attachMotionF`, set from `modelP->motId != HU3D_MOTIONID_NONE`) --
 * TitleMdlId[2] has exactly this (its whole reveal sequence is a baked
 * motion), so EVERY sub-object in it, matched or not, is actually
 * transformed from `curr` at draw time, which the motion system overwrites
 * every frame from its own keyframe data. Writing to `base.pos/scale` here
 * would therefore be silently ineffective (confirmed by direct source
 * reading, not assumed). The raw vertex buffer is a different, ORTHOGONAL
 * layer: `curr`/`base` describe how the whole quad (rigid body) is placed,
 * while the vertex buffer describes the quad's OWN shape in its own local
 * frame -- mutating the shape is invisible to and unaffected by whichever
 * transform field the motion system is driving, so it works identically
 * whether or not a motion is attached. This is the exact same "touch the
 * mesh data, not the transform" strategy the REPEAT mechanism above already
 * uses, for an analogous reason (there, avoiding double-application on
 * REPEAT wrap; here, sidestepping the base/curr ambiguity entirely).
 *
 * ROTATION-AWARE, VIA THE ENGINE'S OWN MATRIX MATH (not hand-derived trig):
 * several of the title's own "grid*" sub-objects carry a real, non-zero
 * `base.rot.z` (confirmed by HSF diagnostic dump -- e.g. "grid10"
 * baseRot.z=-206.92, part of the diagonal color-split/confetti look), so
 * that object's own LOCAL +X axis is NOT the screen-horizontal axis --
 * naively scaling local vertex.x for such an object would stretch it along
 * whatever diagonal its own rotation points, not horizontally on screen.
 * Fixed by building this object's own real forward matrix EXACTLY the way
 * `mp6_ws_accum_bbox_r` above already does (`mtxRot` + `PSMTXScale` +
 * `PSMTXConcat` + `mtxTransCat`, object-local-to-model-space), transforming
 * each native vertex through it to reach MODEL space, applying the "grow X
 * about the shared center, leave Y/Z alone" formula THERE (where world/model
 * X genuinely is screen-horizontal, per this file's own established
 * unrotated-top-level-transform precondition), then mapping back to local
 * space via `PSMTXInverse` of that SAME matrix. This is exact for any
 * combination of base pos/rot/scale (not an approximation, and not a second,
 * independently-hand-derived formula that could silently disagree with the
 * bbox walker's own rotation convention) -- it degenerates to a plain
 * "scale local x about center" for the (majority of) objects whose own
 * `base.rot` is zero, with no special-case branch needed.
 *
 * PRECONDITION (documented, not generically handled): each selected object
 * is assumed a DIRECT child of `hsf->root` (parent's own transform is
 * identity) -- true for every "grid*"/"hikari" object this call site needs
 * (flat billboard quads, no skeletal nesting), matching this file's own
 * established pattern of flagging an unhandled generalization rather than
 * silently mis-transforming a future, differently-shaped caller.
 *
 * The shared `center` is `mp6_ws_model_center()`'s own hierarchical bbox
 * center (the SAME derived value every other mechanism in this file already
 * uses) -- not a hand-picked constant -- so this reuses the one proven,
 * already-verified center computation instead of adding a second one scoped
 * to just the selected subset.
 *
 * Dynamic registry: identical "malloc a native snapshot once per live
 * vertex-buffer pointer, re-derive from it every frame with the CURRENT k"
 * strategy as the REPEAT mechanism, keyed by buffer pointer + owning
 * modelId/hsf for the same liveness reason. Also updates the sub-object's
 * OWN authored `mesh.min`/`max` (re-derived fresh from the transformed
 * vertex extents each apply, since a rotation mixes axes -- unlike the
 * REPEAT mechanism's axis-aligned scale-about-center, min/max cannot just be
 * scaled directly here) so a cull test against it stays correct. */
typedef int (*MP6WSNameMatchFn)(const char *name);

typedef struct {
    void *ptr;              /* live target: the sub-object's vertex buffer data */
    float *native;          /* malloc'd once: pristine native (x,y,z) snapshot */
    s32 count;
    HuVecF center;           /* shared MODEL-local stretch center (mp6_ws_model_center()'s own hierCenter) */
    Mtx fwdMtx;              /* this object's own native Trans(base.pos)*Rot(base.rot)*Scale(base.scale) */
    Mtx invMtx;              /* cached PSMTXInverse(fwdMtx) */
    HuVecF *liveMin;         /* &object->mesh.mesh.min -- kept in step for culling */
    HuVecF *liveMax;         /* &object->mesh.mesh.max */
    HU3D_MODELID modelId;
    HSF_DATA *hsf;
} MP6WSSelectiveEntry;

/* Title's own "grid*"/"hikari" set is 12 vertex buffers this session (see
 * the .patch call site's own per-object census) -- 16 gives headroom for
 * re-entering the title screen many times across a session (dead slots
 * recycled exactly like every other registry in this file). */
#define MP6_WS_SELECTIVE_MAX 16
static MP6WSSelectiveEntry g_mp6WsSelective[MP6_WS_SELECTIVE_MAX];
static s16 g_mp6WsSelectiveNum = 0;

static MP6WSSelectiveEntry *mp6_ws_selective_find(void *ptr)
{
    s16 i;
    for (i = 0; i < g_mp6WsSelectiveNum; i++) {
        if (g_mp6WsSelective[i].ptr == ptr && Hu3DData[g_mp6WsSelective[i].modelId].hsf == g_mp6WsSelective[i].hsf) {
            return &g_mp6WsSelective[i];
        }
    }
    return NULL;
}

static MP6WSSelectiveEntry *mp6_ws_selective_alloc(void)
{
    s16 i;
    for (i = 0; i < g_mp6WsSelectiveNum; i++) {
        if (Hu3DData[g_mp6WsSelective[i].modelId].hsf != g_mp6WsSelective[i].hsf) {
            return &g_mp6WsSelective[i]; /* recycle a dead slot */
        }
    }
    if (g_mp6WsSelectiveNum < MP6_WS_SELECTIVE_MAX) {
        return &g_mp6WsSelective[g_mp6WsSelectiveNum++];
    }
    return NULL;
}

static void mp6_ws_selective_apply(MP6WSSelectiveEntry *e, float k)
{
    /* A savestate restore leaves this registry's GAME half valid
     * (ptr/hsf/modelId are arena addresses and ids, all restored) but
     * nulls its HOST half -- `native` pointed into the capturing
     * process's C runtime heap, which does not exist in the restoring
     * process. mp6_widescreen_savestate_rehydrate() sets it NULL; this
     * early-out keeps the per-frame re-apply from dereferencing it before
     * the registrar re-snapshots. Skipping a frame's re-apply is
     * harmless: the geometry in the arena is already extruded (that is
     * what was captured), and the next register call re-takes the
     * snapshot. */
    if (e->native == NULL || e->count <= 0) {
        return;
    }
    HuVecF *dst = (HuVecF *)e->ptr;
    HuVecF *src = (HuVecF *)e->native;
    HuVecF newMin, newMax;
    BOOL any = FALSE;
    s32 j;

    for (j = 0; j < e->count; j++) {
        HuVecF world, grown, back;
        PSMTXMultVec(e->fwdMtx, &src[j], &world);
        grown.x = e->center.x + k * (world.x - e->center.x);
        grown.y = world.y; /* vertical framing byte-identical to native */
        grown.z = world.z; /* depth byte-identical to native */
        PSMTXMultVec(e->invMtx, &grown, &back);
        dst[j] = back;
        if (!any) {
            newMin = newMax = back;
            any = TRUE;
        } else {
            if (back.x < newMin.x) newMin.x = back.x;
            if (back.y < newMin.y) newMin.y = back.y;
            if (back.z < newMin.z) newMin.z = back.z;
            if (back.x > newMax.x) newMax.x = back.x;
            if (back.y > newMax.y) newMax.y = back.y;
            if (back.z > newMax.z) newMax.z = back.z;
        }
    }
    if (any) {
        *e->liveMin = newMin;
        *e->liveMax = newMax;
    }
}

void mp6_widescreen_extrude_model_selective_horizontal(int16_t modelId, MP6WSNameMatchFn match)
{
    HSF_DATA *hsf;
    HuVecF center;
    s16 i;
    float k;

    if (!mp6_widescreen_enabled() || modelId < 0 || match == NULL) {
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        return;
    }
    if (!mp6_ws_model_center(hsf, &center)) {
        return;
    }
    k = mp6_widescreen_scale_factor();
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *o = &hsf->object[i];
        HSF_TRANSFORM *t;
        MP6WSSelectiveEntry *e;
        BOOL freshInstance;
        Mtx rotMtx, scaleMtx, mtx;

        if (o->type != HSF_OBJ_MESH || o->name == NULL || !match(o->name)) {
            continue;
        }
        if (o->mesh.vertex == NULL || o->mesh.vertex->data == NULL || o->mesh.vertex->count <= 0) {
            continue;
        }
        e = mp6_ws_selective_find(o->mesh.vertex->data);
        freshInstance = (e == NULL) || (e->hsf != hsf);
        if (e == NULL) {
            e = mp6_ws_selective_alloc();
        }
        if (e == NULL) {
            continue; /* registry exhausted -- degrade to "not tracked", same graceful path every other registry here uses */
        }
        t = &o->mesh.base;
        mtxRot(rotMtx, t->rot.x, t->rot.y, t->rot.z);
        PSMTXScale(scaleMtx, t->scale.x, t->scale.y, t->scale.z);
        PSMTXConcat(rotMtx, scaleMtx, mtx);
        mtxTransCat(mtx, t->pos.x, t->pos.y, t->pos.z);
        PSMTXCopy(mtx, e->fwdMtx);
        PSMTXInverse(e->fwdMtx, e->invMtx);
        e->center = center;
        e->modelId = modelId;
        e->hsf = hsf;
        e->liveMin = &o->mesh.mesh.min;
        e->liveMax = &o->mesh.mesh.max;
        if (freshInstance) {
            size_t bytes = (size_t)o->mesh.vertex->count * sizeof(HuVecF);
            float *native = (float *)malloc(bytes);
            if (native == NULL) {
                continue; /* OOM on a tiny alloc -- extremely unlikely; degrade to "not tracked" */
            }
            free(e->native);
            e->native = native;
            memcpy(e->native, o->mesh.vertex->data, bytes);
            e->ptr = o->mesh.vertex->data;
            e->count = o->mesh.vertex->count;
        }
        mp6_ws_selective_apply(e, k);
    }
    GXInvalidateVtxCache();
}

/* ==========================================================================
 * Selective per-sub-object horizontal REPOSITION (never resize, never
 * re-root) -- for decorations that must
 * track a stretching sibling plane's own widening WITHOUT being distorted
 * OR losing their own independent per-object animation
 * ==========================================================================
 * Confirmed needed for real (not speculative) by mdseldll/mdsel.c's
 * mode-select sky (mdlId[2]): the first cut of this fix (matching ONLY
 * "sora", leaving every "cloud*"/"star*" object completely untouched) left
 * clouds/stars at a FIXED native position while "sora"'s own checkered
 * day/night boundary widens out from the shared center -- some clouds ended
 * up over the (now-wider) night half and some stars over the day half. A
 * SECOND on-device report caught a regression in this lane's own first
 * attempt at fixing THAT: an earlier revision of this function mutated each
 * matched object's own raw VERTEX BUFFER (the same technique
 * mp6_widescreen_extrude_model_selective_horizontal() above correctly uses
 * for "sora" itself) -- but unlike "sora" (one single backdrop plane with
 * no per-instance identity), each cloud/star natively animates
 * INDEPENDENTLY on its own root/joint (its own drift/float motion track);
 * overwriting every vertex from a cached native snapshot every frame
 * fights/freezes whatever that object's own per-root animation was doing to
 * its rendered result, and reads on-device as "they all moved together
 * as one group" -- a real, confirmed regression, not a false alarm.
 *
 * Root-caused via direct source reading of game/hsfmotion.c's
 * `Hu3DMotionExec()` (the actual per-frame keyframe evaluator), not guessed:
 * every object's `mesh.curr` is reset from `mesh.base` FIRST, unconditionally,
 * for every UNANIMATED channel (`objP->mesh.curr.pos.x = objP->mesh.base.
 * pos.x;` when no force-override is set) -- then ONLY the channels that
 * genuinely have their own keyframe track get overwritten with an absolute
 * interpolated value, via `GetObjTRXPtr()`, which itself resolves EXCLUSIVELY
 * to `&objPtr->mesh.curr.*` fields -- `mesh.base` is never written by
 * animation playback at all, confirmed by grepping every `GetObjTRXPtr`
 * case. This means each object's own "rest" position (`base.pos`) is
 * exactly the value that flows into `curr.pos` for any channel that object's
 * own track data doesn't drive -- i.e. a plain, permanent write to
 * `mesh.base.pos.x` is the semantically CORRECT "move its resting position"
 * operation: any Y-bob/rotation drift the object's own per-root motion track
 * independently animates every frame is completely undisturbed (this
 * function never touches `curr`, never touches vertex data, never touches
 * any OTHER object's own track/root), while its X position permanently
 * shifts to track "sora"'s own widening boundary. No matrix math is needed
 * at all here (unlike the vertex-mutation mechanisms above) -- `base.pos.x`
 * is already expressed directly in this object's OWN PARENT-relative frame,
 * the same frame the shared model center is computed in, so the "scale
 * outward from center by k" relationship applies to it directly:
 * `newBaseX = centerX + k*(nativeBaseX - centerX)`, cached natively once
 * (`e->nativeBaseX`, `e->centerX`) and re-derived every frame from the
 * live k, same dynamic-registry pattern as every other mechanism here.
 *
 * Precondition (documented, not generically handled): the matched object's
 * own X position is not ALSO independently keyframed by its own motion data
 * (true for every real call site so far -- mode-select's own cloud/star
 * drift is a Y-bob/rotation animation, confirmed by this fix's own on-device
 * verification showing each one still drifting independently after this
 * change) -- an object whose own track DOES drive HSF_CHANNEL_POSX would
 * have its curr.pos.x overwritten by that track's own interpolated value
 * every frame regardless of what this function sets base.pos.x to, exactly
 * mirroring the same "already using this channel for something else" caveat
 * every base/curr-touching mechanism in this project has to respect. */
typedef struct {
    HSF_OBJECT *obj;         /* direct pointer into hsf->object[] -- stable for the model instance's lifetime */
    float nativeBaseX;        /* cached native base.pos.x, read once at first registration */
    float centerX;             /* cached native shared model center x (mp6_ws_model_center()'s own x), read once */
    float bias;                /* extra multiplier on the (k-1) term below, see mp6_ws_reposition_apply();
                                 * always refreshed (not just on freshInstance), same as modelId/hsf above -- lets
                                 * two different callers (matching disjoint name sets) push their own matched
                                 * objects by different amounts through the identical mechanism/registry. */
    float clampMinX;           /* hard lower bound on the repositioned base.pos.x, applied AFTER the
                                 * formula in mp6_ws_reposition_apply(). MP6_WS_NO_CLAMP disables it, which is
                                 * what an unclamped caller passes -- see mp6_ws_reposition_apply(). */
    HU3D_MODELID modelId;
    HSF_DATA *hsf;
} MP6WSRepositionEntry;

/* Sentinel "no clamp": far enough below any real world x that the clamp
 * branch can never fire, so a caller that does not opt in behaves exactly as
 * it did before this clamp existed. */
#define MP6_WS_NO_CLAMP (-1.0e30f)

/* Mode-select's own "cloud1".."cloud6"/"star1".."star7" is 13 objects -- 16
 * gives the same headroom-for-re-entry margin as MP6_WS_SELECTIVE_MAX above,
 * matching this file's own established sizing rationale. */
#define MP6_WS_REPOSITION_MAX 16
static MP6WSRepositionEntry g_mp6WsReposition[MP6_WS_REPOSITION_MAX];
static s16 g_mp6WsRepositionNum = 0;

static MP6WSRepositionEntry *mp6_ws_reposition_find(HSF_OBJECT *obj)
{
    s16 i;
    for (i = 0; i < g_mp6WsRepositionNum; i++) {
        if (g_mp6WsReposition[i].obj == obj && Hu3DData[g_mp6WsReposition[i].modelId].hsf == g_mp6WsReposition[i].hsf) {
            return &g_mp6WsReposition[i];
        }
    }
    return NULL;
}

static MP6WSRepositionEntry *mp6_ws_reposition_alloc(void)
{
    s16 i;
    for (i = 0; i < g_mp6WsRepositionNum; i++) {
        if (Hu3DData[g_mp6WsReposition[i].modelId].hsf != g_mp6WsReposition[i].hsf) {
            return &g_mp6WsReposition[i]; /* recycle a dead slot */
        }
    }
    if (g_mp6WsRepositionNum < MP6_WS_REPOSITION_MAX) {
        return &g_mp6WsReposition[g_mp6WsRepositionNum++];
    }
    return NULL;
}

static void mp6_ws_reposition_apply(MP6WSRepositionEntry *e, float k)
{
    /* A plain, permanent write to this object's own REST position -- never
     * curr, never vertex data, never any other object's own root. Safe to
     * call every frame with the same k (idempotent): recomputed fresh from
     * the cached NATIVE baseline every time, never compounding.
     *
     * `e->bias` generalizes the original `centerX + k*(nativeBaseX-
     * centerX)` to `nativeBaseX + bias*(k-1)*(nativeBaseX-centerX)` --
     * algebraically identical to the original at bias=1.0f (expand:
     * nativeBaseX + (k-1)*(nativeBaseX-centerX) = centerX + k*(nativeBaseX-
     * centerX)), and still exactly native at k=1 for ANY bias value (the
     * (k-1) factor is zero), so a caller that never opts in (bias=1.0f,
     * e.g. mode-select's own stars) is byte-for-byte unchanged from before
     * this existed. */
    e->obj->mesh.base.pos.x = e->nativeBaseX + e->bias * (k - 1.0f) * (e->nativeBaseX - e->centerX);

    /* The formula above scales an object's own offset FROM the divide, so
     * an object that starts NEAR the divide has a (nativeBaseX - centerX)
     * term near zero and therefore barely moves no matter how large `bias`
     * gets -- no bias value can pull mode-select's own divide-straddling
     * clouds fully off the night half ("I don't want any cloud on the
     * night side"). A hard day-side floor does. Gated on k>1 so the
     * native aspect keeps its exactly-native rest positions (byte-
     * identical), matching the same "zero out at native k" contract every
     * mechanism in this file follows; MP6_WS_NO_CLAMP keeps every
     * non-opting caller (e.g. the stars) on the unclamped path. */
    if (k > 1.0f && e->obj->mesh.base.pos.x < e->clampMinX) {
        e->obj->mesh.base.pos.x = e->clampMinX;
    }
}

/* Exposes `mp6_ws_model_center()`'s own x component so a caller that needs
 * to invoke mp6_widescreen_extrude_model_selective_reposition_x() MORE
 * THAN ONCE for the SAME model (e.g. mode-select's sky: once for stars,
 * once for clouds, with different biases) can snapshot the center ONE
 * time and pass the IDENTICAL value to every call, rather than each call
 * recomputing it fresh and getting a DIFFERENT answer depending on what
 * the PREVIOUS call already mutated -- see
 * mp6_widescreen_extrude_model_selective_reposition_x()'s own updated
 * comment for why that matters (it is not just a documentation nit:
 * `mp6_ws_accum_bbox_r()`'s transform composition reads each object's own
 * CURRENT `mesh.base.pos`, so an earlier reposition call's write is visible
 * to a later call's own center recomputation, silently shifting the
 * reference point the second call's `(nativeBaseX-centerX)` term is measured
 * from). Returns 0.0f (a safe, inert fallback -- byte-identical-when-
 * disabled is preserved since callers only ever use this value when
 * widescreen is enabled to begin with) if widescreen is disabled or the
 * model/hsf isn't valid. */
float mp6_widescreen_model_center_x(int16_t modelId)
{
    HSF_DATA *hsf;
    HuVecF center;

    if (!mp6_widescreen_enabled() || modelId < 0) {
        return 0.0f;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL || !mp6_ws_model_center(hsf, &center)) {
        return 0.0f;
    }
    return center.x;
}

static void mp6_ws_reposition_x_impl(int16_t modelId, MP6WSNameMatchFn match, float bias, float centerX, float clampMinX)
{
    HSF_DATA *hsf;
    s16 i;
    float k;

    if (!mp6_widescreen_enabled() || modelId < 0 || match == NULL) {
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        return;
    }
    k = mp6_widescreen_scale_factor();
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *o = &hsf->object[i];
        MP6WSRepositionEntry *e;
        BOOL freshInstance;

        if (o->type != HSF_OBJ_MESH || o->name == NULL || !match(o->name)) {
            continue;
        }
        e = mp6_ws_reposition_find(o);
        freshInstance = (e == NULL) || (e->hsf != hsf);
        if (e == NULL) {
            e = mp6_ws_reposition_alloc();
        }
        if (e == NULL) {
            continue; /* registry exhausted -- degrade to "not tracked", same graceful path every other registry here uses */
        }
        e->modelId = modelId;
        e->hsf = hsf;
        e->obj = o;
        e->bias = bias; /* always refreshed (not just on freshInstance), same as modelId/hsf above */
        e->clampMinX = clampMinX; /* same "always refreshed" rule as bias above */
        if (freshInstance) {
            /* Native baseline, read once: this object's own REST x position,
             * genuinely native -- matching every other mechanism's own
             * "cache the native baseline once, at first registration" rule.
             * `centerX` itself is the CALLER's own responsibility now
             * -- see mp6_widescreen_model_center_x()'s own comment for why a
             * caller invoking this function more than once for the same
             * model must snapshot ONE center and pass it to every call. */
            e->nativeBaseX = o->mesh.base.pos.x;
            e->centerX = centerX;
        }
        mp6_ws_reposition_apply(e, k);
    }
}

void mp6_widescreen_extrude_model_selective_reposition_x(int16_t modelId, MP6WSNameMatchFn match, float bias, float centerX)
{
    mp6_ws_reposition_x_impl(modelId, match, bias, centerX, MP6_WS_NO_CLAMP);
}

/* Identical mechanism, plus a hard day-side floor on the resulting
 * base.pos.x -- see mp6_ws_reposition_apply()'s own clamp comment for why a
 * clamp (not a larger bias) is what moves a divide-straddling object. */
void mp6_widescreen_extrude_model_reposition_x_clamped(int16_t modelId, MP6WSNameMatchFn match, float bias, float centerX, float clampMinX)
{
    mp6_ws_reposition_x_impl(modelId, match, bias, centerX, clampMinX);
}

/* ==========================================================================
 * Border-only extend for a genuinely CLAMP, subdivided-grid backdrop --
 * interior tiles/decal stay native
 * ==========================================================================
 * Confirmed needed for real (not speculative) by mdpartydll/mdparty.c's
 * Party-floor model ("yuka", DATA_mdparty+1): the plain
 * `mp6_widescreen_extrude_model_xz()` (a whole-piece scale-about-center,
 * exactly like every other CLAMP mechanism in this file) visibly enlarges
 * the floor's own BAKED diamond-tile texture and its center play-circle
 * decal (both painted into ONE authored image, `ys77_floor` -- confirmed
 * via HSF diagnostic dump: wrapS=wrapT=0, genuinely CLAMP, not REPEAT) --
 * an "accepted softening"
 * tradeoff for a one-off painted backdrop image (Party's own pillar, the
 * title collage, mode-select's sky), but WRONG here: the tile grid and the
 * play-circle are both meant to read at a CONSISTENT, native world scale
 * (the circle marks real game-logic positions), not grow with the window.
 *
 * The raw-vertex HSF diagnostic dump (this session) found "yuka" is a
 * regular 5x5 vertex grid (25 vertices, 16 quad faces) spanning its own
 * authored min=(-800,0,-800)/max=(800,0,800) evenly, with a matching regular
 * UV unwrap (each of the 16 quad cells maps to its own distinct slice of the
 * SAME single 0..1 texture square -- confirming ys77_floor is one whole
 * baked image, not a small repeating tile). This shape makes a "9-slice"
 * border-extend directly possible: classify each of the object's own native
 * vertices by whether its native x/z already sits AT this object's own
 * authored min/max on that axis (the OUTER 16 perimeter vertices) versus
 * strictly inside it (the INNER 9) -- independently per axis, so an
 * edge-midpoint vertex (e.g. native (0,-800), on the min-Z boundary but at
 * X=0, nowhere near the X extremes) grows only in Z, not X, sliding straight
 * outward instead of diagonally. Only the OUTER vertices' x/z move (scaled
 * about this object's own native bbox center by scale_factor(), the exact
 * same "grow about center" arithmetic used everywhere else in this file);
 * every INNER vertex's position, and EVERY vertex's own UV/ST data, is never
 * touched at all -- so the inner 2x2 block of quad cells (where the
 * circle-decal portion of the baked texture lands) stays byte-identical to
 * native, while only the outer ring of border cells stretches to close the
 * gap, the same "trapezoid instead of square" border-image technique CSS's
 * own border-image/9-slice scaling uses. Y is never touched on any vertex
 * (this floor's own Y-extent is ~0 -- same non-concern already noted for
 * the isotropic-vs-XZ choice on this exact model).
 *
 * Unlike the selective mechanism above, no rotation-awareness is needed
 * here: "yuka" itself is confirmed unrotated at its own base transform
 * (base.rot=(0,0,0), HSF diagnostic dump) and sits at base.pos=(0,0,0), so
 * object-local space and model space already coincide -- flagged as an
 * explicit precondition (unhandled generalization, not silently
 * mis-transformed) for a future rotated caller.
 *
 * Dynamic registry: same "malloc a native snapshot once, re-derive
 * from it every frame with the CURRENT k" strategy as every other buffer
 * mechanism in this file. The per-vertex boundary/interior classification
 * is re-derived fresh every apply from the CACHED NATIVE min/max (a plain
 * float comparison, not a re-walk) rather than cached as a separate mask --
 * equally cheap, one fewer array to maintain. Also keeps the object's own
 * authored `mesh.min`/`max` in step (X/Z scaled about the same native
 * center; Y untouched) for culling, same rationale as the REPEAT mechanism. */
typedef struct {
    void *ptr;
    float *native;         /* malloc'd once: pristine native (x,y,z) snapshot */
    s32 count;
    HuVecF nativeMin;        /* this object's own native mesh.min, cached once */
    HuVecF nativeMax;        /* this object's own native mesh.max, cached once */
    HuVecF localCenter;      /* (nativeMin+nativeMax)*0.5 -- this object's own pivot */
    HuVecF *liveMin;
    HuVecF *liveMax;
    HU3D_MODELID modelId;
    HSF_DATA *hsf;
} MP6WSBorderEntry;

/* This project has exactly one confirmed border-extend user ("yuka") --
 * 4 gives headroom for re-entering Party Mode many times in a session
 * without ever needing to recycle a slot still actually live. */
#define MP6_WS_BORDER_MAX 4
static MP6WSBorderEntry g_mp6WsBorder[MP6_WS_BORDER_MAX];
static s16 g_mp6WsBorderNum = 0;

static MP6WSBorderEntry *mp6_ws_border_find(void *ptr)
{
    s16 i;
    for (i = 0; i < g_mp6WsBorderNum; i++) {
        if (g_mp6WsBorder[i].ptr == ptr && Hu3DData[g_mp6WsBorder[i].modelId].hsf == g_mp6WsBorder[i].hsf) {
            return &g_mp6WsBorder[i];
        }
    }
    return NULL;
}

static MP6WSBorderEntry *mp6_ws_border_alloc(void)
{
    s16 i;
    for (i = 0; i < g_mp6WsBorderNum; i++) {
        if (Hu3DData[g_mp6WsBorder[i].modelId].hsf != g_mp6WsBorder[i].hsf) {
            return &g_mp6WsBorder[i]; /* recycle a dead slot */
        }
    }
    if (g_mp6WsBorderNum < MP6_WS_BORDER_MAX) {
        return &g_mp6WsBorder[g_mp6WsBorderNum++];
    }
    return NULL;
}

static void mp6_ws_border_apply(MP6WSBorderEntry *e, float k)
{
    /* A savestate restore leaves this registry's GAME half valid
     * (ptr/hsf/modelId are arena addresses and ids, all restored) but
     * nulls its HOST half -- `native` pointed into the capturing
     * process's C runtime heap, which does not exist in the restoring
     * process. mp6_widescreen_savestate_rehydrate() sets it NULL; this
     * early-out keeps the per-frame re-apply from dereferencing it before
     * the registrar re-snapshots. Skipping a frame's re-apply is
     * harmless: the geometry in the arena is already extruded (that is
     * what was captured), and the next register call re-takes the
     * snapshot. */
    if (e->native == NULL || e->count <= 0) {
        return;
    }
    HuVecF *dst = (HuVecF *)e->ptr;
    HuVecF *src = (HuVecF *)e->native;
    const float eps = 0.01f;
    s32 j;

    for (j = 0; j < e->count; j++) {
        BOOL atMinX = (src[j].x - e->nativeMin.x < eps) && (src[j].x - e->nativeMin.x > -eps);
        BOOL atMaxX = (src[j].x - e->nativeMax.x < eps) && (src[j].x - e->nativeMax.x > -eps);
        BOOL atMinZ = (src[j].z - e->nativeMin.z < eps) && (src[j].z - e->nativeMin.z > -eps);
        BOOL atMaxZ = (src[j].z - e->nativeMax.z < eps) && (src[j].z - e->nativeMax.z > -eps);
        dst[j].x = (atMinX || atMaxX) ? (e->localCenter.x + k * (src[j].x - e->localCenter.x)) : src[j].x;
        dst[j].y = src[j].y; /* never touched -- this floor's own Y-extent is ~0 */
        dst[j].z = (atMinZ || atMaxZ) ? (e->localCenter.z + k * (src[j].z - e->localCenter.z)) : src[j].z;
    }
    e->liveMin->x = e->localCenter.x + k * (e->nativeMin.x - e->localCenter.x);
    e->liveMin->z = e->localCenter.z + k * (e->nativeMin.z - e->localCenter.z);
    e->liveMax->x = e->localCenter.x + k * (e->nativeMax.x - e->localCenter.x);
    e->liveMax->z = e->localCenter.z + k * (e->nativeMax.z - e->localCenter.z);
}

void mp6_widescreen_extrude_model_border_xz(int16_t modelId, const char *objName)
{
    HSF_DATA *hsf;
    s16 i;
    float k;

    if (!mp6_widescreen_enabled() || modelId < 0 || objName == NULL) {
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        return;
    }
    k = mp6_widescreen_scale_factor();
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *o = &hsf->object[i];
        MP6WSBorderEntry *e;
        BOOL freshInstance;

        if (o->type != HSF_OBJ_MESH || o->name == NULL || strcmp(o->name, objName) != 0) {
            continue;
        }
        if (o->mesh.vertex == NULL || o->mesh.vertex->data == NULL || o->mesh.vertex->count <= 0) {
            return;
        }
        e = mp6_ws_border_find(o->mesh.vertex->data);
        freshInstance = (e == NULL) || (e->hsf != hsf);
        if (e == NULL) {
            e = mp6_ws_border_alloc();
        }
        if (e == NULL) {
            return; /* registry exhausted -- degrade to "not tracked" */
        }
        e->modelId = modelId;
        e->hsf = hsf;
        e->liveMin = &o->mesh.mesh.min;
        e->liveMax = &o->mesh.mesh.max;
        if (freshInstance) {
            size_t bytes = (size_t)o->mesh.vertex->count * sizeof(HuVecF);
            float *native = (float *)malloc(bytes);
            if (native == NULL) {
                return; /* OOM on a tiny alloc -- extremely unlikely; degrade to "not tracked" */
            }
            free(e->native);
            e->native = native;
            memcpy(e->native, o->mesh.vertex->data, bytes);
            e->ptr = o->mesh.vertex->data;
            e->count = o->mesh.vertex->count;
            e->nativeMin = o->mesh.mesh.min;
            e->nativeMax = o->mesh.mesh.max;
            e->localCenter.x = (e->nativeMin.x + e->nativeMax.x) * 0.5f;
            e->localCenter.y = (e->nativeMin.y + e->nativeMax.y) * 0.5f;
            e->localCenter.z = (e->nativeMin.z + e->nativeMax.z) * 0.5f;
        }
        mp6_ws_border_apply(e, k);
        GXInvalidateVtxCache();
        return; /* objName matches at most one object per model in every real call site */
    }
    /* not found -- silently no-op, matching every other mechanism's graceful degradation */
}

/* ==========================================================================
 * Split-separate for a symmetric proscenium-frame backdrop -- NATIVE
 * scale, halves slide apart in X only
 * ==========================================================================
 * Confirmed needed for real (not speculative) by mdpartydll/mdparty.c's
 * Party pillar model ("pillar", DATA_mdparty+0): on-device report says the
 * previous whole-piece isotropic scale (mp6_widescreen_extrude_model(), the
 * same CLAMP mechanism every other one-off backdrop image in this project
 * uses) is wrong for this specific asset for TWO reasons: (1) native-scale
 * art (no softening/stretch of the cloud-border texture) is wanted, with
 * the two halves simply SEPARATING to reveal more scene in the middle,
 * rather than a single piece growing uniformly; (2) the isotropic formula's
 * own position compensation, `pos += (1-k)*scale*center`, moves the WHOLE
 * model by a real amount on every axis whenever its own bbox center isn't at
 * local origin -- confirmed by direct measurement: "pillar"'s own
 * local-space bbox is min=(-800,0,-800)/max=(800,1000,-700), i.e.
 * center=(0,500,-750), a long way from local origin -- at a live 1280x720
 * window (k=1.325) this measured as topPos going from native (0,0,0) to
 * (0,-162.5,243.75): a real 243.75-unit push TOWARD the camera (+Z) and
 * 162.5-unit push down (-Y), neither of which is a "widen" concern at all
 * -- exactly a reported "extrude moves it forward/back in Z, so the floor
 * and cloud border collide" symptom, root-caused via this same
 * measurement.
 *
 * Mechanism: for every HSF_OBJ_MESH sub-object of `modelId` (the Party
 * pillar has exactly one, "pillar", confirmed unrotated/at local origin/
 * identity-scale via direct diagnostic dump -- base.pos=base.rot=(0,0,0),
 * base.scale=(1,1,1), so object-local space and model space coincide
 * exactly, no matrix composition needed, unlike the selective mechanism
 * above which DOES need one for rotated sub-objects), classify each of its
 * own NATIVE vertices by whether its local x sits left or right of that
 * object's own native bbox CENTER x (strictly independent of Y/Z, which are
 * NEVER read, touched, or compensated here -- this is the actual fix for
 * the Z-collision: no Z term, no Y term, anywhere in this mechanism, so
 * there is nothing FOR them to shove). Every LEFT vertex (native x < center
 * x) is translated by -halfWidthDelta; every RIGHT vertex (native x >=
 * center x) by +halfWidthDelta, where halfWidthDelta = halfWidth *
 * (scale_factor()-1) and halfWidth = (nativeMax.x-nativeMin.x)*0.5 -- i.e.
 * a UNIFORM per-half shift (translate), not a per-vertex proportional scale
 * (so the art's own shape is never resampled/softened, staying byte-
 * identical in scale, just relocated). This is exactly the amount needed to
 * keep each half's own OUTER edge flush with the live frame edge: this
 * object's own native bbox already reaches exactly to the native 4:3 frame
 * edge at k=1 (an already-established precondition for this exact asset --
 * "the model being too small for widescreen" was accepted specifically
 * because its edges align with the native viewport edges, not floating
 * short of them) and the camera's own Hor+ widen
 * reveals exactly k times as much half-width at any given depth (the same
 * "world_extent scales with aspect, i.e. with k" relationship
 * mp6_widescreen_cover_fov()'s own header comment already derives) -- so
 * translating the native-authored left edge (at nativeMin.x) by
 * (nativeMin.x)*(k-1) = -halfWidth*(k-1) lands it exactly on the new,
 * k-times-wider edge, with NO change to the art's own local shape or scale.
 * A center GAP opens between the two separated halves by construction (the
 * whole point of "split apart" instead of "scale up") -- accepted per this
 * mission's own explicit direction, hidden behind whatever scene content/
 * logo sits centered in front of this backdrop at every real call site.
 *
 * Every vertex's own Y and Z are copied through completely unchanged (not
 * even re-written with an unchanged value's worth of math -- genuinely
 * skipped) -- the direct fix for the reported Z-collision: this mechanism
 * has no position-compensation term at all (unlike the isotropic/XZ/
 * selective mechanisms above, which all fold a `(1-k)*scale*center` offset
 * into the MODEL's own top-level Hu3DModelPosSet precisely because scaling
 * about an off-center bbox needs one) -- unnecessary here because nothing
 * scales; halves are purely translated in local X, so the model's own
 * top-level Hu3DModelPosSet/ScaleSet is NEVER called by this function at
 * all (stays at whatever native value the caller already set, exactly like
 * the selective/border mechanisms above never touch it either).
 *
 * Dynamic registry: identical "malloc a native snapshot once per live
 * vertex-buffer pointer, re-derive from it every frame with the CURRENT k"
 * strategy as every other buffer mechanism in this file. Also keeps each
 * touched object's own authored `mesh.min`/`max` in step (X only; Y/Z
 * copied from native unchanged) for culling.
 *
 * Precondition (documented, not generically handled, matching this file's
 * own established pattern): each touched sub-object's own base transform is
 * assumed unrotated and at local origin (true for "pillar", confirmed by
 * direct HSF diagnostic dump) -- a future rotated/offset caller would need
 * the same object-to-model matrix composition the selective mechanism above
 * already has, not handled here. */
typedef struct {
    void *ptr;
    float *native;          /* malloc'd once: pristine native (x,y,z) snapshot */
    s32 count;
    float localCenterX;     /* (nativeMin.x+nativeMax.x)*0.5 -- this object's own split line */
    float halfWidth;         /* (nativeMax.x-nativeMin.x)*0.5 -- native half-extent, the translate unit */
    float zBackRate;        /* Z units to shift AWAY from the camera per unit of (k-1); 0 = the
                              * plain behavior (mp6_widescreen_split_separate_x), byte-identical. */
    HuVecF nativeMin;
    HuVecF nativeMax;
    HuVecF *liveMin;
    HuVecF *liveMax;
    HU3D_MODELID modelId;
    HSF_DATA *hsf;
} MP6WSSplitEntry;

/* This project has exactly one confirmed split-separate user ("pillar") --
 * 4 gives headroom for re-entering Party Mode many times in a session
 * without ever needing to recycle a slot still actually live (matching
 * MP6WSBorderEntry's own identical sizing rationale). */
#define MP6_WS_SPLIT_MAX 4
static MP6WSSplitEntry g_mp6WsSplit[MP6_WS_SPLIT_MAX];
static s16 g_mp6WsSplitNum = 0;

static MP6WSSplitEntry *mp6_ws_split_find(void *ptr)
{
    s16 i;
    for (i = 0; i < g_mp6WsSplitNum; i++) {
        if (g_mp6WsSplit[i].ptr == ptr && Hu3DData[g_mp6WsSplit[i].modelId].hsf == g_mp6WsSplit[i].hsf) {
            return &g_mp6WsSplit[i];
        }
    }
    return NULL;
}

static MP6WSSplitEntry *mp6_ws_split_alloc(void)
{
    s16 i;
    for (i = 0; i < g_mp6WsSplitNum; i++) {
        if (Hu3DData[g_mp6WsSplit[i].modelId].hsf != g_mp6WsSplit[i].hsf) {
            return &g_mp6WsSplit[i]; /* recycle a dead slot */
        }
    }
    if (g_mp6WsSplitNum < MP6_WS_SPLIT_MAX) {
        return &g_mp6WsSplit[g_mp6WsSplitNum++];
    }
    return NULL;
}

/* The party camera's own source-confirmed pose (game/objsysobj.c's
 * omOutView(), fed by mdparty.c's fn_1_33A0: center=(0,65,-800),
 * rot=(-7.25,0,0), zoom=2650 for the character-select screen) puts the
 * camera at world (0, 399.4, 1828.8) with rotY=0 -- an UNYAWED camera, so
 * its own "right" screen axis is exactly world +X and its forward axis
 * has zero X component. Under a perspective projection this means
 * screen-space X is proportional to world_x / depthAlongForward, where
 * depthAlongForward(point) = (point-camPos).forward. Pushing this
 * object's own vertices back in Z by `zDelta` (more negative, away from the
 * camera) increases that denominator for every one of them, shrinking their
 * own projected X by the same ratio regardless of which half they're in --
 * so the X delta above needs multiplying by the INVERSE of that shrink to
 * keep landing exactly on the live frame edge, same as it does at zDelta=0.
 * `MP6_WS_PARTY_CAM_DEPTH_AT_PILLAR` is depthAlongForward evaluated once for
 * this object's own bbox-center (y=500, native z=-750) -- a single
 * representative point, matching this mechanism's own "one rigid delta
 * per half" translate-only philosophy (not a per-vertex varying scale,
 * which would distort the art this mechanism explicitly keeps
 * undistorted). */
#define MP6_WS_PARTY_CAM_COS_TILT        0.99201f /* cos(7.25 deg) */
#define MP6_WS_PARTY_CAM_DEPTH_AT_PILLAR 2545.5f  /* depthAlongForward at (y=500, z=-750) */

static void mp6_ws_split_apply(MP6WSSplitEntry *e, float k)
{
    /* A savestate restore leaves this registry's GAME half valid
     * (ptr/hsf/modelId are arena addresses and ids, all restored) but
     * nulls its HOST half -- `native` pointed into the capturing
     * process's C runtime heap, which does not exist in the restoring
     * process. mp6_widescreen_savestate_rehydrate() sets it NULL; this
     * early-out keeps the per-frame re-apply from dereferencing it before
     * the registrar re-snapshots. Skipping a frame's re-apply is
     * harmless: the geometry in the arena is already extruded (that is
     * what was captured), and the next register call re-takes the
     * snapshot. */
    if (e->native == NULL || e->count <= 0) {
        return;
    }
    HuVecF *dst = (HuVecF *)e->ptr;
    HuVecF *src = (HuVecF *)e->native;
    float delta = e->halfWidth * (k - 1.0f);
    float zDelta = e->zBackRate * (k - 1.0f);
    float ratio = 1.0f;
    float centerY = (e->nativeMin.y + e->nativeMax.y) * 0.5f;
    s32 j;

    if (zDelta != 0.0f) {
        ratio = 1.0f - (MP6_WS_PARTY_CAM_COS_TILT * zDelta) / MP6_WS_PARTY_CAM_DEPTH_AT_PILLAR;
        delta *= ratio;
    }

    /* The Z push shrinks this object's own PROJECTED SIZE by exactly
     * `1/ratio` -- not just the split translation the line above already
     * compensates. An earlier revision only scaled that translation, so
     * each half still landed on the live frame edge horizontally while the
     * art itself projected smaller: at a large Z-back, the frame's own top
     * edge visibly receded from the top corners and left clear-color
     * wedges there. Pre-multiplying every vertex about this object's own
     * native bbox center by the SAME `ratio` cancels that shrink, so the
     * pillar projects at its native on-screen size no matter how far back
     * it goes -- which is what "just move it further back" has to mean
     * visually: a depth change only, to close the parallax gap onto the
     * floor, with the art unchanged.
     *
     * This is a SCALE about the object's own center, NOT the Y translate
     * ("floor dip") rejected above: at zDelta==0 -- native aspect (k=1),
     * and every caller that never opts into a Z-back
     * (mp6_widescreen_split_separate_x) -- `ratio` is exactly 1.0f and
     * both axes below collapse algebraically to the plain (src.x +/-
     * delta, src.y) expressions, so those paths stay byte-identical. Like
     * the existing X compensation it evaluates the perspective ratio at
     * ONE representative depth (this object's own bbox center), matching
     * this mechanism's own established "one rigid delta per half"
     * approximation rather than introducing a per-vertex varying
     * projection. */
    for (j = 0; j < e->count; j++) {
        float sx = e->localCenterX + (src[j].x - e->localCenterX) * ratio;
        dst[j].x = (src[j].x < e->localCenterX) ? (sx - delta) : (sx + delta);
        dst[j].y = centerY + (src[j].y - centerY) * ratio;
        dst[j].z = src[j].z + zDelta; /* uniform rigid Z shift only -- 0 at k=1 and for every
                                        * caller that never opts in (mp6_widescreen_split_separate_x),
                                        * so the plain "no Z shove" contract is unchanged for them. */
    }
    e->liveMin->x = e->localCenterX + (e->nativeMin.x - e->localCenterX) * ratio - delta;
    e->liveMax->x = e->localCenterX + (e->nativeMax.x - e->localCenterX) * ratio + delta;
    e->liveMin->y = centerY + (e->nativeMin.y - centerY) * ratio;
    e->liveMax->y = centerY + (e->nativeMax.y - centerY) * ratio;
    e->liveMin->z = e->nativeMin.z + zDelta;
    e->liveMax->z = e->nativeMax.z + zDelta;
}

static void mp6_ws_split_separate_x_impl(int16_t modelId, float zBackRate)
{
    HSF_DATA *hsf;
    s16 i;
    float k;

    if (!mp6_widescreen_enabled() || modelId < 0) {
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        return;
    }
    k = mp6_widescreen_scale_factor();
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *o = &hsf->object[i];
        MP6WSSplitEntry *e;
        BOOL freshInstance;

        if (o->type != HSF_OBJ_MESH) {
            continue;
        }
        if (o->mesh.vertex == NULL || o->mesh.vertex->data == NULL || o->mesh.vertex->count <= 0) {
            continue;
        }
        e = mp6_ws_split_find(o->mesh.vertex->data);
        freshInstance = (e == NULL) || (e->hsf != hsf);
        if (e == NULL) {
            e = mp6_ws_split_alloc();
        }
        if (e == NULL) {
            continue; /* registry exhausted -- degrade to "not tracked", same graceful path every other registry here uses */
        }
        e->modelId = modelId;
        e->hsf = hsf;
        e->zBackRate = zBackRate; /* always refreshed (not just on freshInstance), same as modelId/hsf above */
        e->liveMin = &o->mesh.mesh.min;
        e->liveMax = &o->mesh.mesh.max;
        if (freshInstance) {
            size_t bytes = (size_t)o->mesh.vertex->count * sizeof(HuVecF);
            float *native = (float *)malloc(bytes);
            if (native == NULL) {
                continue; /* OOM on a tiny alloc -- extremely unlikely; degrade to "not tracked" */
            }
            free(e->native);
            e->native = native;
            memcpy(e->native, o->mesh.vertex->data, bytes);
            e->ptr = o->mesh.vertex->data;
            e->count = o->mesh.vertex->count;
            e->nativeMin = o->mesh.mesh.min;
            e->nativeMax = o->mesh.mesh.max;
            e->localCenterX = (e->nativeMin.x + e->nativeMax.x) * 0.5f;
            e->halfWidth = (e->nativeMax.x - e->nativeMin.x) * 0.5f;
        }
        mp6_ws_split_apply(e, k);
    }
    GXInvalidateVtxCache();
}

void mp6_widescreen_split_separate_x(int16_t modelId)
{
    mp6_ws_split_separate_x_impl(modelId, 0.0f);
}

void mp6_widescreen_split_separate_x_zback(int16_t modelId, float zBackAtRefK, float refK)
{
    /* refK==1 would be a caller bug (no widescreen extrusion to reference at
     * all) -- degrade to the plain, zero-Z-shift behavior rather than
     * divide by zero. */
    float zBackRate = (refK != 1.0f) ? (zBackAtRefK / (refK - 1.0f)) : 0.0f;
    mp6_ws_split_separate_x_impl(modelId, zBackRate);
}

/* ==========================================================================
 * Floor border-fill via a SEPARATE, NON-SHADOW-RECEIVING duplicate model
 * instance -- decouples the projected-shadow texcoord generation from
 * border_xz's own per-vertex fill, WITHOUT touching any shared decomp
 * draw code (game/hsfdraw.c/hsfman.c untouched).
 * ==========================================================================
 * THE PROBLEM: mdpartydll's mushroom-house/fence shadows are a genuine
 * real-time PROJECTED shadow -- every frame, `Hu3DShadowExec`
 * (game/hsfman.c) renders every `HU3D_ATTR_SHADOW`-flagged caster from a
 * shadow camera into a small offscreen texture, then `game/hsfdraw.c`'s
 * `SetShadow()` paints it onto every `HU3D_CONST_SHADOW_MAP`-flagged
 * receiver via `GXSetTexCoordGen(texCoord, GX_TG_MTX3x4, GX_TG_POS,
 * GX_TEXMTX9)` -- i.e. the texcoord fed to the shadow TEV stage is computed
 * LIVE, every draw, as `(shadowProj*shadowView*invCamera*drawObj->matrix) *
 * rawVertexPosition`, where `rawVertexPosition` is READ FROM THE SAME
 * GX_VA_POS ARRAY the ordinary render uses (`objMesh`'s own
 * `GXSetArray(GX_VA_POS, objPtr->mesh.vertex->data, ...)`, hsfdraw.c) --
 * confirmed directly by reading `ObjDraw`/`FaceDraw`'s own texture-matrix
 * setup. "yuka"'s own top-level transform is untouched by `border_xz`
 * (only raw vertex data is mutated), so `drawObj->matrix` here is exactly
 * native -- the ENTIRE distortion is the raw per-vertex position term,
 * which `border_xz` deliberately moves for its outer 16 perimeter
 * vertices (that is its whole job: close the widescreen gap). Since
 * GX_TG_POS has no API-level way to source a DIFFERENT position stream
 * from the one used for the main render within a single draw, and a
 * texture-MATRIX compensation is a single transform applied uniformly to
 * every vertex of one draw call (so it cannot selectively "undo" the
 * per-vertex/per-axis CONDITIONAL displacement border_xz applies to only
 * SOME vertices without equally corrupting the untouched native ones) --
 * neither a matrix trick nor "just feed it the native position" is
 * possible without a genuinely separate per-vertex position channel,
 * which would mean threading a new vertex-attribute path through
 * hsfdraw.c's `FaceDraw`/`SetShadow` -- shared draw code used by every
 * model in the game -- rejected as disproportionate to a cosmetic
 * shadow-alignment fix. A finer/graduated border mesh doesn't help either:
 * it would need new vertices+faces spliced into "yuka"'s own fixed 5x5
 * grid, and the actual caster geometry already reaches to within 50-130
 * world units of the floor's true edge, so there is no meaningfully-sized
 * native margin to reclaim by narrowing border_xz's own outer ring
 * either.
 *
 * THE FIX: since ONE vertex buffer cannot serve two different purposes in
 * one draw (native for shadow texcoord, extended for the fill), use TWO
 * draws instead. `Hu3DModelCreateData(dataNum)` (the SAME macro every
 * ordinary scene setup already calls, `game/hu3d.h`) is called a SECOND
 * time on the SAME on-disk floor asset -- confirmed safe by direct source
 * reading: it expands to `Hu3DModelCreate(HuDataSelHeapReadNum(...))`, and
 * `Hu3DModelCreate` (game/hsfman.c) unconditionally calls `LoadHSF` (this
 * port's `MP6_LoadHSFNative`, platform/hsf/hsf_load_native.c) fresh, which
 * allocates BRAND NEW buffers and decodes every vertex/face/attribute
 * array directly from the raw file bytes every single call -- no cache,
 * no pointer sharing keyed by data number. So a second
 * `Hu3DModelCreateData(DATA_mdparty+1)` yields a SECOND, fully independent
 * `HSF_DATA`/vertex buffer -- mutating IT via the EXISTING, unchanged
 * `mp6_widescreen_extrude_model_border_xz()` can never touch the FIRST
 * (original) instance's own vertex data. (This is a materially different,
 * and safe, operation from `Hu3DModelLink()`, game/hsfman.c's OTHER
 * "second instance" primitive, which explicitly SHARES object/attribute/
 * material data via `Hu3DObjDuplicate` et al and would NOT be safe here --
 * deliberately not used.)
 *
 * The ORIGINAL "yuka" instance (mdpartydll's own `obj->mdlId[1]`) is now
 * left COMPLETELY untouched by any widescreen mechanism -- 100% native
 * vertex data, exactly as `Hu3DModelShadowMapSet` (still called on it,
 * unchanged call site) already expects for the shadow receiver -- so its
 * own projected shadow decal is pixel-native for every world position the
 * native 4:3 game ever actually rendered. This NEW duplicate instance is
 * the one that gets `border_xz` (unchanged mechanism, unchanged formula,
 * same visual fill this project already shipped) -- and is deliberately
 * NEVER given `Hu3DModelShadowMapSet`, so its own moved outer-ring
 * vertices never feed a texcoord generator at all. The dynamic-resize/
 * reapply behavior comes for free: `border_xz` already registers whatever
 * modelId it's given into the existing border registry, so the duplicate
 * re-derives its own fill every frame from its OWN cached native snapshot
 * exactly like every other border_xz caller -- no new reapply wiring
 * needed here.
 *
 * TRADE-OFF, stated plainly (not hidden): over the region the native 4:3
 * game already showed (the inner 800x800 footprint) the shadow is now
 * fully native -- but a mushroom-house/fence shadow that would have
 * projected FURTHER OUT than that (into space the widescreen hack itself
 * reveals) is simply ABSENT there, rather than stretched. That region was
 * never rendered by the native game at all, so "no shadow decal in
 * screen-space the original never had" is judged a more honest outcome
 * than a decal that visibly grows/deforms as the window is resized -- but
 * it is a real, deliberate trade-off, not a full elimination of every
 * possible shadow ray.
 *
 * COINCIDENT-SURFACE (Z-FIGHT) SAFETY: the duplicate's own inner 9 (of 25)
 * vertices are left untouched by border_xz (same as the original's), so
 * over the shared 800x800 footprint the two instances render the exact
 * same native geometry at the exact same world position -- which, without
 * more care, is exactly the setup for undefined-order Z-fighting between
 * two coincident opaque surfaces. Resolved with a standard coplanar-decal
 * offset: the duplicate's own top-level Y position (never read by any game
 * logic -- this is a synthetic instance this mechanism alone owns) is set
 * once, a small constant distance BELOW native (`MP6_WS_FLOOR_DUP_Y_BIAS`),
 * strictly farther from this scene's downward-looking camera at every
 * shared point. Because GX's Z-test (`GX_LEQUAL`, Z-write on for this
 * material, confirmed via hsfdraw.c's own `FaceDraw`) is a per-fragment
 * comparison against whatever depth is already buffered, this makes the
 * ORIGINAL (nearer) instance win the shared inner region regardless of
 * which of the two instances the engine happens to submit first -- no
 * dependency on this project's own model-draw ordering. The offset is
 * tiny relative to this scene's own scale (party board pieces span
 * hundreds to ~1600 world units) -- chosen larger than any plausible
 * float rounding at this coordinate magnitude, small enough to be
 * visually undetectable in a near-overhead view; confirmed clean (no
 * visible seam/flicker) by direct capture rather than assumed.
 *
 * Never touches the ORIGINAL model's own top-level transform (position,
 * scale, rotation) at all -- the ONLY change to the ORIGINAL floor
 * instance is that it no longer receives a border_xz call at its own
 * former call site. Same default-off contract as every other mechanism in
 * this file (gated on mp6_widescreen_enabled(); a disabled build never
 * calls Hu3DModelCreateData a second time at all, so the duplicate simply
 * never exists -- byte-identical to the original single-instance
 * behavior). */
#define MP6_WS_FLOOR_DUP_Y_BIAS (-2.0f)

/* ==========================================================================
 * Companion fix: the border-fill duplicate's stretched ring must sample
 * CLEAN tile texels -- a user report of "baked shadow leakage in
 * widescreen floor."
 * ==========================================================================
 * ROOT CAUSE, measured off the decoded ys77_floor bitmap (256x256 RGB565;
 * tools/mp6scene dump + texel census): the floor art bakes the
 * mushroom-house/fence shadows INTO the picture as dark blobs hugging
 * the FAR half -- component-filtered census (luma < 160 vs the clean-tile
 * median 201, centre circle excluded): one 4738 px blob at s 0.227..0.875
 * / t 0.012..0.369 (t=0 is the far z=-800 edge), plus side blobs reaching
 * s=0.125 (left) and s=0.875 (right). border_xz moves ONLY vertices --
 * every ST stays authored -- so each outer ring cell STRETCHES its
 * authored texture band across the widescreen margin, and the visible
 * band (world |coord| > 800) samples the outermost ~14% of the texture on
 * each stretched axis at k=1.625: the far band carries 2523 shadow px of
 * 9216 and the side bands the blob tips -- exactly the diagonal smears
 * visible in the shipped captures' lower corners.
 *
 * FIX: rewrite the DUPLICATE's ring-cell STs once, at creation, so the
 * stretched bands sample the measured shadow-free parts of the SAME art
 * (the native floor instance -- the shadow receiver -- is never touched;
 * its own baked shadows render pixel-native where the artist put them):
 *   - FAR ring cells (any corner vert at native z = -800): every loop t
 *     := MP6_WS_FLOOR_ST_PIN_T. The tile art is a vertical-stripe pattern
 *     (colour varies with s, constant along t, measured: rows 104..255
 *     are shadow-free full-width and rows 192..255 are also below the
 *     circle decal), so pinning t continues the stripes along z at native
 *     density -- visually identical to the shipped stretch EXCEPT the
 *     shadows, which are the only t-varying content in the band. This is
 *     the pillar strip's own pinned-UV trick, one axis over.
 *   - LEFT/RIGHT ring cells (any corner vert at native x = -+800): the
 *     INNER loop columns (verts at x = -+400) move from s = 0.25/0.75 to
 *     MP6_WS_FLOOR_ST_EDGE_INSET / 1 - it, so the whole cell samples the
 *     measured shadow-free edge-column runs (texels 0..31 / 228..255
 *     incl. penumbra margin; the stripe pattern itself continues, just
 *     from a narrower clean band). The inset also bounds the sampled band
 *     for EVERY k by construction: as k grows the visible band's inner
 *     sample limit tends to the inset itself (texel ~27 / ~228), still
 *     inside the clean runs -- no aspect re-derivation needed, ever.
 *   - NEAR ring cells (z = +800): untouched -- their band (texture rows
 *     220..255) measured 0 shadow px.
 * Only the region the native game never rendered changes (the ring beyond
 * +-800 world is occluded by the native floor everywhere the two
 * overlap); at k = 1 the duplicate is fully covered by the native floor
 * (Y_BIAS keeps it strictly below), so the rewrite is invisible there,
 * and a disabled build never creates the duplicate at all. STs are
 * written once (they are k-independent constants; only vertex positions
 * ride the per-frame reapply) and the arrays die with the duplicate model.
 * Validated against the measured "yuka" layout before any write (25
 * verts on the exact {0,+-400,+-800} grid, 16 QUAD faces, 64 per-loop
 * STs, bijective ST indexing) -- anything else refuses and leaves the
 * duplicate exactly as before this fix. */
#define MP6_WS_FLOOR_ST_EDGE_INSET 0.105f /* clean edge-column runs: texels 0..31 / 228..255 (blob max col 227) */
#define MP6_WS_FLOOR_ST_PIN_T      0.9f   /* texture row 229.5: shadow-free full-width, below the circle decal */

/* Defined in the natural-extension section below; shared here for the
 * same refuse-don't-guess discipline and the same tolerant float compare. */
static void mp6_ws_extend_refuse(const char *objName, const char *why);
static BOOL mp6_ws_pillar_near(float a, float b, float eps);

static void mp6_ws_floor_dup_clean_sts(HU3D_MODELID dupId, const char *objName)
{
    HSF_DATA *hsf = Hu3DData[dupId].hsf;
    HSF_OBJECT *o = NULL;
    HuVecF *verts;
    HuVec2f *sts;
    HSF_FACE *faces;
    float halfX, halfZ;
    BOOL seen[64];
    s16 i, c;

    if (hsf == NULL) {
        return;
    }
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *cand = &hsf->object[i];
        if (cand->type == HSF_OBJ_MESH && cand->name != NULL && strcmp(cand->name, objName) == 0) {
            o = cand;
            break;
        }
    }
    if (o == NULL || o->mesh.vertex == NULL || o->mesh.vertex->data == NULL
        || o->mesh.st == NULL || o->mesh.st->data == NULL
        || o->mesh.face == NULL || o->mesh.face->data == NULL) {
        mp6_ws_extend_refuse(objName, "floor dup: missing buffers");
        return;
    }
    verts = (HuVecF *)o->mesh.vertex->data;
    sts = (HuVec2f *)o->mesh.st->data;
    faces = (HSF_FACE *)o->mesh.face->data;
    /* The measured layout, refused otherwise: 5x5 grid = 25 verts on the
     * exact {0, +-half/2, +-half} lattice in x and z, 16 QUAD cells, 64
     * per-loop STs indexed bijectively. */
    if (o->mesh.vertex->count != 25 || o->mesh.face->count != 16 || o->mesh.st->count != 64) {
        mp6_ws_extend_refuse(objName, "floor dup: not the 25/16/64 grid layout");
        return;
    }
    halfX = o->mesh.mesh.max.x;
    halfZ = o->mesh.mesh.max.z;
    if (!mp6_ws_pillar_near(o->mesh.mesh.min.x, -halfX, 0.01f)
        || !mp6_ws_pillar_near(o->mesh.mesh.min.z, -halfZ, 0.01f)
        || halfX <= 0.0f || halfZ <= 0.0f) {
        mp6_ws_extend_refuse(objName, "floor dup: box not symmetric");
        return;
    }
    for (i = 0; i < 25; i++) {
        float ax = verts[i].x < 0.0f ? -verts[i].x : verts[i].x;
        float az = verts[i].z < 0.0f ? -verts[i].z : verts[i].z;
        if (!(mp6_ws_pillar_near(ax, 0.0f, 0.01f) || mp6_ws_pillar_near(ax, halfX * 0.5f, 0.01f)
              || mp6_ws_pillar_near(ax, halfX, 0.01f))
            || !(mp6_ws_pillar_near(az, 0.0f, 0.01f) || mp6_ws_pillar_near(az, halfZ * 0.5f, 0.01f)
                 || mp6_ws_pillar_near(az, halfZ, 0.01f))) {
            mp6_ws_extend_refuse(objName, "floor dup: vert off the 5x5 lattice");
            return;
        }
    }
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < 16; i++) {
        if ((u16)(faces[i].typeSrc & HSF_FACE_MASK) != (u16)HSF_FACE_QUAD) {
            mp6_ws_extend_refuse(objName, "floor dup: non-QUAD cell");
            return;
        }
        for (c = 0; c < 4; c++) {
            s16 st = faces[i].index[c].st;
            if (st < 0 || st >= 64 || seen[st]) {
                mp6_ws_extend_refuse(objName, "floor dup: ST indexing not bijective");
                return;
            }
            seen[st] = TRUE;
        }
    }
    /* All checks green -- rewrite the ring cells' loops. Runs BEFORE
     * border_xz (vertex positions still native), so edge membership reads
     * the authored lattice directly. */
    for (i = 0; i < 16; i++) {
        BOOL atXMin = FALSE, atXMax = FALSE, atZFar = FALSE;
        for (c = 0; c < 4; c++) {
            const HuVecF *v = &verts[faces[i].index[c].vertex];
            if (mp6_ws_pillar_near(v->x, -halfX, 0.01f)) atXMin = TRUE;
            if (mp6_ws_pillar_near(v->x, halfX, 0.01f)) atXMax = TRUE;
            if (mp6_ws_pillar_near(v->z, -halfZ, 0.01f)) atZFar = TRUE;
        }
        for (c = 0; c < 4; c++) {
            const HuVecF *v = &verts[faces[i].index[c].vertex];
            HuVec2f *st = &sts[faces[i].index[c].st];
            if (atZFar) {
                st->y = MP6_WS_FLOOR_ST_PIN_T;
            }
            if (atXMin && mp6_ws_pillar_near(v->x, -halfX * 0.5f, 0.01f)) {
                st->x = MP6_WS_FLOOR_ST_EDGE_INSET;
            }
            if (atXMax && mp6_ws_pillar_near(v->x, halfX * 0.5f, 0.01f)) {
                st->x = 1.0f - MP6_WS_FLOOR_ST_EDGE_INSET;
            }
        }
    }
}

typedef struct {
    HU3D_MODELID dupModelId;
    HSF_DATA *dupHsf; /* liveness snapshot, same pattern as every other registry here */
} MP6WSFloorDupEntry;

/* This project has exactly one confirmed user (the party floor) -- 4 gives
 * headroom for re-entering Party Mode many times in a session, matching
 * MP6WSBorderEntry/MP6WSSplitEntry's own identical sizing rationale. */
#define MP6_WS_FLOOR_DUP_MAX 4
static MP6WSFloorDupEntry g_mp6WsFloorDup[MP6_WS_FLOOR_DUP_MAX];
static s16 g_mp6WsFloorDupNum = 0;

/* Called once per scene setup (mdpartydll/mdparty.c's fn_1_861C, in place
 * of the direct `mp6_widescreen_extrude_model_border_xz(obj->mdlId[i],
 * "yuka")` call this replaces) -- `dataNum` is the SAME `DATA_mdparty+1`
 * data-number expression the caller's own `Hu3DModelCreateData` call
 * already uses for the ORIGINAL floor instance (a plain enum-derived int,
 * matching this header's own established int16_t/int32_t-not-decomp-type
 * precedent so this stays callable from a decomp-header-rich REL TU
 * without any type mismatch). `objName` is passed straight through to the
 * unchanged `mp6_widescreen_extrude_model_border_xz()` call this function
 * makes on the NEW duplicate. No-op (creates nothing) when Widescreen is
 * disabled, matching every other mechanism's default-off contract. */
void mp6_widescreen_floor_border_fill_dup(int32_t dataNum, const char *objName)
{
    HU3D_MODELID dupId;
    HU3D_MOTIONID motId;
    HuVecF pos;

    if (!mp6_widescreen_enabled() || objName == NULL) {
        return;
    }
    if (g_mp6WsFloorDupNum >= MP6_WS_FLOOR_DUP_MAX) {
        return; /* registry exhausted -- degrade to "no fill duplicate this
                  * entry" rather than crash; not expected given this
                  * project's real Party-Mode re-entry count. */
    }
    dupId = Hu3DModelCreateData(dataNum);
    if (dupId < 0 || Hu3DData[dupId].hsf == NULL) {
        return;
    }
    /* Mirrors the ESSENTIAL parts of the same per-sub-model setup
     * mdpartydll's own fn_1_861C already does for every real party
     * sub-model (Hu3DModelLayerSet + motion-shift) -- "yuka" itself has no
     * real motion data (confirmed this session: its own `[HSF] loaded`
     * census shows no accompanying deform-section line, unlike the
     * skinned character models), so `Hu3DMotionIDGet` is expected to read
     * back HU3D_MOTIONID_NONE here and `Hu3DMotionShiftSet` a graceful
     * no-op -- mirrored anyway for exact parity with the original
     * instance's own setup rather than assuming it never matters. */
    Hu3DModelLayerSet(dupId, 1);
    motId = Hu3DMotionIDGet(dupId);
    Hu3DMotionShiftSet(dupId, motId, 0.0f, 0.0f, HU3D_MOTATTR_LOOP);

    /* Coplanar-decal offset -- see this section's own file-header comment
     * for the full Z-fight-safety derivation. Hu3DModelCreate's own default
     * pos is (0,0,0), matching the original floor instance's native
     * position exactly; only Y is nudged, once, permanently (not part of
     * the dynamic-reapply system -- this is a fixed rendering-order
     * safety margin, not a widescreen-scale-dependent quantity). */
    Hu3DModelPosGet(dupId, &pos);
    Hu3DModelPosSet(dupId, pos.x, pos.y + MP6_WS_FLOOR_DUP_Y_BIAS, pos.z);

    /* Deliberately NEVER Hu3DModelShadowMapSet(dupId) here -- this instance
     * exists ONLY to provide the border-extended VISUAL fill; it must never
     * become a shadow receiver, so its own extended outer-ring vertices can
     * never feed GX_TG_POS's projected-shadow texcoord generator. */

    /* Retargets the duplicate's ring-cell STs onto the measured
     * shadow-free bands of its own texture BEFORE the border extension
     * moves any vertex (the helper keys cell membership off the authored
     * lattice) -- see mp6_ws_floor_dup_clean_sts' own section comment for
     * the full texel census and rules. The ORIGINAL floor instance is
     * untouched, as ever. */
    mp6_ws_floor_dup_clean_sts(dupId, objName);
    mp6_widescreen_extrude_model_border_xz(dupId, objName);

    g_mp6WsFloorDup[g_mp6WsFloorDupNum].dupModelId = dupId;
    g_mp6WsFloorDup[g_mp6WsFloorDupNum].dupHsf = Hu3DData[dupId].hsf;
    g_mp6WsFloorDupNum++;
}

/* Companion teardown -- called once per scene teardown (mdpartydll's own
 * fn_1_87BC), alongside the existing Hu3DModelKill loop over
 * obj->mdlId[0..2]. Kills every LIVE registered duplicate (the liveness
 * check mirrors every other registry in this file: a slot whose own
 * modelId's CURRENT Hu3DData[].hsf no longer matches what was live at
 * registration time was already killed/recycled elsewhere and must not be
 * resurrected) and resets the registry, so a later Party-Mode re-entry
 * starts from a clean slate. A true no-op when Widescreen was never
 * enabled (the registry stays permanently empty in that case, matching
 * every other mechanism's default-off contract). */
void mp6_widescreen_floor_border_fill_dup_kill_all(void)
{
    s16 i;
    for (i = 0; i < g_mp6WsFloorDupNum; i++) {
        MP6WSFloorDupEntry *e = &g_mp6WsFloorDup[i];
        if (Hu3DData[e->dupModelId].hsf == e->dupHsf) {
            Hu3DModelKill(e->dupModelId);
        }
    }
    g_mp6WsFloorDupNum = 0;
}

/* ==========================================================================
 * Natural scene extension -- the curated backdrops stay PIXEL-NATIVE in
 * the centre and the margins are generated from the scene's OWN art, as
 * appended degenerate-collapsible quads in the object's OWN buffers:
 *   - mode-select "sora" (mdsel[2]): periodic TILE strips continuing the
 *     checker outward (day window u [0, 93/512], world tile width
 *     93*(6000/512)=1089.84375; night [1-95/512, 1], 1113.28125 -- both
 *     windows measured on the decoded 512x256 bitmap, exhaustive shift
 *     search; counts: k=1.625 -> 2+2, k=1.775 -> 3+3). Replaces an earlier
 *     selective horizontal stretch of sora's own verts.
 *   - title "grid5"/"grid14" (title[0x14]): one MIRROR border quad per
 *     side, u running BACKWARD from the fold (u=0 at x=-288 -- C0-exact,
 *     the native edge texel; outer u = (k-1)/2, clamped at 1.0 -- a single
 *     fold is valid to k=3, a 3.6:1 window). Chosen over tiling because
 *     neither back_ex nor back_house has ANY measurable x-periodicity
 *     (autocorrelation < 0.20 everywhere). Replaces an earlier selective
 *     horizontal stretch of these two quads.
 *   - party "CP" (mdparty[3]): a pinned-UV rim RING -- one quad per fan
 *     chord (16), inner vertices the rim vertices VERBATIM (watertight),
 *     outer the same positions scaled by k, both loops carrying the rim's
 *     own loop STs, so the ring samples, radially constant, the exact UV
 *     chord the disc's outer edge samples (the rim band r_tex 0.98..1.00
 *     is flat opaque gold, sd <= 4.2, alpha 255; a radial mirror would
 *     replay the disc's TRANSPARENT interior and was rejected on that
 *     pixel data). Replaces an earlier whole-model isotropic extrude --
 *     which also stops scaling panel1/panel2 (the live board-map render
 *     targets): they now render at native size at wide aspects, a
 *     deliberate, accepted side effect, as is the authored gold surround
 *     becoming visible in the wide corners.
 *   All UVs stay inside [0,1] (sub-window repetition is done by geometry
 *   subdivision, not wrap mode) -- every touched material is CLAMP/CLAMP
 *   and needs NO override.
 *   - A fourth treatment on this same mechanism: the party "pillar" EDGE
 *     EXTRUSION (its own section further down). Its strip STs are
 *     verbatim copies of authored edge-loop STs, including the arch
 *     chain's authored v>1 CLAMP-band value -- the art's own top-edge
 *     dissolve -- so the [0,1] rule above is deliberately not asserted
 *     for it; CLAMP still needs no override for the same reason the ring
 *     needs none (every strip loop is an authored loop's own value).
 *
 * WHY GROWN BUFFERS, NOT COMPANION MODELS (the floor-dup shape used
 * above): Hu3DModelCreateData can only re-decode an ON-DISC asset (LoadHSF
 * parses raw file bytes -- there is no create-from-generated-HSF_DATA
 * path), and, decisively, the title model draws every object from
 * `mesh.curr` under its baked reveal motion (base writes are ineffective
 * there): a sibling model could never ride that motion, while faces
 * appended to the object's OWN buffers ride `curr` automatically. A design
 * pass proved the two shapes draw-equivalent at bind pose (its companion
 * objects copy the source's parent + base TRS) -- that equivalence is
 * what makes tools/mp6scene/build/wsdesign_*.png valid evidence for THIS
 * engine shape.
 *
 * THE DISPLAY-LIST RE-BAKE (engine fact the spec's design pass could not
 * see, discovered by direct source reading here): game/hsfdraw.c's
 * MakeDisplayList (called once inside Hu3DModelCreate) bakes every
 * object's face INDICES into a GX display list (constData->dlBuf), and
 * the per-frame FaceDraw only sets material state then
 * GXCallDisplayList()s that bake -- faceBuf->count is never re-read at
 * draw time, so faces appended after model create would simply never
 * draw. Vertex/ST POSITIONS are still fetched live (GXSetArray points GX
 * at mesh.vertex->data/st->data every draw -- the same live-array fact
 * every other vertex mechanism in this file already relies on), so
 * the fix splits cleanly: grow the buffers ONCE at registration, then
 * re-run MakeDisplayList(modelId, mallocNo) ONCE so the new (fixed-
 * capacity, initially zero-area) quads are in the bake -- and from then
 * on the per-tick reapply only rewrites array CONTENTS (positions; the
 * mirror's STs), which the baked indices fetch live. The re-bake is
 * gated, and safe, because:
 *   - the old per-object constData/drawData/dlBuf are explicitly
 *     HuMemDirectFree'd first (all three are individual
 *     HuMemDirectMallocNum allocations under the model's own mallocNo --
 *     ObjConstantMake/MDObjMesh, game/hsfdraw.c) -- no leak, not even a
 *     bounded one;
 *   - MakeDisplayList's only side effect beyond the bake is
 *     `if (attr & HU3D_ATTR_SHADOW) shadowNum++` -- a double increment
 *     that would permanently skew the engine's shadow refcount, so
 *     registration REFUSES (skips, with a diag line) any model carrying
 *     HU3D_ATTR_SHADOW. None of the three registered models is a shadow
 *     caster (sora/STAR/CP -- mdsel and title have no Hu3DShadowExec at
 *     all; party's casters are the props model, DATA_mdparty+2);
 *   - every constData flag it resets is either re-derived at bake/draw
 *     time from the material data (XLU/ALTBLEND/...) or never set on
 *     these models (HU3D_CONST_SHADOW_MAP is the party FLOOR's, which
 *     this mechanism never touches) -- and every call site registers
 *     immediately after model create, before anything else touches
 *     constData.
 * The party CP model's Hu3DModelLink twin is handled by ORDER, not code:
 * fn_1_9218 registers at i==0, BEFORE the i==1 Hu3DModelLink, so the
 * link's Hu3DObjDuplicate (game/hsfdraw.c -- a memcpy of the object
 * array + a memcpy of each constData) copies the ALREADY-re-baked
 * constData/dlBuf pointers and the twin replays the grown bake; the
 * buffers themselves (mesh.vertex/st/face pointers, copied by value) are
 * shared outright, so the one per-tick ring rewrite feeds both instances
 * -- exactly how the twins already share the HSF.
 *
 * BUFFER OWNERSHIP / TEARDOWN: the grown vertex/ST/face arrays are
 * allocated with HuMemDirectMallocNum(HEAP_MODEL, ..., modelP->mallocNo)
 * -- the SAME heap and tag hsf_load_native.c used for the arrays they
 * replace (whose individual allocations are HuMemDirectFree'd here once
 * unreferenced) -- so Hu3DModelKill's own tag bulk-free reclaims them
 * with the rest of the model and NO new teardown call site is needed
 * anywhere (unlike the floor dup above, which is a whole extra model
 * instance and does need its explicit kill). A killed/reused model slot
 * is detected by the same Hu3DData[modelId].hsf identity check every
 * registry in this file uses; dead entries are recycled, never
 * resurrected. New vertex/ST pointers are mp6_gxarray_register'd with
 * their real byte size, exactly like the loader registers the originals.
 *
 * SAVESTATE: this registry deliberately caches ONLY scalars and arena
 * pointers (the grown buffers, the object, the hsf) -- NO host-heap
 * snapshot exists, so unlike the four vertex registries above there is
 * nothing to serialize or rehydrate: a restore brings back the grown
 * arena buffers, this TU's registry statics, and the identity check just
 * holds. Deliberately NOT added to the blob writer's registry ids.
 *
 * THE k=1 ZERO CONTRACT: every appended quad collapses to zero area at
 * k=1 (tiles/mirror: all four corners on the seam edge; ring: outer ==
 * rim verbatim), GX rasterises nothing, and the object's live
 * mesh.min/max are written back EXACTLY native -- and a disabled build
 * never registers (or grows, or re-bakes) anything at all, so default-off
 * stays byte-identical, the same contract every mechanism in this file
 * keeps. The proven counts: k=1.625 -> sora 2+2 strips, k=1.775 -> 3+3
 * (fixed capacity 9+9 covers k <= 4.0, a ~5.3:1 window -- beyond it the
 * strip count saturates and coverage falls short, accepted for aspects
 * no real display has); grids exactly 1+1; ring exactly 16; the pillar
 * treatment exactly 16 duplicates + 10 quads, k-independent.
 *
 * QUAD AUTHORING FACTS (measured off the real on-disc data via
 * tools/mp6scene/mp6_hsf.py, this session -- not assumed):
 *   - HSF QUAD corner storage is a strip/Z order, NOT a loop: FaceDraw
 *     emits index[0], index[2], index[3], index[1], and GX's front face
 *     is CLOCKWISE in this engine's +y-up screen frame (grid5's own
 *     native quad: [0]=TL(-288,240) [1]=BL [2]=TR [3]=BR -- emission
 *     TL,TR,BR,BL). Every appended quad here authors that same
 *     [TL, BL, TR, BR] slot pattern with TL/BL the x-min pair, which
 *     reproduces the native winding on both sides of the screen.
 *   - sora: 6 verts (x in {-3000,0,3000}, y in {-800,2200}, z 0), 2 QUAD
 *     faces, raw u 0 at x=-3000 (day/left) .. 1 at x=+3000 (night), raw
 *     t 0 at y=+2200 (top) .. 1 at y=-800; grid5/grid14: one 4-vert QUAD
 *     each, u 0 at x=-288 .. 1 at +288, t 0 at top; CP: 17 verts (fan
 *     centre v0 + 16 rim), 16 TRI faces [rim_p, rim_q, centre] with the
 *     centre always corner slot 2 in the on-disc data (the code derives
 *     it per face by radius anyway), per-face loop STs (the quarter-arc
 *     window repeats 4x around the disc, so one rim vertex carries
 *     DIFFERENT STs in adjacent faces -- which is exactly why the ring
 *     reuses each chord's own ST INDICES verbatim instead of averaging
 *     anything; zero ST growth for the ring).
 *   - every one of these objects authors constant (0,0,1) normals and no
 *     colour buffer (color index -1, vtxMode != 5) -- appended corners
 *     copy a native corner's normal index (zero normal growth) and
 *     color -1.
 * ========================================================================== */
typedef enum {
    MP6_WS_EXTEND_TILES = 0,
    MP6_WS_EXTEND_MIRROR,
    MP6_WS_EXTEND_RING,
    MP6_WS_EXTEND_PILLAR  /* pillar edge extrusion (see its own section below) */
} MP6WSExtendMode;

/* Fixed per-side strip capacity for the TILE treatment: ceil(halfW*(kCap-1)
 * / tileW) at registration, bounded by this compile-time maximum (sora at
 * kCap=4.0 needs ceil(9000/1089.84)=9 per side). */
#define MP6_WS_EXTEND_TILE_CAP_MAX 12
/* The ring treatment is authored for CP's exact 16-gon fan -- refused
 * (degrade to no extension) for any other chord count. */
#define MP6_WS_EXTEND_RIM_MAX 16

typedef struct {
    HU3D_MODELID modelId;
    HSF_DATA *hsf;           /* identity/liveness snapshot -- see file header */
    HSF_OBJECT *obj;         /* the grown object, inside hsf->object[] */
    MP6WSExtendMode mode;
    /* grown live buffers (== obj->mesh.vertex->data / st->data after the
     * swap; re-checked against the object at reapply as a cheap paranoia
     * guard) */
    HuVecF *vtx;
    HuVec2f *st;
    s32 baseVtx;             /* native vertex count == first appended index */
    s32 baseSt;              /* native ST count */
    /* tiles + mirror */
    float halfW;             /* native half extent in object-local x */
    float y0, y1;            /* object-local bottom / top edge */
    float zRef;              /* the plane's own z (0 for all three assets) */
    float tTop, tBot;        /* raw ST t at the top / bottom edge (read off
                              * the native corner records at registration) */
    float tileWL, tileWR;    /* world width of one day / night tile */
    float puL, puR;          /* u-window width: periodPx / texW per side */
    s16 capL, capR;          /* appended strip capacity per side */
    /* ring -- and, since the counts coincide exactly (16), the pillar
     * treatment reuses these same arrays with a different meaning per
     * element i: rimVtx = the SOURCE edge vertex index the duplicate copies,
     * rimX = the side sign (-1 left / +1 right; the per-tick x is
     * sign*halfW*k), rimY/rimZ = the duplicate's fixed y/z (copied verbatim
     * from the source vertex at registration, never scaled). */
    s16 rimCount;
    s16 rimVtx[MP6_WS_EXTEND_RIM_MAX];   /* native rim vertex indices */
    float rimX[MP6_WS_EXTEND_RIM_MAX];   /* native rim positions */
    float rimY[MP6_WS_EXTEND_RIM_MAX];
    float rimZ[MP6_WS_EXTEND_RIM_MAX];
    /* cull box bookkeeping: the object's own authored native min/max --
     * written back verbatim at k=1 (byte-identical contract) and extended
     * to the live companion reach otherwise, the same "keep the cull walk
     * honest" duty established by the mechanisms above. */
    HuVecF nativeMin, nativeMax;
} MP6WSExtendEntry;

/* Five live users exist (sora, grid5, grid14, CP, and the pillar --
 * at most two of them, CP + pillar, even share a scene) and only one
 * scene is ever on-screen -- 8 gives the same re-entry headroom margin as
 * every other registry in this file (dead slots recycled by liveness). */
#define MP6_WS_EXTEND_MAX 8
static MP6WSExtendEntry g_mp6WsExtend[MP6_WS_EXTEND_MAX];
static s16 g_mp6WsExtendNum = 0;

static MP6WSExtendEntry *mp6_ws_extend_find(HSF_OBJECT *obj)
{
    s16 i;
    for (i = 0; i < g_mp6WsExtendNum; i++) {
        if (g_mp6WsExtend[i].obj == obj && Hu3DData[g_mp6WsExtend[i].modelId].hsf == g_mp6WsExtend[i].hsf) {
            return &g_mp6WsExtend[i];
        }
    }
    return NULL;
}

static MP6WSExtendEntry *mp6_ws_extend_alloc(void)
{
    s16 i;
    for (i = 0; i < g_mp6WsExtendNum; i++) {
        if (Hu3DData[g_mp6WsExtend[i].modelId].hsf != g_mp6WsExtend[i].hsf) {
            return &g_mp6WsExtend[i]; /* recycle a dead slot */
        }
    }
    if (g_mp6WsExtendNum < MP6_WS_EXTEND_MAX) {
        return &g_mp6WsExtend[g_mp6WsExtendNum++];
    }
    return NULL;
}

/* Env-gated (MP6_WS_HSF_DIAG, the file's existing diagnostic switch)
 * refusal trace -- a violated layout expectation degrades to "no
 * extension for this object" silently in a normal run, loudly under the
 * diag env, never to a guess. */
static void mp6_ws_extend_refuse(const char *objName, const char *why)
{
    if (getenv("MP6_WS_HSF_DIAG")) {
        printf("[MP6-WS-EXTEND] %s: REFUSED -- %s\n", objName, why);
        fflush(stdout);
    }
}

/* One appended quad's four corners, [TL, BL, TR, BR] with (xl,*) the x-min
 * pair -- see the section header's QUAD AUTHORING FACTS for why this slot
 * pattern reproduces the native winding on either side of the screen. */
static void mp6_ws_extend_write_quad_verts(HuVecF *v, float xl, float xr,
                                           float y0, float y1, float z)
{
    v[0].x = xl; v[0].y = y1; v[0].z = z; /* TL */
    v[1].x = xl; v[1].y = y0; v[1].z = z; /* BL */
    v[2].x = xr; v[2].y = y1; v[2].z = z; /* TR */
    v[3].x = xr; v[3].y = y0; v[3].z = z; /* BR */
}

static void mp6_ws_extend_write_quad_sts(HuVec2f *st, float ul, float ur,
                                         float tTop, float tBot)
{
    st[0].x = ul; st[0].y = tTop;
    st[1].x = ul; st[1].y = tBot;
    st[2].x = ur; st[2].y = tTop;
    st[3].x = ur; st[3].y = tBot;
}

/* The shared grow-and-rebake step. Appends `addVtx` vertices + `addSt` STs
 * (both initialised degenerate by the caller's first apply) and `addQuads`
 * QUAD faces (authored by `writeFaces` into the appended tail) to the
 * object's own buffers, swaps them live, then re-bakes the model's display
 * lists. Returns FALSE (nothing changed) on any allocation failure --
 * degrade to "no extension", same graceful path every registry here uses. */
static BOOL mp6_ws_extend_grow(HU3D_MODELID modelId, HSF_OBJECT *o,
                               s32 addVtx, s32 addSt, s32 addQuads,
                               void (*writeFaces)(MP6WSExtendEntry *e, HSF_FACE *tail, const HSF_FACE *nativeFaces),
                               MP6WSExtendEntry *e)
{
    HSF_BUFFER *vb = o->mesh.vertex;
    HSF_BUFFER *stb = o->mesh.st;
    HSF_BUFFER *fb = o->mesh.face;
    u32 mallocNo = Hu3DData[modelId].mallocNo;
    HuVecF *newV;
    HuVec2f *newSt;
    HSF_FACE *newF;
    s32 i;

    newV = (HuVecF *)HuMemDirectMallocNum(HEAP_MODEL, (s32)((vb->count + addVtx) * sizeof(HuVecF)), mallocNo);
    newSt = (HuVec2f *)HuMemDirectMallocNum(HEAP_MODEL, (s32)((stb->count + addSt) * sizeof(HuVec2f)), mallocNo);
    newF = (HSF_FACE *)HuMemDirectMallocNum(HEAP_MODEL, (s32)((fb->count + addQuads) * sizeof(HSF_FACE)), mallocNo);
    if (newV == NULL || newSt == NULL || newF == NULL) {
        if (newV != NULL) HuMemDirectFree(newV);
        if (newSt != NULL) HuMemDirectFree(newSt);
        if (newF != NULL) HuMemDirectFree(newF);
        return FALSE;
    }
    memcpy(newV, vb->data, (size_t)vb->count * sizeof(HuVecF));
    memcpy(newSt, stb->data, (size_t)stb->count * sizeof(HuVec2f));
    memcpy(newF, fb->data, (size_t)fb->count * sizeof(HSF_FACE));
    /* Appended verts/STs start as copies of vertex 0 / ST 0 (any real
     * value works -- the caller's immediate apply overwrites every one;
     * zero-area is guaranteed by THAT write, not by this init). */
    for (i = 0; i < addVtx; i++) {
        newV[vb->count + i] = newV[0];
    }
    for (i = 0; i < addSt; i++) {
        newSt[stb->count + i] = newSt[0];
    }
    memset(newF + fb->count, 0, (size_t)addQuads * sizeof(HSF_FACE));
    writeFaces(e, newF + fb->count, (const HSF_FACE *)fb->data);

    /* Swap live. Draw code fetches these pointers fresh every draw
     * (GXSetArray in FaceDraw; MDObjMesh at the re-bake below), so there
     * is no dangling window -- and the OLD arrays are individual
     * HuMemDirectMallocNum allocations (hsf_load_native.c's
     * LoadVertexArrays/LoadSTArrays/LoadFaceGroups), so freeing them
     * directly is exact, not a heuristic. */
    {
        void *oldV = vb->data, *oldSt = stb->data, *oldF = fb->data;
        vb->data = newV;
        vb->count += addVtx;
        stb->data = newSt;
        stb->count += addSt;
        fb->data = newF;
        fb->count += addQuads;
        mp6_gxarray_register(newV, (uint32_t)(vb->count * sizeof(HuVecF)));
        mp6_gxarray_register(newSt, (uint32_t)(stb->count * sizeof(HuVec2f)));
        HuMemDirectFree(oldV);
        HuMemDirectFree(oldSt);
        HuMemDirectFree(oldF);
    }

    /* Re-bake the display lists so the appended faces are in the bake --
     * see the section header's DISPLAY-LIST RE-BAKE block for the full
     * safety argument. Old constData/drawData/dlBuf freed first (they are
     * re-created per mesh object by MakeDisplayList).
     *
     * The receiver-side SHADOW-MAP state must survive the re-bake.
     * Hu3DModelShadowMapSet (game/hsfman.c) stores its effect in
     * constData->attr (HU3D_CONST_SHADOW_MAP; the TPLvl variant also sets
     * HU3D_CONST_SHADOW_MAP_TPLVL + constData->shadowAlpha) -- and
     * MakeDisplayList's ObjConstantMake resets attr to HU3D_CONST_NONE and
     * only re-derives the MATERIAL-flag bits (SHADOW/SHADOW_MAP-from-
     * material/ALTBLEND/HILITE/..., game/hsfdraw.c), never an API-set
     * receiver bit. That was harmless for sora/grids/CP (none of them is a
     * shadow receiver) -- but the party pillar registration below runs
     * AFTER fn_1_861C's own Hu3DModelShadowMapSet(obj->mdlId[i]) call on
     * that very model, so without this snapshot/restore the re-bake would
     * silently stop the pillar receiving the mushroom-house/fence projected
     * shadows. Snapshotted per object BEFORE the frees, OR-ed back after
     * MakeDisplayList; a no-op (all-zero bits) for every caller that isn't
     * a shadow receiver. */
    {
        HSF_DATA *hsf = Hu3DData[modelId].hsf;
        u32 *keepAttr = (u32 *)malloc((size_t)hsf->objectNum * sizeof(u32));
        u8 *keepAlpha = (u8 *)malloc((size_t)hsf->objectNum * sizeof(u8));
        for (i = 0; i < hsf->objectNum; i++) {
            HSF_OBJECT *oi = &hsf->object[i];
            if (keepAttr != NULL && keepAlpha != NULL) {
                keepAttr[i] = 0;
                keepAlpha[i] = 0;
                if (oi->type == HSF_OBJ_MESH && oi->constData != NULL) {
                    HSF_CONSTDATA *cd = (HSF_CONSTDATA *)oi->constData;
                    keepAttr[i] = cd->attr & (HU3D_CONST_SHADOW_MAP | HU3D_CONST_SHADOW_MAP_TPLVL);
                    keepAlpha[i] = cd->shadowAlpha;
                }
            }
            if (oi->type == HSF_OBJ_MESH && oi->constData != NULL) {
                HSF_CONSTDATA *cd = (HSF_CONSTDATA *)oi->constData;
                if (cd->dlBuf != NULL) HuMemDirectFree(cd->dlBuf);
                if (cd->drawData != NULL) HuMemDirectFree(cd->drawData);
                HuMemDirectFree(cd);
                oi->constData = NULL;
            }
        }
        MakeDisplayList(modelId, mallocNo);
        if (keepAttr != NULL && keepAlpha != NULL) {
            for (i = 0; i < hsf->objectNum; i++) {
                HSF_OBJECT *oi = &hsf->object[i];
                if (keepAttr[i] != 0 && oi->type == HSF_OBJ_MESH && oi->constData != NULL) {
                    HSF_CONSTDATA *cd = (HSF_CONSTDATA *)oi->constData;
                    cd->attr |= keepAttr[i];
                    if (keepAttr[i] & HU3D_CONST_SHADOW_MAP_TPLVL) {
                        cd->shadowAlpha = keepAlpha[i];
                    }
                }
            }
        }
        /* keepAttr/keepAlpha NULL (host-heap OOM on a tiny alloc --
         * extremely unlikely): the re-bake still happened correctly; only
         * the receiver-bit restore is skipped, degrading exactly to the
         * behavior from before this shadow-bit preservation existed. */
        free(keepAttr);
        free(keepAlpha);
    }
    return TRUE;
}

/* Shared registration preamble: liveness/identity, object lookup by exact
 * name, and the layout checks every treatment needs (a real mesh, QUAD/TRI
 * face data, no skinning/cluster/shape channels, and -- because every
 * mechanism here mutates buffers in place -- no OTHER object sharing this
 * object's vertex/ST/face buffer pointers, the exact hazard
 * mp6_widescreen_debug_dump_model()'s vtxBuf census exists to check). */
static HSF_OBJECT *mp6_ws_extend_locate(HU3D_MODELID modelId, const char *objName)
{
    HSF_DATA *hsf;
    HSF_OBJECT *o = NULL;
    s16 i;

    if (!mp6_widescreen_enabled() || modelId < 0 || objName == NULL) {
        return NULL;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        return NULL;
    }
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *cand = &hsf->object[i];
        if (cand->type == HSF_OBJ_MESH && cand->name != NULL && strcmp(cand->name, objName) == 0) {
            o = cand;
            break;
        }
    }
    if (o == NULL) {
        mp6_ws_extend_refuse(objName, "object not found");
        return NULL;
    }
    if (o->mesh.vertex == NULL || o->mesh.vertex->data == NULL || o->mesh.vertex->count <= 0
        || o->mesh.st == NULL || o->mesh.st->data == NULL || o->mesh.st->count <= 0
        || o->mesh.face == NULL || o->mesh.face->data == NULL || o->mesh.face->count <= 0
        || o->mesh.normal == NULL || o->mesh.normal->data == NULL || o->mesh.normal->count <= 0) {
        mp6_ws_extend_refuse(objName, "missing vertex/st/face/normal data");
        return NULL;
    }
    if (o->mesh.cenvNum != 0 || o->mesh.clusterNum != 0 || o->mesh.shapeNum != 0) {
        mp6_ws_extend_refuse(objName, "skinned/cluster/shape mesh -- buffer growth not designed for it");
        return NULL;
    }
    if (Hu3DData[modelId].attr & HU3D_ATTR_SHADOW) {
        /* Re-running MakeDisplayList would shadowNum++ a second time --
         * see the section header. Not true for any registered model. */
        mp6_ws_extend_refuse(objName, "model is a shadow caster -- re-bake unsafe");
        return NULL;
    }
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *other = &hsf->object[i];
        if (other == o || other->type != HSF_OBJ_MESH) {
            continue;
        }
        if (other->mesh.vertex == o->mesh.vertex || other->mesh.st == o->mesh.st
            || other->mesh.face == o->mesh.face) {
            mp6_ws_extend_refuse(objName, "buffer shared with another object");
            return NULL;
        }
    }
    return o;
}

/* --------------------------------------------------------------------------
 * TILES apply -- positions only (the u windows are constants written once
 * at registration; only the live strip COUNT varies with k).
 * -------------------------------------------------------------------------- */
static void mp6_ws_extend_tiles_apply(MP6WSExtendEntry *e, float k)
{
    float ext = e->halfW * (k - 1.0f);
    s16 side, i;
    s32 q = 0;

    for (side = 0; side < 2; side++) {
        float w = (side == 0) ? e->tileWL : e->tileWR;
        s16 cap = (side == 0) ? e->capL : e->capR;
        float sign = (side == 0) ? -1.0f : 1.0f;
        s16 n = 0;
        if (ext > 0.0f && w > 0.0f) {
            n = (s16)ceilf(ext / w - 1e-6f);
        }
        if (n > cap) {
            n = cap; /* saturate past kCap -- see the section header */
        }
        for (i = 1; i <= cap; i++, q++) {
            HuVecF *v = e->vtx + e->baseVtx + q * 4;
            if (i <= n) {
                float xNear = sign * (e->halfW + (float)(i - 1) * w);
                float xFar = sign * (e->halfW + (float)i * w);
                if (side == 0) {
                    mp6_ws_extend_write_quad_verts(v, xFar, xNear, e->y0, e->y1, e->zRef);
                } else {
                    mp6_ws_extend_write_quad_verts(v, xNear, xFar, e->y0, e->y1, e->zRef);
                }
            } else {
                /* collapsed: all four corners on the seam edge -- zero area */
                mp6_ws_extend_write_quad_verts(v, sign * e->halfW, sign * e->halfW, e->y0, e->y0, e->zRef);
            }
        }
    }
    /* Cull box: native at k=1 (byte-identical), else out to the live strip
     * reach (which meets or exceeds the halfW*k the shipped stretch
     * reached, by ceil()). Only x moves; y/z stay authored-native. */
    e->obj->mesh.mesh.min = e->nativeMin;
    e->obj->mesh.mesh.max = e->nativeMax;
    if (ext > 0.0f) {
        s16 nL = (s16)ceilf(ext / e->tileWL - 1e-6f);
        s16 nR = (s16)ceilf(ext / e->tileWR - 1e-6f);
        if (nL > e->capL) nL = e->capL;
        if (nR > e->capR) nR = e->capR;
        e->obj->mesh.mesh.min.x = -(e->halfW + (float)nL * e->tileWL);
        e->obj->mesh.mesh.max.x = e->halfW + (float)nR * e->tileWR;
    }
}

/* --------------------------------------------------------------------------
 * MIRROR apply -- positions AND STs (the outer u tracks (k-1)/2 live,
 * clamped at 1.0: a single fold holds to k=3; past it the margin shows the
 * full mirrored texture rather than inventing an unspecified double fold).
 * -------------------------------------------------------------------------- */
static void mp6_ws_extend_mirror_apply(MP6WSExtendEntry *e, float k)
{
    float ext = e->halfW * (k - 1.0f);
    float uExt = (k - 1.0f) * 0.5f;
    HuVecF *v = e->vtx + e->baseVtx;
    HuVec2f *st = e->st + e->baseSt;

    if (ext < 0.0f) {
        ext = 0.0f;
    }
    if (uExt < 0.0f) {
        uExt = 0.0f;
    }
    if (uExt > 1.0f) {
        uExt = 1.0f;
    }
    /* left quad: fold at x=-halfW carries u=0 (the native edge texel --
     * C0-exact), outer edge carries the mirrored u = uExt.  Both quads'
     * appended tails are 4 verts + 4 STs each ([TL,BL,TR,BR] slots), so
     * the right quad lives at +4 in BOTH arrays -- an st+8 here (an
     * early bring-up bug this session) wrote 32 bytes past the grown ST
     * buffer and corrupted the adjacent heap block. */
    mp6_ws_extend_write_quad_verts(v, -e->halfW - ext, -e->halfW, e->y0, e->y1, e->zRef);
    mp6_ws_extend_write_quad_sts(st, uExt, 0.0f, e->tTop, e->tBot);
    /* right quad: fold at x=+halfW carries u=1 */
    mp6_ws_extend_write_quad_verts(v + 4, e->halfW, e->halfW + ext, e->y0, e->y1, e->zRef);
    mp6_ws_extend_write_quad_sts(st + 4, 1.0f, 1.0f - uExt, e->tTop, e->tBot);

    e->obj->mesh.mesh.min = e->nativeMin;
    e->obj->mesh.mesh.max = e->nativeMax;
    if (ext > 0.0f) {
        e->obj->mesh.mesh.min.x = -(e->halfW + ext);
        e->obj->mesh.mesh.max.x = e->halfW + ext;
    }
}

/* --------------------------------------------------------------------------
 * RING apply -- positions only (each outer vertex = its rim vertex's
 * native position scaled by k about the disc's own local origin; the ring
 * STs are the rim chords' own ST indices, never touched).
 * -------------------------------------------------------------------------- */
static void mp6_ws_extend_ring_apply(MP6WSExtendEntry *e, float k)
{
    s16 i;
    for (i = 0; i < e->rimCount; i++) {
        HuVecF *v = e->vtx + e->baseVtx + i;
        v->x = e->rimX[i] * k;
        v->y = e->rimY[i] * k;
        v->z = e->rimZ[i];
    }
    /* The disc is authored about local (0,0): scaling its box about the
     * origin is exact, and collapses to the authored values at k=1. */
    e->obj->mesh.mesh.min = e->nativeMin;
    e->obj->mesh.mesh.max = e->nativeMax;
    e->obj->mesh.mesh.min.x = e->nativeMin.x * k;
    e->obj->mesh.mesh.min.y = e->nativeMin.y * k;
    e->obj->mesh.mesh.max.x = e->nativeMax.x * k;
    e->obj->mesh.mesh.max.y = e->nativeMax.y * k;
}

/* --------------------------------------------------------------------------
 * PILLAR apply -- positions only. Every duplicate i sits at
 * x = sign_i * halfW * k with its y/z fixed at the values copied from its
 * source edge vertex at registration; the strip STs are pinned copies of
 * the source loops' own STs, written once at registration and never touched
 * again. At k = 1 every duplicate lands exactly ON its source vertex, so
 * all 10 strip quads are zero-area (GX rasterises nothing) and the live
 * min/max write back exactly native (x * 1.0f is exact) -- the same zero
 * contract every treatment in this registry keeps. Native vertices are
 * never written by ANY path of this treatment.
 * -------------------------------------------------------------------------- */
static void mp6_ws_extend_pillar_apply(MP6WSExtendEntry *e, float k)
{
    s16 i;
    for (i = 0; i < e->rimCount; i++) {
        HuVecF *v = e->vtx + e->baseVtx + i;
        v->x = e->rimX[i] * e->halfW * k;   /* rimX = side sign, +-1 */
        v->y = e->rimY[i];
        v->z = e->rimZ[i];
    }
    /* The authored box is symmetric about x=0 (+-halfW) -- only x grows;
     * y/z stay authored-native, exactly like the tiles/mirror boxes. */
    e->obj->mesh.mesh.min = e->nativeMin;
    e->obj->mesh.mesh.max = e->nativeMax;
    e->obj->mesh.mesh.min.x = e->nativeMin.x * k;
    e->obj->mesh.mesh.max.x = e->nativeMax.x * k;
}

static void mp6_ws_extend_apply(MP6WSExtendEntry *e, float k)
{
    if (e->vtx == NULL || e->obj == NULL || e->obj->mesh.vertex == NULL
        || e->obj->mesh.vertex->data != e->vtx) {
        return; /* buffer no longer ours (paranoia guard) -- never scribble */
    }
    switch (e->mode) {
        case MP6_WS_EXTEND_TILES:
            mp6_ws_extend_tiles_apply(e, k);
            break;
        case MP6_WS_EXTEND_MIRROR:
            mp6_ws_extend_mirror_apply(e, k);
            break;
        case MP6_WS_EXTEND_RING:
            mp6_ws_extend_ring_apply(e, k);
            break;
        case MP6_WS_EXTEND_PILLAR:
            mp6_ws_extend_pillar_apply(e, k);
            break;
    }
}

/* Face-tail writers, one per treatment (called by mp6_ws_extend_grow with
 * the entry's base counts already final). Every appended face copies the
 * object's own first native face's mat + nbt (same material batch -- the
 * re-bake merges same-material same-type runs into one GXBegin) and a
 * native corner's normal index; color stays -1 (no colour buffer on any
 * of these objects, vtxMode != 5). */
static void mp6_ws_extend_write_faces_quads(MP6WSExtendEntry *e, HSF_FACE *tail, const HSF_FACE *nativeFaces, s32 quadCount)
{
    s16 nrmIdx = nativeFaces[0].index[0].normal;
    s32 q, c;
    for (q = 0; q < quadCount; q++) {
        HSF_FACE *f = &tail[q];
        f->typeSrc = (u16)HSF_FACE_QUAD;
        f->mat = nativeFaces[0].mat;
        f->nbt[0] = nativeFaces[0].nbt[0];
        f->nbt[1] = nativeFaces[0].nbt[1];
        f->nbt[2] = nativeFaces[0].nbt[2];
        for (c = 0; c < 4; c++) {
            f->index[c].vertex = (s16)(e->baseVtx + q * 4 + c);
            f->index[c].normal = nrmIdx;
            f->index[c].color = -1;
            f->index[c].st = (s16)(e->baseSt + q * 4 + c);
        }
    }
}

static void mp6_ws_extend_write_faces_tiles(MP6WSExtendEntry *e, HSF_FACE *tail, const HSF_FACE *nativeFaces)
{
    mp6_ws_extend_write_faces_quads(e, tail, nativeFaces, (s32)e->capL + (s32)e->capR);
    /* The constant u windows are written by the registrar right after the
     * grow swaps the ST buffer live (the writer runs before the swap and
     * cannot see it) -- see mp6_ws_extend_tiles_write_sts(). */
}

/* The tile u windows are constants: day strips sample [0, puL] (u
 * increases with x inside each strip, exactly the periodic continuation
 * u(x) = ((x+halfW)/(2 halfW)) mod puL), night strips [1-puR, 1]. Written
 * ONCE, at registration -- STs never change for tiles afterwards. */
static void mp6_ws_extend_tiles_write_sts(MP6WSExtendEntry *e)
{
    s32 q = 0;
    s16 side, i;
    for (side = 0; side < 2; side++) {
        for (i = 0; i < ((side == 0) ? e->capL : e->capR); i++, q++) {
            HuVec2f *st = e->st + e->baseSt + q * 4;
            if (side == 0) {
                mp6_ws_extend_write_quad_sts(st, 0.0f, e->puL, e->tTop, e->tBot);
            } else {
                mp6_ws_extend_write_quad_sts(st, 1.0f - e->puR, 1.0f, e->tTop, e->tBot);
            }
        }
    }
}

static void mp6_ws_extend_write_faces_mirror(MP6WSExtendEntry *e, HSF_FACE *tail, const HSF_FACE *nativeFaces)
{
    mp6_ws_extend_write_faces_quads(e, tail, nativeFaces, 2);
    /* STs are live (uExt tracks k) -- the first apply writes them. */
}

static void mp6_ws_extend_write_faces_ring(MP6WSExtendEntry *e, HSF_FACE *tail, const HSF_FACE *nativeFaces)
{
    /* One quad per fan chord. Derived and winding-verified off the real
     * CP data (section header): for native TRI [p, q, centre] (centre =
     * the one corner whose vertex is NOT in the registration's rim set --
     * membership, not a second radius derivation, so the writer only
     * consumes already-validated cached state), the ring quad is
     *   index[0] = outer(q) carrying q's own normal/st indices,
     *   index[1] = rim q            (same indices),
     *   index[2] = outer(p) carrying p's own normal/st indices,
     *   index[3] = rim p            (same indices),
     * whose emission (0,2,3,1) = outerQ -> outerP -> rimP -> rimQ matches
     * the fan's own clockwise front face. Inner corners ARE the rim
     * vertex indices verbatim -- watertight; along the shared edge the
     * disc triangle and ring quad interpolate the same UV chord (C0
     * pixel-exact, spec section 4). */
    s32 f;
    for (f = 0; f < MP6_WS_EXTEND_RIM_MAX; f++) {
        const HSF_FACE *tri = &nativeFaces[f];
        HSF_FACE *quad = &tail[f];
        s16 ci = -1, pSlot, qSlot, m, pOut = -1, qOut = -1;
        s16 c;
        for (c = 0; c < 3; c++) {
            BOOL isRim = FALSE;
            for (m = 0; m < e->rimCount; m++) {
                if (e->rimVtx[m] == tri->index[c].vertex) {
                    isRim = TRUE;
                    break;
                }
            }
            if (!isRim) {
                ci = c; /* the fan-centre corner */
            }
        }
        if (ci < 0) {
            continue; /* registration validated this cannot happen */
        }
        pSlot = (s16)((ci + 1) % 3);
        qSlot = (s16)((ci + 2) % 3);
        for (m = 0; m < e->rimCount; m++) {
            if (e->rimVtx[m] == tri->index[pSlot].vertex) pOut = m;
            if (e->rimVtx[m] == tri->index[qSlot].vertex) qOut = m;
        }
        if (pOut < 0 || qOut < 0) {
            continue; /* registration validated this cannot happen */
        }
        quad->typeSrc = (u16)HSF_FACE_QUAD;
        quad->mat = tri->mat;
        quad->nbt[0] = tri->nbt[0];
        quad->nbt[1] = tri->nbt[1];
        quad->nbt[2] = tri->nbt[2];
        quad->index[0].vertex = (s16)(e->baseVtx + qOut);
        quad->index[0].normal = tri->index[qSlot].normal;
        quad->index[0].color = -1;
        quad->index[0].st = tri->index[qSlot].st;
        quad->index[1] = tri->index[qSlot];
        quad->index[1].color = -1;
        quad->index[2].vertex = (s16)(e->baseVtx + pOut);
        quad->index[2].normal = tri->index[pSlot].normal;
        quad->index[2].color = -1;
        quad->index[2].st = tri->index[pSlot].st;
        quad->index[3] = tri->index[pSlot];
        quad->index[3].color = -1;
    }
}

/* --------------------------------------------------------------------------
 * Public registration entry points (spec section 5.1's call shapes).
 * Each: once per (modelId, hsf) pairing -- a repeat call on the same live
 * instance just re-applies from the cached registration; a reused HU3D
 * slot is a fresh registration (grow + re-bake again on the NEW decode).
 * -------------------------------------------------------------------------- */
void mp6_widescreen_extend_tiles(int16_t modelId, const char *objName,
                                 float halfW, int32_t texW,
                                 int32_t periodPxL, int32_t periodPxR, float kCap)
{
    HSF_OBJECT *o = mp6_ws_extend_locate(modelId, objName);
    MP6WSExtendEntry *e;
    float k;

    if (o == NULL) {
        return;
    }
    e = mp6_ws_extend_find(o);
    if (e == NULL) {
        HuVecF *verts = (HuVecF *)o->mesh.vertex->data;
        HSF_FACE *faces = (HSF_FACE *)o->mesh.face->data;
        float minX = verts[0].x, maxX = verts[0].x, minY = verts[0].y, maxY = verts[0].y;
        float tTop = -1.0f, tBot = -1.0f;
        s32 i;
        s16 c;

        if (texW <= 0 || periodPxL <= 0 || periodPxR <= 0 || halfW <= 0.0f || kCap <= 1.0f) {
            mp6_ws_extend_refuse(objName, "bad tile parameters");
            return;
        }
        for (i = 1; i < o->mesh.vertex->count; i++) {
            if (verts[i].x < minX) minX = verts[i].x;
            if (verts[i].x > maxX) maxX = verts[i].x;
            if (verts[i].y < minY) minY = verts[i].y;
            if (verts[i].y > maxY) maxY = verts[i].y;
        }
        /* The measured layout this treatment was designed on (spec 1.1):
         * a symmetric plane spanning exactly +-halfW, split at x=0, QUAD
         * faces, u 0..1 across. Anything else: refuse, never guess. */
        if (minX != -halfW || maxX != halfW) {
            mp6_ws_extend_refuse(objName, "x span != +-halfW");
            return;
        }
        for (i = 0; i < o->mesh.face->count; i++) {
            if ((faces[i].typeSrc & HSF_FACE_MASK) != HSF_FACE_QUAD) {
                mp6_ws_extend_refuse(objName, "non-QUAD native face");
                return;
            }
        }
        /* Read the raw t at the top/bottom edge off the native corner
         * records (raw ST space -- NOT assumed; sora authors t=0 at the
         * TOP edge, measured this session). */
        {
            HuVec2f *sts = (HuVec2f *)o->mesh.st->data;
            for (i = 0; i < o->mesh.face->count; i++) {
                for (c = 0; c < 4; c++) {
                    const HuVecF *pv = &verts[faces[i].index[c].vertex];
                    const HuVec2f *ps = &sts[faces[i].index[c].st];
                    if (pv->y == maxY) tTop = ps->y;
                    if (pv->y == minY) tBot = ps->y;
                }
            }
        }
        if (tTop < 0.0f || tBot < 0.0f || tTop == tBot) {
            mp6_ws_extend_refuse(objName, "could not read edge t values");
            return;
        }
        e = mp6_ws_extend_alloc();
        if (e == NULL) {
            return; /* registry exhausted -- degrade to "no extension" */
        }
        memset(e, 0, sizeof(*e));
        e->modelId = modelId;
        e->hsf = Hu3DData[modelId].hsf;
        e->obj = o;
        e->mode = MP6_WS_EXTEND_TILES;
        e->halfW = halfW;
        e->y0 = minY;
        e->y1 = maxY;
        e->zRef = verts[0].z;
        e->tTop = tTop;
        e->tBot = tBot;
        e->puL = (float)periodPxL / (float)texW;
        e->puR = (float)periodPxR / (float)texW;
        e->tileWL = e->puL * 2.0f * halfW;
        e->tileWR = e->puR * 2.0f * halfW;
        {
            s32 capL = (s32)ceilf(halfW * (kCap - 1.0f) / e->tileWL - 1e-6f);
            s32 capR = (s32)ceilf(halfW * (kCap - 1.0f) / e->tileWR - 1e-6f);
            if (capL > MP6_WS_EXTEND_TILE_CAP_MAX) capL = MP6_WS_EXTEND_TILE_CAP_MAX;
            if (capR > MP6_WS_EXTEND_TILE_CAP_MAX) capR = MP6_WS_EXTEND_TILE_CAP_MAX;
            if (capL < 1) capL = 1;
            if (capR < 1) capR = 1;
            e->capL = (s16)capL;
            e->capR = (s16)capR;
        }
        e->nativeMin = o->mesh.mesh.min;
        e->nativeMax = o->mesh.mesh.max;
        e->baseVtx = o->mesh.vertex->count;
        e->baseSt = o->mesh.st->count;
        if (!mp6_ws_extend_grow(modelId, o, (e->capL + e->capR) * 4, (e->capL + e->capR) * 4,
                                e->capL + e->capR, mp6_ws_extend_write_faces_tiles, e)) {
            e->obj = NULL; /* mark dead: allocation failed, nothing grew */
            e->hsf = NULL;
            return;
        }
        e->vtx = (HuVecF *)o->mesh.vertex->data;
        e->st = (HuVec2f *)o->mesh.st->data;
        mp6_ws_extend_tiles_write_sts(e);
    }
    k = mp6_widescreen_scale_factor();
    mp6_ws_extend_apply(e, k);
    GXInvalidateVtxCache();
}

void mp6_widescreen_extend_mirror(int16_t modelId, const char *objName, float halfW)
{
    HSF_OBJECT *o = mp6_ws_extend_locate(modelId, objName);
    MP6WSExtendEntry *e;
    float k;

    if (o == NULL) {
        return;
    }
    e = mp6_ws_extend_find(o);
    if (e == NULL) {
        HuVecF *verts = (HuVecF *)o->mesh.vertex->data;
        HSF_FACE *faces = (HSF_FACE *)o->mesh.face->data;
        float minX = verts[0].x, maxX = verts[0].x, minY = verts[0].y, maxY = verts[0].y;
        float tTop = -1.0f, tBot = -1.0f;
        s32 i;
        s16 c;

        if (halfW <= 0.0f) {
            mp6_ws_extend_refuse(objName, "bad mirror halfW");
            return;
        }
        for (i = 1; i < o->mesh.vertex->count; i++) {
            if (verts[i].x < minX) minX = verts[i].x;
            if (verts[i].x > maxX) maxX = verts[i].x;
            if (verts[i].y < minY) minY = verts[i].y;
            if (verts[i].y > maxY) maxY = verts[i].y;
        }
        if (minX != -halfW || maxX != halfW) {
            mp6_ws_extend_refuse(objName, "x span != +-halfW");
            return;
        }
        for (i = 0; i < o->mesh.face->count; i++) {
            if ((faces[i].typeSrc & HSF_FACE_MASK) != HSF_FACE_QUAD) {
                mp6_ws_extend_refuse(objName, "non-QUAD native face");
                return;
            }
        }
        {
            HuVec2f *sts = (HuVec2f *)o->mesh.st->data;
            for (i = 0; i < o->mesh.face->count; i++) {
                for (c = 0; c < 4; c++) {
                    const HuVecF *pv = &verts[faces[i].index[c].vertex];
                    const HuVec2f *ps = &sts[faces[i].index[c].st];
                    if (pv->y == maxY) tTop = ps->y;
                    if (pv->y == minY) tBot = ps->y;
                }
            }
        }
        if (tTop < 0.0f || tBot < 0.0f || tTop == tBot) {
            mp6_ws_extend_refuse(objName, "could not read edge t values");
            return;
        }
        e = mp6_ws_extend_alloc();
        if (e == NULL) {
            return;
        }
        memset(e, 0, sizeof(*e));
        e->modelId = modelId;
        e->hsf = Hu3DData[modelId].hsf;
        e->obj = o;
        e->mode = MP6_WS_EXTEND_MIRROR;
        e->halfW = halfW;
        e->y0 = minY;
        e->y1 = maxY;
        e->zRef = verts[0].z;
        e->tTop = tTop;
        e->tBot = tBot;
        e->nativeMin = o->mesh.mesh.min;
        e->nativeMax = o->mesh.mesh.max;
        e->baseVtx = o->mesh.vertex->count;
        e->baseSt = o->mesh.st->count;
        if (!mp6_ws_extend_grow(modelId, o, 8, 8, 2, mp6_ws_extend_write_faces_mirror, e)) {
            e->obj = NULL;
            e->hsf = NULL;
            return;
        }
        e->vtx = (HuVecF *)o->mesh.vertex->data;
        e->st = (HuVec2f *)o->mesh.st->data;
    }
    k = mp6_widescreen_scale_factor();
    mp6_ws_extend_apply(e, k);
    GXInvalidateVtxCache();
}

void mp6_widescreen_extend_ring(int16_t modelId, const char *objName)
{
    HSF_OBJECT *o = mp6_ws_extend_locate(modelId, objName);
    MP6WSExtendEntry *e;
    float k;

    if (o == NULL) {
        return;
    }
    e = mp6_ws_extend_find(o);
    if (e == NULL) {
        HuVecF *verts = (HuVecF *)o->mesh.vertex->data;
        HSF_FACE *faces = (HSF_FACE *)o->mesh.face->data;
        float maxR2 = 0.0f;
        s32 i;
        s16 c;
        s16 rimCount = 0;
        s16 rimVtx[MP6_WS_EXTEND_RIM_MAX];

        if (o->mesh.face->count != MP6_WS_EXTEND_RIM_MAX) {
            mp6_ws_extend_refuse(objName, "fan is not exactly 16 chords");
            return;
        }
        for (i = 0; i < o->mesh.vertex->count; i++) {
            float r2 = verts[i].x * verts[i].x + verts[i].y * verts[i].y;
            if (r2 > maxR2) maxR2 = r2;
        }
        for (i = 0; i < o->mesh.face->count; i++) {
            s16 ci = -1;
            float bestR2 = -1.0f;
            if ((faces[i].typeSrc & HSF_FACE_MASK) != HSF_FACE_TRI) {
                mp6_ws_extend_refuse(objName, "non-TRI fan face");
                return;
            }
            for (c = 0; c < 3; c++) {
                const HuVecF *pv = &verts[faces[i].index[c].vertex];
                float r2 = pv->x * pv->x + pv->y * pv->y;
                if (bestR2 < 0.0f || r2 < bestR2) {
                    bestR2 = r2;
                    ci = c;
                }
            }
            /* The centre corner must genuinely be a fan centre (well
             * inside the rim), and the other two must be rim vertices --
             * the same "no centre vertex -> refuse" check the design's
             * own generator asserts. */
            if (bestR2 > maxR2 * 0.25f) {
                mp6_ws_extend_refuse(objName, "face without a fan-centre corner");
                return;
            }
            for (c = 0; c < 3; c++) {
                s16 vi;
                s16 m;
                BOOL seen = FALSE;
                if (c == ci) {
                    continue;
                }
                vi = faces[i].index[c].vertex;
                for (m = 0; m < rimCount; m++) {
                    if (rimVtx[m] == vi) {
                        seen = TRUE;
                        break;
                    }
                }
                if (!seen) {
                    if (rimCount >= MP6_WS_EXTEND_RIM_MAX) {
                        mp6_ws_extend_refuse(objName, "more than 16 rim vertices");
                        return;
                    }
                    rimVtx[rimCount++] = vi;
                }
            }
        }
        if (rimCount != MP6_WS_EXTEND_RIM_MAX) {
            mp6_ws_extend_refuse(objName, "rim vertex count != 16");
            return;
        }
        e = mp6_ws_extend_alloc();
        if (e == NULL) {
            return;
        }
        memset(e, 0, sizeof(*e));
        e->modelId = modelId;
        e->hsf = Hu3DData[modelId].hsf;
        e->obj = o;
        e->mode = MP6_WS_EXTEND_RING;
        e->rimCount = rimCount;
        for (i = 0; i < rimCount; i++) {
            e->rimVtx[i] = rimVtx[i];
            e->rimX[i] = verts[rimVtx[i]].x;
            e->rimY[i] = verts[rimVtx[i]].y;
            e->rimZ[i] = verts[rimVtx[i]].z;
        }
        e->nativeMin = o->mesh.mesh.min;
        e->nativeMax = o->mesh.mesh.max;
        e->baseVtx = o->mesh.vertex->count;
        e->baseSt = o->mesh.st->count;
        if (!mp6_ws_extend_grow(modelId, o, MP6_WS_EXTEND_RIM_MAX, 0, MP6_WS_EXTEND_RIM_MAX,
                                mp6_ws_extend_write_faces_ring, e)) {
            e->obj = NULL;
            e->hsf = NULL;
            return;
        }
        e->vtx = (HuVecF *)o->mesh.vertex->data;
        e->st = (HuVec2f *)o->mesh.st->data;
    }
    k = mp6_widescreen_scale_factor();
    mp6_ws_extend_apply(e, k);
    GXInvalidateVtxCache();
}

/* ==========================================================================
 * Party pillar EDGE EXTRUSION -- replaces an earlier split+move+Z-back
 * treatment of the party proscenium border (mdparty[0], object "pillar")
 * outright. Directive chain: extend the pillars instead of moving them;
 * grab the outermost vertices and move them to the screen edge; extrude
 * rather than pull-stretch. Standard mesh-extrude semantics: every native
 * vertex/face/UV/normal stays byte-identical, the 7 outermost verts per
 * side (|x| = 800 exactly; the nearest interior vert is 106.4 units away
 * at |x| = 693.595, so "outermost" is exact, not a band) are DUPLICATED,
 * only the duplicates translate outward to +-800k, and the bridging quad
 * strips carry PINNED UVs -- both strip columns sample the source edge
 * loop's own STs, so the border art continues outward at native texel
 * density with zero stretch (the swirl rim ring's own trick, one
 * mechanism over). Measured off the real texture: the edge texel column
 * is pure flat white over the column band (sd 0.0) and horizontally
 * uniform grey + the gold trim exit over the arch band (adjacent-column
 * RMS <= 1.84), so the horizontally-constant strip is pixel-equivalent to
 * what the art itself does at its own edge.
 *
 * WHY THIS SUPERSEDES THE SPLIT: moving the split halves outward is what
 * opened the floor-behind-border parallax window -- the Z-back/size-
 * preserve family above exists only to compensate that movement, and an
 * extended floor-gap row scan measured the shipped split form STILL
 * leaking hundreds of pixels of floor below the historically-scanned
 * band, at both tested aspects. With the border NATIVE (no split, no
 * Z-back, centre-x untouched, arch corner pieces continuous, strips lying
 * IN their sheets' authored z planes) the parallax mechanism is
 * structurally absent -- the design renders measure 0/0 across the FULL
 * extended band at both aspects, with a control render (border ends at
 * +-800) confirming the metric bites. A simpler alternative tried first
 * (translate the edge verts, append nothing) also measured 0/0 but smears
 * the arch trim's wisp ornament ~6x across the margin -- its resampling
 * zone reaches inboard to world |x| 565..800, INSIDE the native frame --
 * exactly the stretch this directive rejects.
 *
 * THE STRIP GEOMETRY (per side): three extrudable edge chains -- column
 * front edge (z=-750; y 0 / 267.47 / 499.43), arch front edge (z=-700; y
 * 421.57 / 508.86 / 1000) and the arch rim pair (the two y=421.57 verts at
 * z=-700/-800 joined by the arch's bottom rim-wall quad) -- give 2+2+1 = 5
 * bridge quads and 3+3+2 = 8 duplicates per side (the arch's bottom corner
 * vert duplicates once for its fill strip and once for the rim strip:
 * per-loop UVs are face-dependent -- that vert carries u=0.004 on the
 * front fill but u=0.886 on its rim-wall quad -- and the extrusion honours
 * the per-face loop UV exactly as authored). Totals: 16 duplicated verts,
 * 16 appended STs (pinned copies, written once), 10 QUAD faces, fixed for
 * all k. Winding is DERIVED from the native data, not assumed: each strip
 * quad's emission cycle traverses its shared edge OPPOSITE to the native
 * face it continues (the standard manifold-consistency rule,
 * convention-free), read off the real faces' own GX emission order (TRI
 * emits corners 0,2,1; QUAD 0,2,3,1 -- game/hsfdraw.c's FaceDraw, same
 * facts the section header above records). Appended corners copy their
 * source loop's own normal INDEX (zero normal growth) and color -1 (no
 * colour buffer on this object -- validated).
 *
 * Registration anchors on the measured shipped layout and REFUSES
 * (degrade to native, loud under MP6_WS_HSF_DIAG) anything else: half
 * width exactly 800, exactly 7 edge verts per side, the 3/3/1 z-sheet
 * census per side, left/right mirror symmetry, sheet-fill loop STs that
 * agree per vert, exactly one rim-wall face per side, and no TRISTRIP
 * face touching any edge vert (the pillar's 6 strips are interior rim
 * walls -- verified on the real data). Coverage: +-800k is algebraically
 * the shipped split's own outer-edge reach (800 + 800(k-1)), which covers
 * the live frame edge with >20% margin at both proven aspects (resting-
 * camera frame edge ~658k at the column plane; spec section 1.3).
 * ========================================================================== */
#define MP6_WS_PILLAR_HALF_W   800.0f  /* |x| of the outer edge columns (measured, exact) */
#define MP6_WS_PILLAR_COL_Z    (-750.0f) /* column front fill sheet */
#define MP6_WS_PILLAR_ARCH_Z   (-700.0f) /* arch front fill sheet */
#define MP6_WS_PILLAR_BACK_Z   (-800.0f) /* rim back plane (one authored z has a -800.00006 wobble) */
#define MP6_WS_PILLAR_EDGE_EPS 0.5f    /* nearest interior vert is at |x|=693.595 */
#define MP6_WS_PILLAR_Z_EPS    0.01f
#define MP6_WS_PILLAR_EDGE_PER_SIDE 7
#define MP6_WS_PILLAR_DUPS     16      /* 8 per side: 3 column + 3 arch + 2 rim */
#define MP6_WS_PILLAR_QUADS    10      /* 5 per side: 2 column + 2 arch + 1 rim */

/* Registration -> face-writer handoff. mp6_ws_extend_grow()'s writeFaces
 * callback only receives the entry + the buffer tails, and the pillar's 10
 * quads are fully determined during registration (native/duplicate corner
 * indices, per-corner st/normal indices, per-quad source face for
 * mat/nbt) -- staged here, consumed synchronously by the writer inside the
 * same registration call. Slot order is GX storage order (index[0..3]);
 * the emission CYCLE positions were already mapped to slots when this was
 * filled (cycle p -> slot {0,2,3,1}[p]). */
typedef struct {
    s16 vertex[MP6_WS_PILLAR_QUADS][4];
    s16 st[MP6_WS_PILLAR_QUADS][4];
    s16 normal[MP6_WS_PILLAR_QUADS][4];
    s16 srcFace[MP6_WS_PILLAR_QUADS];
} MP6WSPillarScratch;
static MP6WSPillarScratch g_mp6WsPillarScratch;

static BOOL mp6_ws_pillar_near(float a, float b, float eps)
{
    float d = a - b;
    return d < eps && d > -eps;
}

/* TRI/QUAD corner access; returns corner count, 0 for TRISTRIP/other (the
 * caller decides whether a strip is a refusal or a skip). */
static s16 mp6_ws_pillar_face_corners(const HSF_FACE *f)
{
    u16 kind = (u16)(f->typeSrc & HSF_FACE_MASK);
    if (kind == HSF_FACE_TRI) return 3;
    if (kind == HSF_FACE_QUAD) return 4;
    return 0;
}

/* The face's GX emission cycle, as corner-slot indices (game/hsfdraw.c's
 * FaceDraw: TRI emits index[0],[2],[1]; QUAD emits index[0],[2],[3],[1]). */
static s16 mp6_ws_pillar_emission_slot(s16 corners, s16 cyclePos)
{
    static const s16 triOrder[3] = { 0, 2, 1 };
    static const s16 quadOrder[4] = { 0, 2, 3, 1 };
    return (corners == 3) ? triOrder[cyclePos] : quadOrder[cyclePos];
}

/* Is every corner vertex of this face on the given z sheet? (The fill
 * faces of one sheet -- rim walls always bridge two sheets and are
 * excluded by construction, the same per-sheet rule the design module's
 * _vert_uv_on_sheet anchors on.) */
static BOOL mp6_ws_pillar_face_on_sheet(const HSF_FACE *f, s16 corners, const HuVecF *verts, float sheetZ)
{
    s16 c;
    for (c = 0; c < corners; c++) {
        if (!mp6_ws_pillar_near(verts[f->index[c].vertex].z, sheetZ, MP6_WS_PILLAR_Z_EPS)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* The loop (st index / normal index) vertex `vi` carries on the front FILL
 * of one z sheet -- demands every such loop agrees on the ST VALUE within
 * 1e-4 (per-loop UVs are face-dependent; "the edge vert's UV" only means
 * something per sheet). Returns FALSE if no fill loop exists or they
 * disagree. */
static BOOL mp6_ws_pillar_sheet_loop(const HSF_FACE *faces, s32 faceCount,
                                     const HuVecF *verts, const HuVec2f *sts,
                                     s16 vi, float sheetZ,
                                     s16 *outSt, s16 *outNormal, s32 *outFace)
{
    BOOL found = FALSE;
    HuVec2f val;
    s32 fi;
    s16 c;
    for (fi = 0; fi < faceCount; fi++) {
        const HSF_FACE *f = &faces[fi];
        s16 corners = mp6_ws_pillar_face_corners(f);
        if (corners == 0 || !mp6_ws_pillar_face_on_sheet(f, corners, verts, sheetZ)) {
            continue;
        }
        for (c = 0; c < corners; c++) {
            if (f->index[c].vertex != vi) {
                continue;
            }
            if (!found) {
                found = TRUE;
                val = sts[f->index[c].st];
                *outSt = f->index[c].st;
                *outNormal = f->index[c].normal;
                *outFace = fi;
            } else {
                const HuVec2f *v2 = &sts[f->index[c].st];
                if (!mp6_ws_pillar_near(v2->x, val.x, 1e-4f) || !mp6_ws_pillar_near(v2->y, val.y, 1e-4f)) {
                    return FALSE; /* conflicting sheet UVs -- not the measured layout */
                }
            }
        }
    }
    return found;
}

/* Does the native face's emission cycle traverse a->b (TRUE) or b->a
 * (FALSE)? `*found` reports whether the edge appears at all. */
static BOOL mp6_ws_pillar_cycle_dir(const HSF_FACE *f, s16 corners, s16 a, s16 b, BOOL *found)
{
    s16 p;
    for (p = 0; p < corners; p++) {
        s16 v0 = f->index[mp6_ws_pillar_emission_slot(corners, p)].vertex;
        s16 v1 = f->index[mp6_ws_pillar_emission_slot(corners, (s16)((p + 1) % corners))].vertex;
        if (v0 == a && v1 == b) { *found = TRUE; return TRUE; }
        if (v0 == b && v1 == a) { *found = TRUE; return FALSE; }
    }
    *found = FALSE;
    return FALSE;
}

/* Stage one strip quad into the scratch. `cycle*` are the four corners in
 * the desired EMISSION order (each: vertex index + st index + normal
 * index); the mapping to storage slots is the QUAD emission order inverted
 * (cycle position p lands in slot {0,2,3,1}[p]). */
static void mp6_ws_pillar_stage_quad(s32 q, s32 srcFace,
                                     const s16 cycleVtx[4], const s16 cycleSt[4], const s16 cycleNrm[4])
{
    s16 p;
    for (p = 0; p < 4; p++) {
        s16 slot = mp6_ws_pillar_emission_slot(4, p);
        g_mp6WsPillarScratch.vertex[q][slot] = cycleVtx[p];
        g_mp6WsPillarScratch.st[q][slot] = cycleSt[p];
        g_mp6WsPillarScratch.normal[q][slot] = cycleNrm[p];
    }
    g_mp6WsPillarScratch.srcFace[q] = (s16)srcFace;
}

static void mp6_ws_extend_write_faces_pillar(MP6WSExtendEntry *e, HSF_FACE *tail, const HSF_FACE *nativeFaces)
{
    s32 q;
    s16 c;
    (void)e;
    for (q = 0; q < MP6_WS_PILLAR_QUADS; q++) {
        HSF_FACE *f = &tail[q];
        const HSF_FACE *src = &nativeFaces[g_mp6WsPillarScratch.srcFace[q]];
        f->typeSrc = (u16)HSF_FACE_QUAD;
        f->mat = src->mat;
        f->nbt[0] = src->nbt[0];
        f->nbt[1] = src->nbt[1];
        f->nbt[2] = src->nbt[2];
        for (c = 0; c < 4; c++) {
            f->index[c].vertex = g_mp6WsPillarScratch.vertex[q][c];
            f->index[c].st = g_mp6WsPillarScratch.st[q][c];
            f->index[c].normal = g_mp6WsPillarScratch.normal[q][c];
            f->index[c].color = -1; /* no colour buffer on this object -- validated at registration */
        }
    }
}

void mp6_widescreen_extend_pillar_edge(int16_t modelId, const char *objName)
{
    HSF_OBJECT *o = mp6_ws_extend_locate(modelId, objName);
    MP6WSExtendEntry *e;
    float k;

    if (o == NULL) {
        return;
    }
    e = mp6_ws_extend_find(o);
    if (e == NULL) {
        HuVecF *verts = (HuVecF *)o->mesh.vertex->data;
        HuVec2f *sts = (HuVec2f *)o->mesh.st->data;
        HSF_FACE *faces = (HSF_FACE *)o->mesh.face->data;
        s32 faceCount = o->mesh.face->count;
        float halfW = 0.0f;
        s16 edge[2][MP6_WS_PILLAR_EDGE_PER_SIDE]; /* [0]=left, [1]=right */
        s16 edgeNum[2] = { 0, 0 };
        /* Per side: chain vertex indices + their sheet/rim loop data. */
        s16 chainVtx[2][8];      /* col0..2, arch0..2, rimF, rimB */
        s16 chainSt[2][8];
        s16 chainNrm[2][8];
        s32 chainFace[2][8];     /* source face for each loop (fills: sheet fill; rim: rim wall) */
        s32 i, fi;
        s16 side, c, j;

        if (o->mesh.color != NULL && o->mesh.color->data != NULL && o->mesh.color->count > 0) {
            mp6_ws_extend_refuse(objName, "unexpected colour buffer");
            return;
        }
        for (i = 0; i < o->mesh.vertex->count; i++) {
            float ax = verts[i].x < 0.0f ? -verts[i].x : verts[i].x;
            if (ax > halfW) halfW = ax;
        }
        if (!mp6_ws_pillar_near(halfW, MP6_WS_PILLAR_HALF_W, 0.01f)) {
            mp6_ws_extend_refuse(objName, "half width != 800 -- not the measured pillar layout");
            return;
        }
        for (i = 0; i < o->mesh.vertex->count; i++) {
            side = -1;
            if (mp6_ws_pillar_near(-verts[i].x, halfW, MP6_WS_PILLAR_EDGE_EPS)) side = 0;
            else if (mp6_ws_pillar_near(verts[i].x, halfW, MP6_WS_PILLAR_EDGE_EPS)) side = 1;
            if (side < 0) {
                continue;
            }
            if (edgeNum[side] >= MP6_WS_PILLAR_EDGE_PER_SIDE) {
                mp6_ws_extend_refuse(objName, "more than 7 edge verts on one side");
                return;
            }
            edge[side][edgeNum[side]++] = (s16)i;
        }
        if (edgeNum[0] != MP6_WS_PILLAR_EDGE_PER_SIDE || edgeNum[1] != MP6_WS_PILLAR_EDGE_PER_SIDE) {
            mp6_ws_extend_refuse(objName, "edge vert count != 7 per side");
            return;
        }
        /* No TRISTRIP face may touch an edge vert (none does in the real
         * data -- the 6 strips are interior rim walls); the loop scans
         * below only read TRI/QUAD faces, so an edge-touching strip would
         * be an unmodelled loop source. */
        for (fi = 0; fi < faceCount; fi++) {
            if (mp6_ws_pillar_face_corners(&faces[fi]) == 0) {
                s16 sc;
                const HSF_FACE *f = &faces[fi];
                s32 stripN;
                if ((u16)(f->typeSrc & HSF_FACE_MASK) != (u16)HSF_FACE_TRISTRIP) {
                    mp6_ws_extend_refuse(objName, "face of unmodelled kind");
                    return;
                }
                stripN = (s32)f->strip.count;
                for (sc = 0; sc < 3; sc++) {
                    for (side = 0; side < 2; side++) {
                        for (j = 0; j < MP6_WS_PILLAR_EDGE_PER_SIDE; j++) {
                            if (f->strip.index[sc].vertex == edge[side][j]) {
                                mp6_ws_extend_refuse(objName, "TRISTRIP touches an edge vert");
                                return;
                            }
                        }
                    }
                }
                for (i = 0; i < stripN; i++) {
                    for (side = 0; side < 2; side++) {
                        for (j = 0; j < MP6_WS_PILLAR_EDGE_PER_SIDE; j++) {
                            if (f->strip.data[i].vertex == edge[side][j]) {
                                mp6_ws_extend_refuse(objName, "TRISTRIP touches an edge vert");
                                return;
                            }
                        }
                    }
                }
            }
        }
        /* Structure each side's 7 edge verts into the three chains
         * (3/3/1 z census, y-ascending within a chain) and read each
         * chain vert's loop STs off the faces the strips continue. */
        for (side = 0; side < 2; side++) {
            s16 col[3], arch[3], back = -1;
            s16 nCol = 0, nArch = 0, nBack = 0;
            for (j = 0; j < MP6_WS_PILLAR_EDGE_PER_SIDE; j++) {
                s16 vi = edge[side][j];
                float z = verts[vi].z;
                if (mp6_ws_pillar_near(z, MP6_WS_PILLAR_COL_Z, MP6_WS_PILLAR_Z_EPS)) {
                    if (nCol >= 3) { mp6_ws_extend_refuse(objName, "column chain > 3"); return; }
                    col[nCol++] = vi;
                } else if (mp6_ws_pillar_near(z, MP6_WS_PILLAR_ARCH_Z, MP6_WS_PILLAR_Z_EPS)) {
                    if (nArch >= 3) { mp6_ws_extend_refuse(objName, "arch chain > 3"); return; }
                    arch[nArch++] = vi;
                } else if (mp6_ws_pillar_near(z, MP6_WS_PILLAR_BACK_Z, MP6_WS_PILLAR_Z_EPS)) {
                    back = vi;
                    nBack++;
                } else {
                    mp6_ws_extend_refuse(objName, "edge vert on unexpected z sheet");
                    return;
                }
            }
            if (nCol != 3 || nArch != 3 || nBack != 1) {
                mp6_ws_extend_refuse(objName, "edge z census != 3/3/1");
                return;
            }
            /* y-ascending insertion sort (3 elements). */
            for (i = 0; i < 2; i++) {
                for (j = 0; j < 2 - (s16)i; j++) {
                    s16 t;
                    if (verts[col[j]].y > verts[col[j + 1]].y) { t = col[j]; col[j] = col[j + 1]; col[j + 1] = t; }
                    if (verts[arch[j]].y > verts[arch[j + 1]].y) { t = arch[j]; arch[j] = arch[j + 1]; arch[j + 1] = t; }
                }
            }
            /* The arch front corner IS the rim front vert; the rim pair
             * shares its y. */
            if (!mp6_ws_pillar_near(verts[arch[0]].y, verts[back].y, 0.01f)) {
                mp6_ws_extend_refuse(objName, "rim pair y mismatch");
                return;
            }
            for (j = 0; j < 3; j++) {
                chainVtx[side][j] = col[j];
                chainVtx[side][3 + j] = arch[j];
            }
            chainVtx[side][6] = arch[0]; /* rim front (duplicates a second time) */
            chainVtx[side][7] = back;    /* rim back */
            for (j = 0; j < 3; j++) {
                if (!mp6_ws_pillar_sheet_loop(faces, faceCount, verts, sts, col[j],
                                              MP6_WS_PILLAR_COL_Z,
                                              &chainSt[side][j], &chainNrm[side][j], &chainFace[side][j])) {
                    mp6_ws_extend_refuse(objName, "column vert has no agreeing sheet loop");
                    return;
                }
                if (!mp6_ws_pillar_sheet_loop(faces, faceCount, verts, sts, arch[j],
                                              MP6_WS_PILLAR_ARCH_Z,
                                              &chainSt[side][3 + j], &chainNrm[side][3 + j], &chainFace[side][3 + j])) {
                    mp6_ws_extend_refuse(objName, "arch vert has no agreeing sheet loop");
                    return;
                }
            }
            /* The ONE rim-wall face joining the pair (measured: quad
             * [86,72,71,85] on the left, its mirror on the right); its own
             * loops author the rim-lip band UV for both verts. */
            {
                s32 rimFace = -1;
                for (fi = 0; fi < faceCount; fi++) {
                    const HSF_FACE *f = &faces[fi];
                    s16 corners = mp6_ws_pillar_face_corners(f);
                    BOOL hasF = FALSE, hasB = FALSE;
                    if (corners == 0) continue;
                    for (c = 0; c < corners; c++) {
                        if (f->index[c].vertex == chainVtx[side][6]) hasF = TRUE;
                        if (f->index[c].vertex == chainVtx[side][7]) hasB = TRUE;
                    }
                    if (hasF && hasB) {
                        if (rimFace >= 0) { mp6_ws_extend_refuse(objName, "ambiguous rim face"); return; }
                        rimFace = fi;
                    }
                }
                if (rimFace < 0) {
                    mp6_ws_extend_refuse(objName, "no rim face joins the rim pair");
                    return;
                }
                {
                    const HSF_FACE *f = &faces[rimFace];
                    s16 corners = mp6_ws_pillar_face_corners(f);
                    for (c = 0; c < corners; c++) {
                        if (f->index[c].vertex == chainVtx[side][6]) {
                            chainSt[side][6] = f->index[c].st;
                            chainNrm[side][6] = f->index[c].normal;
                            chainFace[side][6] = rimFace;
                        }
                        if (f->index[c].vertex == chainVtx[side][7]) {
                            chainSt[side][7] = f->index[c].st;
                            chainNrm[side][7] = f->index[c].normal;
                            chainFace[side][7] = rimFace;
                        }
                    }
                }
            }
        }
        /* Mirror symmetry: every left edge vert has a right partner at the
         * same (y, z) -- the layout the whole treatment was measured on. */
        for (j = 0; j < MP6_WS_PILLAR_EDGE_PER_SIDE; j++) {
            BOOL matched = FALSE;
            for (i = 0; i < MP6_WS_PILLAR_EDGE_PER_SIDE; i++) {
                if (mp6_ws_pillar_near(verts[edge[0][j]].y, verts[edge[1][i]].y, 0.01f)
                    && mp6_ws_pillar_near(verts[edge[0][j]].z, verts[edge[1][i]].z, MP6_WS_PILLAR_Z_EPS)) {
                    matched = TRUE;
                    break;
                }
            }
            if (!matched) {
                mp6_ws_extend_refuse(objName, "edge verts not mirror-symmetric");
                return;
            }
        }

        e = mp6_ws_extend_alloc();
        if (e == NULL) {
            return; /* registry exhausted -- degrade to "no extension" */
        }
        memset(e, 0, sizeof(*e));
        e->modelId = modelId;
        e->hsf = Hu3DData[modelId].hsf;
        e->obj = o;
        e->mode = MP6_WS_EXTEND_PILLAR;
        e->halfW = halfW;
        e->rimCount = MP6_WS_PILLAR_DUPS;
        e->nativeMin = o->mesh.mesh.min;
        e->nativeMax = o->mesh.mesh.max;
        e->baseVtx = o->mesh.vertex->count;
        e->baseSt = o->mesh.st->count;
        /* Duplicate table: slot side*8+j duplicates chainVtx[side][j]
         * (y/z copied verbatim; x re-derived per tick as sign*halfW*k). */
        for (side = 0; side < 2; side++) {
            for (j = 0; j < 8; j++) {
                s16 slot = (s16)(side * 8 + j);
                s16 vi = chainVtx[side][j];
                e->rimVtx[slot] = vi;
                e->rimX[slot] = (side == 0) ? -1.0f : 1.0f;
                e->rimY[slot] = verts[vi].y;
                e->rimZ[slot] = verts[vi].z;
            }
        }
        /* Stage the 10 strip quads. Emission cycles are derived from the
         * native faces' own traversal of each shared edge (opposite
         * direction = same facing), so both sides reproduce the authored
         * winding without any hand-coded left/right cases. */
        {
            s32 q = 0;
            for (side = 0; side < 2; side++) {
                /* fill strips: column chain (slots 0..2), arch chain (3..5) */
                static const s16 segBase[4] = { 0, 1, 3, 4 };
                for (i = 0; i < 4; i++) {
                    s16 aSlot = segBase[i];
                    s16 bSlot = (s16)(aSlot + 1);
                    s16 a = chainVtx[side][aSlot], b = chainVtx[side][bSlot];
                    s16 da = (s16)(e->baseVtx + side * 8 + aSlot);
                    s16 db = (s16)(e->baseVtx + side * 8 + bSlot);
                    s16 sa = chainSt[side][aSlot], sb = chainSt[side][bSlot];
                    s16 dsa = (s16)(e->baseSt + side * 8 + aSlot);
                    s16 dsb = (s16)(e->baseSt + side * 8 + bSlot);
                    s16 na = chainNrm[side][aSlot], nb = chainNrm[side][bSlot];
                    s32 srcFi = chainFace[side][aSlot];
                    const HSF_FACE *sf = &faces[srcFi];
                    BOOL found = FALSE;
                    BOOL ab = mp6_ws_pillar_cycle_dir(sf, mp6_ws_pillar_face_corners(sf), a, b, &found);
                    s16 cv[4], cs[4], cn[4];
                    if (!found) {
                        /* the loop-source face does not contain the edge --
                         * find the sheet face that does */
                        float sheetZ = (aSlot < 3) ? MP6_WS_PILLAR_COL_Z : MP6_WS_PILLAR_ARCH_Z;
                        s32 fj;
                        for (fj = 0; fj < faceCount && !found; fj++) {
                            const HSF_FACE *f2 = &faces[fj];
                            s16 corners = mp6_ws_pillar_face_corners(f2);
                            if (corners == 0 || !mp6_ws_pillar_face_on_sheet(f2, corners, verts, sheetZ)) {
                                continue;
                            }
                            ab = mp6_ws_pillar_cycle_dir(f2, corners, a, b, &found);
                            if (found) {
                                srcFi = fj;
                            }
                        }
                        if (!found) {
                            mp6_ws_extend_refuse(objName, "no sheet face traverses a chain segment");
                            e->obj = NULL;
                            e->hsf = NULL;
                            return;
                        }
                    }
                    if (ab) { /* native a->b => strip cycle b, a, da, db */
                        cv[0] = b; cv[1] = a; cv[2] = da; cv[3] = db;
                        cs[0] = sb; cs[1] = sa; cs[2] = dsa; cs[3] = dsb;
                        cn[0] = nb; cn[1] = na; cn[2] = na; cn[3] = nb;
                    } else {  /* native b->a => strip cycle a, b, db, da */
                        cv[0] = a; cv[1] = b; cv[2] = db; cv[3] = da;
                        cs[0] = sa; cs[1] = sb; cs[2] = dsb; cs[3] = dsa;
                        cn[0] = na; cn[1] = nb; cn[2] = nb; cn[3] = na;
                    }
                    mp6_ws_pillar_stage_quad(q, srcFi, cv, cs, cn);
                    q++;
                }
                /* rim strip (slots 6/7 = front/back pair) */
                {
                    s16 vf = chainVtx[side][6], vb = chainVtx[side][7];
                    s16 df = (s16)(e->baseVtx + side * 8 + 6);
                    s16 db2 = (s16)(e->baseVtx + side * 8 + 7);
                    s16 sf2 = chainSt[side][6], sb2 = chainSt[side][7];
                    s16 dsf = (s16)(e->baseSt + side * 8 + 6);
                    s16 dsb2 = (s16)(e->baseSt + side * 8 + 7);
                    s16 nf = chainNrm[side][6], nb2 = chainNrm[side][7];
                    s32 srcFi = chainFace[side][6];
                    const HSF_FACE *sfc = &faces[srcFi];
                    BOOL found = FALSE;
                    BOOL fb = mp6_ws_pillar_cycle_dir(sfc, mp6_ws_pillar_face_corners(sfc), vf, vb, &found);
                    s16 cv[4], cs[4], cn[4];
                    if (!found) {
                        mp6_ws_extend_refuse(objName, "rim face does not traverse the rim pair");
                        e->obj = NULL;
                        e->hsf = NULL;
                        return;
                    }
                    if (fb) { /* native vf->vb => strip cycle vb, vf, df, db */
                        cv[0] = vb; cv[1] = vf; cv[2] = df; cv[3] = db2;
                        cs[0] = sb2; cs[1] = sf2; cs[2] = dsf; cs[3] = dsb2;
                        cn[0] = nb2; cn[1] = nf; cn[2] = nf; cn[3] = nb2;
                    } else {  /* native vb->vf => strip cycle vf, vb, db, df */
                        cv[0] = vf; cv[1] = vb; cv[2] = db2; cv[3] = df;
                        cs[0] = sf2; cs[1] = sb2; cs[2] = dsb2; cs[3] = dsf;
                        cn[0] = nf; cn[1] = nb2; cn[2] = nb2; cn[3] = nf;
                    }
                    mp6_ws_pillar_stage_quad(q, srcFi, cv, cs, cn);
                    q++;
                }
            }
        }
        if (!mp6_ws_extend_grow(modelId, o, MP6_WS_PILLAR_DUPS, MP6_WS_PILLAR_DUPS,
                                MP6_WS_PILLAR_QUADS, mp6_ws_extend_write_faces_pillar, e)) {
            e->obj = NULL; /* mark dead: allocation failed, nothing grew */
            e->hsf = NULL;
            return;
        }
        e->vtx = (HuVecF *)o->mesh.vertex->data;
        e->st = (HuVec2f *)o->mesh.st->data;
        /* Pin the 16 appended STs ONCE: each duplicate's ST is a verbatim
         * copy of its source loop's own ST value (the strip's inner column
         * IS the native edge -- positions and UVs bit-identical, C0
         * watertight; the outer column repeats the same value, which is
         * what makes the strip horizontally constant). Written after the
         * grow swapped the ST buffer live, exactly like the tile
         * registrar's own post-grow ST write; never touched again. */
        for (side = 0; side < 2; side++) {
            for (j = 0; j < 8; j++) {
                e->st[e->baseSt + side * 8 + j] = e->st[chainSt[side][j]];
            }
        }
    }
    k = mp6_widescreen_scale_factor();
    mp6_ws_extend_apply(e, k);
    GXInvalidateVtxCache();
}

/* The load-bearing cloud/star reference centre, in closed form. The
 * SHIPPED pipeline widened sora's own verts about the sky model's
 * hierarchical centre (which clouds and stars pull to +702.95) and only
 * then snapshotted model_center_x for the star/cloud repositions -- so the
 * value those repositions actually consume is k-DEPENDENT: +702.95 at
 * k=1, -439.35 at k=1.625, -544.79 at k=1.775. Under the tile treatment
 * sora's verts never widen, so that snapshot would silently change and
 * move every cloud by >1000 units. This reproduces the shipped value
 * exactly, without any phantom mutation:
 *     c0         = model_center_x(native model)          (+702.95)
 *     objBox'    = [c0 + k(objMin-c0), c0 + k(objMax-c0)] (widen about c0)
 *     centerX    = midpoint( union(all OTHER meshes' composed boxes, objBox') )
 * Verified bit-equal against the shipped-order phantom widen on the real
 * mdsel[2] data at k=1, 1.625 and 1.775 (-439.3461 / -544.7892).
 * Collapses to the plain hierarchical
 * centre at k=1 (the (k-1) term vanishes and sora's own box dominates
 * neither side), so a caller may use it unconditionally where it used
 * mp6_widescreen_model_center_x() before. Same walk composition as
 * mp6_ws_accum_bbox_r (PGObjCalc's own transform order), split into
 * "named object" vs "everything else" accumulators. */
typedef struct {
    const char *name;
    HuVecF otherMin, otherMax;
    BOOL anyOther;
    float objMinX, objMaxX;
    BOOL anyObj;
} MP6WSCenterWidenCtx;

static void mp6_ws_center_widened_r(HSF_OBJECT *object, Mtx parentMtx, MP6WSCenterWidenCtx *ctx)
{
    Mtx rotMtx, mtx;
    HSF_TRANSFORM *t;
    u32 i;

    if (object == NULL || object->type == HSF_OBJ_CAMERA || object->type == HSF_OBJ_LIGHT) {
        return;
    }
    t = &object->mesh.base;
    mtxRot(rotMtx, t->rot.x, t->rot.y, t->rot.z);
    PSMTXScale(mtx, t->scale.x, t->scale.y, t->scale.z);
    PSMTXConcat(rotMtx, mtx, mtx);
    mtxTransCat(mtx, t->pos.x, t->pos.y, t->pos.z);
    PSMTXConcat(parentMtx, mtx, mtx);

    if (object->type == HSF_OBJ_MESH && object->mesh.vertex != NULL) {
        BOOL isTarget = (object->name != NULL && strcmp(object->name, ctx->name) == 0);
        HuVecF p;
        int corner;
        for (corner = 0; corner < 2; corner++) {
            PSMTXMultVec(mtx, corner == 0 ? &object->mesh.mesh.min : &object->mesh.mesh.max, &p);
            if (isTarget) {
                if (!ctx->anyObj) {
                    ctx->objMinX = ctx->objMaxX = p.x;
                    ctx->anyObj = TRUE;
                } else {
                    if (p.x < ctx->objMinX) ctx->objMinX = p.x;
                    if (p.x > ctx->objMaxX) ctx->objMaxX = p.x;
                }
            } else {
                if (!ctx->anyOther) {
                    ctx->otherMin = p;
                    ctx->otherMax = p;
                    ctx->anyOther = TRUE;
                } else {
                    if (p.x < ctx->otherMin.x) ctx->otherMin.x = p.x;
                    if (p.y < ctx->otherMin.y) ctx->otherMin.y = p.y;
                    if (p.z < ctx->otherMin.z) ctx->otherMin.z = p.z;
                    if (p.x > ctx->otherMax.x) ctx->otherMax.x = p.x;
                    if (p.y > ctx->otherMax.y) ctx->otherMax.y = p.y;
                    if (p.z > ctx->otherMax.z) ctx->otherMax.z = p.z;
                }
            }
        }
    }
    if (object->type != HSF_OBJ_REPLICA) {
        for (i = 0; i < object->mesh.childNum; i++) {
            mp6_ws_center_widened_r(object->mesh.child[i], mtx, ctx);
        }
    }
}

float mp6_widescreen_model_center_x_widened(int16_t modelId, const char *objName)
{
    HSF_DATA *hsf;
    HuVecF center;
    MP6WSCenterWidenCtx ctx;
    Mtx identity;
    float k, w0, w1, u0, u1;

    if (!mp6_widescreen_enabled() || modelId < 0 || objName == NULL) {
        return 0.0f;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL || !mp6_ws_model_center(hsf, &center)) {
        return 0.0f;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.name = objName;
    PSMTXIdentity(identity);
    mp6_ws_center_widened_r(hsf->root, identity, &ctx);
    if (!ctx.anyObj || !ctx.anyOther) {
        return center.x; /* degenerate model -- the plain centre is all there is */
    }
    k = mp6_widescreen_scale_factor();
    w0 = center.x + k * (ctx.objMinX - center.x);
    w1 = center.x + k * (ctx.objMaxX - center.x);
    u0 = (ctx.otherMin.x < w0) ? ctx.otherMin.x : w0;
    u1 = (ctx.otherMax.x > w1) ? ctx.otherMax.x : w1;
    return (u0 + u1) * 0.5f;
}

/* Per-frame re-apply for the extension registry -- same "flat loop,
 * no re-walk" shape as every other *_reapply_all() here. Only array
 * CONTENTS change (the baked display lists index into them live), so this
 * is pure arithmetic + one vertex-cache invalidate. */
static void mp6_ws_extend_reapply_all(float k)
{
    s16 i;
    BOOL any = FALSE;
    for (i = 0; i < g_mp6WsExtendNum; i++) {
        MP6WSExtendEntry *e = &g_mp6WsExtend[i];
        if (Hu3DData[e->modelId].hsf != e->hsf) {
            continue; /* dead / reused modelId slot -- never resurrect */
        }
        mp6_ws_extend_apply(e, k);
        any = TRUE;
    }
    if (any) {
        GXInvalidateVtxCache();
    }
}

/* ==========================================================================
 * The single per-frame entry point -- called once per tick from
 * platform/gx/aurora_bridge.c's VIWaitForRetrace (the same site the
 * render-width refresh, mp6_widescreen_apply_render_width(), already uses),
 * so a continuous drag-resize converges every frame, not just at the next
 * scene-(re)load event. A true no-op (a for-loop over permanently-empty
 * registries) whenever widescreen is disabled -- see this file's own top
 * comment for the full default-off reasoning.
 * ========================================================================== */
static void mp6_ws_repeat_reapply_all(float k)
{
    s16 i;
    BOOL any = FALSE;
    for (i = 0; i < g_mp6WsRepeatNum; i++) {
        MP6WSRepeatEntry *e = &g_mp6WsRepeat[i];
        if (Hu3DData[e->modelId].hsf != e->hsf) {
            continue; /* dead / reused modelId slot -- never resurrect */
        }
        mp6_ws_repeat_apply(e, k);
        any = TRUE;
    }
    if (any) {
        GXInvalidateVtxCache();
    }
}

static void mp6_ws_camera_reapply_all(float k)
{
    s16 i;
    for (i = 0; i < g_mp6WsCameraNum; i++) {
        MP6WSCameraEntry *e = &g_mp6WsCameras[i];
        int idx = mp6_ws_camera_bit_index(e->bit);
        if (idx < 0 || idx >= HU3D_CAM_MAX || Hu3DCamera[idx].fov == -1.0f) {
            continue; /* camera bit killed by its own scene's teardown -- never resurrect */
        }
        mp6_ws_apply_camera(e, k);
    }
}

/* Per-frame re-apply for the two selective/border registries above --
 * same "flat loop over the already-flattened registry, no re-walk"
 * shape as mp6_ws_repeat_reapply_all(). */
static void mp6_ws_selective_reapply_all(float k)
{
    s16 i;
    BOOL any = FALSE;
    for (i = 0; i < g_mp6WsSelectiveNum; i++) {
        MP6WSSelectiveEntry *e = &g_mp6WsSelective[i];
        if (Hu3DData[e->modelId].hsf != e->hsf) {
            continue; /* dead / reused modelId slot -- never resurrect */
        }
        mp6_ws_selective_apply(e, k);
        any = TRUE;
    }
    if (any) {
        GXInvalidateVtxCache();
    }
}

static void mp6_ws_border_reapply_all(float k)
{
    s16 i;
    BOOL any = FALSE;
    for (i = 0; i < g_mp6WsBorderNum; i++) {
        MP6WSBorderEntry *e = &g_mp6WsBorder[i];
        if (Hu3DData[e->modelId].hsf != e->hsf) {
            continue; /* dead / reused modelId slot -- never resurrect */
        }
        mp6_ws_border_apply(e, k);
        any = TRUE;
    }
    if (any) {
        GXInvalidateVtxCache();
    }
}

/* Per-frame re-apply for the reposition-only registry above -- same
 * "flat loop over the already-flattened registry, no re-walk" shape as
 * every other *_reapply_all() in this file. */
static void mp6_ws_reposition_reapply_all(float k)
{
    s16 i;
    for (i = 0; i < g_mp6WsRepositionNum; i++) {
        MP6WSRepositionEntry *e = &g_mp6WsReposition[i];
        if (Hu3DData[e->modelId].hsf != e->hsf) {
            continue; /* dead / reused modelId slot -- never resurrect */
        }
        mp6_ws_reposition_apply(e, k);
        /* No GXInvalidateVtxCache() here -- unlike every other mechanism in
         * this file, this one writes a plain mesh.base.pos.x struct field,
         * never vertex-array memory, so there is no GX vertex cache to
         * invalidate. */
    }
}

/* Per-frame re-apply for the split-separate registry above -- same
 * "flat loop over the already-flattened registry, no re-walk" shape as
 * every other *_reapply_all() in this file. */
static void mp6_ws_split_reapply_all(float k)
{
    s16 i;
    BOOL any = FALSE;
    for (i = 0; i < g_mp6WsSplitNum; i++) {
        MP6WSSplitEntry *e = &g_mp6WsSplit[i];
        if (Hu3DData[e->modelId].hsf != e->hsf) {
            continue; /* dead / reused modelId slot -- never resurrect */
        }
        mp6_ws_split_apply(e, k);
        any = TRUE;
    }
    if (any) {
        GXInvalidateVtxCache();
    }
}

void mp6_widescreen_reapply(void)
{
    float k;
    s16 i;

    if (!mp6_widescreen_enabled()) {
        return;
    }
    k = mp6_widescreen_scale_factor();

    for (i = 0; i < g_mp6WsModelNum; i++) {
        MP6WSModelEntry *e = &g_mp6WsModels[i];
        if (Hu3DData[e->modelId].hsf != e->hsf) {
            continue; /* dead / reused modelId slot -- never resurrect */
        }
        mp6_ws_apply_model_from_native(e, k);
    }

    mp6_ws_repeat_reapply_all(k);
    mp6_ws_camera_reapply_all(k);
    mp6_ws_selective_reapply_all(k);
    mp6_ws_border_reapply_all(k);
    mp6_ws_split_reapply_all(k);
    mp6_ws_reposition_reapply_all(k);
    mp6_ws_extend_reapply_all(k); /* tile/mirror/ring companions */
}

void mp6_widescreen_debug_dump_model(HU3D_MODELID modelId, const char *label)
{
    HSF_DATA *hsf;
    HuVecF topPos, topRot, topScale, hierCenter;
    BOOL hasHierCenter;
    s16 i;

    if (!getenv("MP6_WS_HSF_DIAG")) {
        return;
    }
    if (modelId < 0) {
        printf("[MP6-WS-DIAG] %s: modelId=%d (invalid)\n", label, modelId);
        fflush(stdout);
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        printf("[MP6-WS-DIAG] %s: modelId=%d hsf=NULL\n", label, modelId);
        fflush(stdout);
        return;
    }
    Hu3DModelPosGet(modelId, &topPos);
    Hu3DModelRotGet(modelId, &topRot);
    Hu3DModelScaleGet(modelId, &topScale);
    hasHierCenter = mp6_ws_model_center(hsf, &hierCenter);
    printf("[MP6-WS-DIAG] %s: modelId=%d objectNum=%d attributeNum=%d "
           "topPos=(%.2f,%.2f,%.2f) topRot=(%.2f,%.2f,%.2f) topScale=(%.2f,%.2f,%.2f) "
           "hierCenter=(%.2f,%.2f,%.2f)\n",
           label, modelId, hsf->objectNum, hsf->attributeNum,
           topPos.x, topPos.y, topPos.z, topRot.x, topRot.y, topRot.z,
           topScale.x, topScale.y, topScale.z,
           hasHierCenter ? hierCenter.x : 0.0f, hasHierCenter ? hierCenter.y : 0.0f,
           hasHierCenter ? hierCenter.z : 0.0f);
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *o = &hsf->object[i];
        if (o->type != HSF_OBJ_MESH) {
            continue;
        }
        /* Includes vtxBuf=/vtxData=/childNum= in this reusable diagnostic --
         * a raw pointer
         * identity check across every object in a census, needed to confirm
         * or rule out two DIFFERENT-named sub-objects (e.g. a "grid*"
         * background decoration and a "chara*" foreground portrait)
         * referencing the SAME shared HSF vertex-pool entry (the HSF file
         * format stores vertex arrays in one pool, `ctx->vertex[]`, indexed
         * by a per-object `vertexId` -- platform/hsf/hsf_load_native.c's
         * LoadVertexArrays()/`out[i].mesh.vertex = &ctx->vertex[vertexId]` --
         * so nothing in the format itself guarantees each mesh object owns
         * an exclusive buffer). Mutating a SHARED buffer in place (every
         * mechanism in this file does this) would silently stretch every
         * OTHER object referencing that same pool entry too, regardless of
         * whether that other object's own name matched the caller's
         * predicate -- a real risk this print makes directly checkable
         * instead of assumed away. */
        printf("[MP6-WS-DIAG]   obj[%d] name=%s vtx=%d vtxBuf=%p vtxData=%p childNum=%u min=(%.2f,%.2f,%.2f) max=(%.2f,%.2f,%.2f) "
               "basePos=(%.2f,%.2f,%.2f) baseRot=(%.2f,%.2f,%.2f) baseScale=(%.2f,%.2f,%.2f) "
               "ownAttr wrapS=%d wrapT=%d bitmap=%s\n",
               i, o->name ? o->name : "(null)",
               o->mesh.vertex ? o->mesh.vertex->count : -1,
               (void *)o->mesh.vertex,
               (void *)(o->mesh.vertex ? o->mesh.vertex->data : NULL),
               (unsigned)o->mesh.childNum,
               o->mesh.mesh.min.x, o->mesh.mesh.min.y, o->mesh.mesh.min.z,
               o->mesh.mesh.max.x, o->mesh.mesh.max.y, o->mesh.mesh.max.z,
               o->mesh.base.pos.x, o->mesh.base.pos.y, o->mesh.base.pos.z,
               o->mesh.base.rot.x, o->mesh.base.rot.y, o->mesh.base.rot.z,
               o->mesh.base.scale.x, o->mesh.base.scale.y, o->mesh.base.scale.z,
               o->mesh.attribute ? (int)o->mesh.attribute->wrapS : -1,
               o->mesh.attribute ? (int)o->mesh.attribute->wrapT : -1,
               (o->mesh.attribute && o->mesh.attribute->bitmap && o->mesh.attribute->bitmap->name)
                   ? o->mesh.attribute->bitmap->name : "(null)");
    }
    for (i = 0; i < hsf->attributeNum; i++) {
        HSF_ATTRIBUTE *a = &hsf->attribute[i];
        printf("[MP6-WS-DIAG]   attr[%d] name=%s wrapS=%u wrapT=%u bitmap=%s\n",
               i, a->name ? a->name : "(null)",
               (unsigned)a->wrapS, (unsigned)a->wrapT,
               a->bitmap && a->bitmap->name ? a->bitmap->name : "(null)");
    }
    fflush(stdout);
}

/* TEMPORARY diagnostic (removed before final commit): raw vertex/ST
 * dump + material attribute-list dump for ONE named sub-object of a model --
 * mp6_widescreen_debug_dump_model() above reports only min/max/base transform
 * per object, not enough to (a) understand a mesh's own internal vertex-grid
 * layout (needed to design a border-only extend) or (b) see whether an
 * object's MATERIAL references more than one texture attribute (multi-
 * texture/TEV blend passes aren't visible via mesh.attribute alone, which is
 * only the first/primary attribute). Gated on the same MP6_WS_HSF_DIAG env
 * var, zero output/cost otherwise. */
void mp6_widescreen_debug_dump_verts(HU3D_MODELID modelId, const char *objName, const char *label)
{
    HSF_DATA *hsf;
    s16 i;
    s32 j, printN;

    if (!getenv("MP6_WS_HSF_DIAG")) {
        return;
    }
    if (modelId < 0 || Hu3DData[modelId].hsf == NULL) {
        printf("[MP6-WS-VERTS] %s/%s: invalid model\n", label, objName);
        fflush(stdout);
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    for (i = 0; i < hsf->objectNum; i++) {
        HSF_OBJECT *o = &hsf->object[i];
        HuVecF *v;
        HuVec2f *st;
        if (o->type != HSF_OBJ_MESH || o->name == NULL || strcmp(o->name, objName) != 0) {
            continue;
        }
        printf("[MP6-WS-VERTS] %s/%s: obj[%d] vtxCount=%d stCount=%d\n",
               label, objName, i, o->mesh.vertex ? o->mesh.vertex->count : -1,
               o->mesh.st ? o->mesh.st->count : -1);
        if (o->mesh.material) {
            printf("[MP6-WS-VERTS]   material name=%s vtxMode=%u pass=%u attrNum=%u attrs=[",
                   o->mesh.material->name ? o->mesh.material->name : "(null)",
                   (unsigned)o->mesh.material->vtxMode, (unsigned)o->mesh.material->pass,
                   (unsigned)o->mesh.material->attrNum);
            for (j = 0; j < (s32)o->mesh.material->attrNum; j++) {
                s32 idx = o->mesh.material->attr[j];
                const char *bmp = (idx >= 0 && idx < hsf->attributeNum && hsf->attribute[idx].bitmap)
                    ? hsf->attribute[idx].bitmap->name : "(?)";
                printf("%d:%s ", idx, bmp);
            }
            printf("] (NOTE: mesh.material is a WHOLE-ARRAY pointer -- ctx->material,\n"
                   "[MP6-WS-VERTS]   always material[0] regardless of THIS object -- see REAL below)\n");
        } else {
            printf("[MP6-WS-VERTS]   material=NULL\n");
        }
        /* The REAL per-object material, resolved the same way
         * game/hsfdraw.c's own FaceDraw() does it --
         * `objPtr->mesh.material[face->mat & 0xFFF]` -- i.e. THIS object's
         * first face's own `.mat` index into the (whole-array) material
         * pointer above, not mesh.material itself (which is always index 0,
         * confirmed a whole-array-pointer artifact, not a per-object field --
         * see hsf_load_native.c's own LoadObjects() comment). This is the
         * only accurate way to answer "what texture does THIS specific
         * sub-object actually show". */
        if (o->mesh.face != NULL && o->mesh.face->data != NULL && o->mesh.face->count > 0 && o->mesh.material != NULL) {
            HSF_FACE *faces = (HSF_FACE *)o->mesh.face->data;
            s16 realMatIdx = faces[0].mat & 0xFFF;
            HSF_MATERIAL *realMat = &o->mesh.material[realMatIdx];
            printf("[MP6-WS-VERTS]   REAL(per-face) material[%d] name=%s attrNum=%u attrs=[",
                   (int)realMatIdx, realMat->name ? realMat->name : "(null)", (unsigned)realMat->attrNum);
            for (j = 0; j < (s32)realMat->attrNum; j++) {
                s32 idx = realMat->attr[j];
                const char *bmp = (idx >= 0 && idx < hsf->attributeNum && hsf->attribute[idx].bitmap)
                    ? hsf->attribute[idx].bitmap->name : "(?)";
                printf("%d:%s ", idx, bmp);
            }
            printf("]\n");
        } else {
            printf("[MP6-WS-VERTS]   REAL(per-face) material: no face/material data\n");
        }
        if (o->mesh.vertex && o->mesh.vertex->data) {
            v = (HuVecF *)o->mesh.vertex->data;
            printN = o->mesh.vertex->count;
            if (printN > 80) printN = 80;
            printf("[MP6-WS-VERTS]   verts(x,y,z):");
            for (j = 0; j < printN; j++) {
                printf(" [%d](%.1f,%.1f,%.1f)", j, v[j].x, v[j].y, v[j].z);
            }
            printf("\n");
        }
        if (o->mesh.st && o->mesh.st->data) {
            st = (HuVec2f *)o->mesh.st->data;
            printN = o->mesh.st->count;
            if (printN > 80) printN = 80;
            printf("[MP6-WS-VERTS]   st(s,t):");
            for (j = 0; j < printN; j++) {
                printf(" [%d](%.3f,%.3f)", j, st[j].x, st[j].y);
            }
            printf("\n");
        }
        if (o->mesh.face) {
            printf("[MP6-WS-VERTS]   faceBufCount=%d\n", o->mesh.face->count);
        }
        fflush(stdout);
        return;
    }
    printf("[MP6-WS-VERTS] %s/%s: object not found (objectNum=%d)\n", label, objName, hsf->objectNum);
    fflush(stdout);
}

/* Per-object COMPOSED world/model-space bbox dump -- mp6_widescreen_debug_dump_model() above
 * only prints each object's own RAW `mesh.base.pos/rot/scale` (its
 * placement relative to its own immediate PARENT), which reads as (0,0,0)
 * for a mesh authored at local origin under a separate, non-mesh placement
 * joint/null (confirmed for real by this investigation: mdpartydll's props
 * model, DATA_mdparty+2, has its "kinoko"/"kinoko1..3"/"saku"/"sou" mesh
 * objects all report basePos=(0,0,0) despite visually standing at
 * different corners of the party board -- each is a child of its OWN
 * placement joint, not a direct child of hsf->root). This walks the
 * hierarchy exactly like mp6_ws_accum_bbox_r() above (same mtxRot +
 * PSMTXScale + PSMTXConcat + mtxTransCat, then parent-concat, starting from
 * an IDENTITY matrix at hsf->root) but, instead of accumulating one global
 * min/max, prints each individual HSF_OBJ_MESH leaf's own FULLY-COMPOSED
 * (root-to-leaf) world-space bbox -- transforming all 8 corners of its raw
 * local min/max (not just the two corner points) so a rotated placement
 * joint is handled correctly, matching the same rigor
 * mp6_ws_selective_apply() already uses for a rotated sub-object's own
 * min/max. Gated on MP6_WS_HSF_DIAG, zero cost otherwise -- a permanent,
 * reusable diagnostic (matching mp6_widescreen_debug_dump_model()/_verts()'s
 * own established precedent), even though any one investigation's own
 * temporary CALL SITE is removed again afterward. */
static void mp6_ws_dump_world_bbox_r(HSF_OBJECT *object, Mtx parentMtx, const char *label)
{
    Mtx rotMtx, mtx;
    HSF_TRANSFORM *t;
    u32 i;

    if (object == NULL || object->type == HSF_OBJ_CAMERA || object->type == HSF_OBJ_LIGHT) {
        return;
    }

    t = &object->mesh.base;
    mtxRot(rotMtx, t->rot.x, t->rot.y, t->rot.z);
    PSMTXScale(mtx, t->scale.x, t->scale.y, t->scale.z);
    PSMTXConcat(rotMtx, mtx, mtx);
    mtxTransCat(mtx, t->pos.x, t->pos.y, t->pos.z);
    PSMTXConcat(parentMtx, mtx, mtx); /* mtx: this object's own model-local accumulated transform */

    if (object->type == HSF_OBJ_MESH && object->mesh.vertex != NULL) {
        HuVecF *mn = &object->mesh.mesh.min;
        HuVecF *mx = &object->mesh.mesh.max;
        HuVecF corner, p, wmin, wmax;
        int c;
        for (c = 0; c < 8; c++) {
            corner.x = (c & 1) ? mx->x : mn->x;
            corner.y = (c & 2) ? mx->y : mn->y;
            corner.z = (c & 4) ? mx->z : mn->z;
            PSMTXMultVec(mtx, &corner, &p);
            if (c == 0) {
                wmin = wmax = p;
            } else {
                if (p.x < wmin.x) wmin.x = p.x;
                if (p.y < wmin.y) wmin.y = p.y;
                if (p.z < wmin.z) wmin.z = p.z;
                if (p.x > wmax.x) wmax.x = p.x;
                if (p.y > wmax.y) wmax.y = p.y;
                if (p.z > wmax.z) wmax.z = p.z;
            }
        }
        printf("[MP6-WS-WBBOX] %s: obj name=%s worldMin=(%.2f,%.2f,%.2f) worldMax=(%.2f,%.2f,%.2f)\n",
               label, object->name ? object->name : "(null)",
               wmin.x, wmin.y, wmin.z, wmax.x, wmax.y, wmax.z);
        fflush(stdout);
    }
    if (object->type != HSF_OBJ_REPLICA) {
        for (i = 0; i < object->mesh.childNum; i++) {
            mp6_ws_dump_world_bbox_r(object->mesh.child[i], mtx, label);
        }
    }
}

void mp6_widescreen_debug_dump_world_bbox(HU3D_MODELID modelId, const char *label)
{
    HSF_DATA *hsf;
    Mtx identity;

    if (!getenv("MP6_WS_HSF_DIAG")) {
        return;
    }
    if (modelId < 0) {
        printf("[MP6-WS-WBBOX] %s: modelId=%d (invalid)\n", label, modelId);
        fflush(stdout);
        return;
    }
    hsf = Hu3DData[modelId].hsf;
    if (hsf == NULL) {
        printf("[MP6-WS-WBBOX] %s: modelId=%d hsf=NULL\n", label, modelId);
        fflush(stdout);
        return;
    }
    PSMTXIdentity(identity);
    mp6_ws_dump_world_bbox_r(hsf->root, identity, label);
}

/* =======================================================================
 * Savestate rehydrate
 * =======================================================================
 * Every registry in this file is HALF game state and HALF host state, which
 * is exactly why this TU is deliberately NOT carved out of the savestate:
 *   - GAME half (`ptr`, `liveMin`/`liveMax`, `hsf`, `modelId`, the cached
 *     native bbox/center scalars): arena addresses and model ids describing
 *     extrusions ALREADY APPLIED to restored arena geometry. These must be
 *     restored, or the arena comes back permanently extruded with an empty
 *     registry and nothing left to re-derive it from.
 *   - HOST half (`native`): a C-runtime-heap snapshot of the pristine
 *     pre-extrude vertices. Not captured, not at a reproducible address.
 *
 * The registries' own liveness guards cannot detect the problem, which is
 * what makes this dangerous rather than merely broken: they test
 * `entry.ptr == ptr && Hu3DData[id].hsf == entry.hsf`, and all three of
 * those ARE restored and DO match. So the registrar takes its "already
 * tracked" path, never re-snapshots, and the per-frame apply reads through
 * a foreign heap pointer -- plus free() would later be handed a pointer this
 * process's allocator never issued, and the repeat registry's reuse branch
 * would memcpy through it.
 *
 * Nulling `native` fixes all three at once by making the EXISTING
 * re-snapshot path do the work: free(NULL) is a no-op, the reuse branch
 * sees NULL and takes the fresh-malloc else-branch, and each *_apply() now
 * early-outs until the registrar has re-taken the snapshot from restored
 * arena data. No new mechanism, no change to the extrusion math.
 *
 * NOTE this is unobservable in the headless build (mp6_widescreen_enabled()
 * hard-returns 0 there), so it is windowed-only to test -- and the enabled
 * flag itself lives in a carved-out TU, meaning a restore correctly reflects
 * the RESTORING process's widescreen setting, not the captured one. */
/* C4 (review) REPLACED the original null-and-let-the-registrar-resnapshot
 * rehydrate, which was wrong twice over:
 *  1. The registrars only re-snapshot when find() fails or the hsf pointer
 *     differs -- and after a restore, ptr/hsf/modelId are all restored arena
 *     values that still MATCH, so the snapshot was never re-taken mid-scene
 *     and every vertex registry early-outed until full scene re-entry
 *     (live-resize extrusion silently dead, geometry frozen at captured k).
 *  2. Even forcing a re-snapshot would be wrong: the restored arena holds
 *     EXTRUDED vertices (that is what capture captured), so re-snapshotting
 *     records extruded-as-native and every later apply extrudes on top --
 *     double extrusion. The pristine data exists only in the capturing
 *     process's native buffers.
 * So the native buffers are game state and must travel IN the state file.
 * These four functions are that: serialize every live native snapshot at
 * capture, free this process's own snapshots before the image restore
 * overwrites the pointers (S5 -- otherwise they leak on every load), and
 * rebuild each entry's native from the blob afterwards.
 *
 * Blob layout, all little-endian u32 (same-binary contract as the rest of
 * the format): [entryCount] then per entry [registryId][slotIndex]
 * [floatCount][floatCount * f32]. Registry ids: 0=repeat 1=selective
 * 2=border 3=split. */

static uint32_t mp6_ws_entry_float_count(int registry, int slot)
{
    switch (registry) {
    case 0: return (uint32_t)g_mp6WsRepeat[slot].count * (uint32_t)g_mp6WsRepeat[slot].stride;
    case 1: return (uint32_t)g_mp6WsSelective[slot].count * 3u;
    case 2: return (uint32_t)g_mp6WsBorder[slot].count * 3u;
    case 3: return (uint32_t)g_mp6WsSplit[slot].count * 3u;
    default: return 0;
    }
}

static float *mp6_ws_entry_native(int registry, int slot)
{
    switch (registry) {
    case 0: return g_mp6WsRepeat[slot].native;
    case 1: return (float *)g_mp6WsSelective[slot].native;
    case 2: return (float *)g_mp6WsBorder[slot].native;
    case 3: return (float *)g_mp6WsSplit[slot].native;
    default: return NULL;
    }
}

static void mp6_ws_entry_set_native(int registry, int slot, float *p)
{
    switch (registry) {
    case 0: g_mp6WsRepeat[slot].native = p; break;
    case 1: g_mp6WsSelective[slot].native = (void *)p; break;
    case 2: g_mp6WsBorder[slot].native = (void *)p; break;
    case 3: g_mp6WsSplit[slot].native = (void *)p; break;
    default: break;
    }
}

static int mp6_ws_registry_num(int registry)
{
    switch (registry) {
    case 0: return g_mp6WsRepeatNum;
    case 1: return g_mp6WsSelectiveNum;
    case 2: return g_mp6WsBorderNum;
    case 3: return g_mp6WsSplitNum;
    default: return 0;
    }
}

size_t mp6_widescreen_savestate_blob_size(void)
{
    size_t total = sizeof(uint32_t);
    int r, i;
    for (r = 0; r < 4; r++) {
        for (i = 0; i < mp6_ws_registry_num(r); i++) {
            if (mp6_ws_entry_native(r, i) != NULL) {
                total += 3 * sizeof(uint32_t) + (size_t)mp6_ws_entry_float_count(r, i) * sizeof(float);
            }
        }
    }
    return total;
}

void mp6_widescreen_savestate_blob_write(void *buf)
{
    unsigned char *p = (unsigned char *)buf;
    uint32_t n = 0;
    int r, i;
    unsigned char *countAt = p;
    p += sizeof(uint32_t);
    for (r = 0; r < 4; r++) {
        for (i = 0; i < mp6_ws_registry_num(r); i++) {
            float *nat = mp6_ws_entry_native(r, i);
            uint32_t fc = mp6_ws_entry_float_count(r, i);
            uint32_t hdr3[3];
            if (nat == NULL) {
                continue;
            }
            hdr3[0] = (uint32_t)r;
            hdr3[1] = (uint32_t)i;
            hdr3[2] = fc;
            memcpy(p, hdr3, sizeof(hdr3));
            p += sizeof(hdr3);
            memcpy(p, nat, (size_t)fc * sizeof(float));
            p += (size_t)fc * sizeof(float);
            n++;
        }
    }
    memcpy(countAt, &n, sizeof(n));
}

void mp6_widescreen_savestate_prerestore(void)
{
    /* Free THIS process's own snapshots before the image restore overwrites
     * the registry entries with the capturing process's pointers -- without
     * this, every load leaks the live scene's full snapshot set (S5), and a
     * long repro-grinding session walks into the RSS watchdog. */
    int r, i;
    for (r = 0; r < 4; r++) {
        for (i = 0; i < mp6_ws_registry_num(r); i++) {
            float *nat = mp6_ws_entry_native(r, i);
            free(nat);
            mp6_ws_entry_set_native(r, i, NULL);
        }
    }
}

void mp6_widescreen_savestate_apply_natives(const void *blob, size_t blobSize)
{
    const unsigned char *p = (const unsigned char *)blob;
    const unsigned char *end = p + blobSize;
    uint32_t n, k;

    /* The image restore just installed the CAPTURING process's native
     * pointers into every entry -- they are foreign heap addresses and must
     * not survive regardless of what the blob contains. Null them all
     * first, so an entry the blob does not cover (or fails validation)
     * degrades to the apply early-out (frozen, not corrupt). */
    {
        int r, i;
        for (r = 0; r < 4; r++) {
            for (i = 0; i < mp6_ws_registry_num(r); i++) {
                mp6_ws_entry_set_native(r, i, NULL);
            }
        }
    }

    if (blob == NULL || blobSize < sizeof(uint32_t)) {
        return;
    }
    memcpy(&n, p, sizeof(n));
    p += sizeof(n);
    for (k = 0; k < n; k++) {
        uint32_t hdr3[3];
        uint32_t fc, expect;
        float *fresh;
        if ((size_t)(end - p) < sizeof(hdr3)) {
            fprintf(stderr, "[SAVESTATE] widescreen blob truncated at entry %u -- remaining entries skipped\n", k);
            return;
        }
        memcpy(hdr3, p, sizeof(hdr3));
        p += sizeof(hdr3);
        fc = hdr3[2];
        if ((size_t)(end - p) < (size_t)fc * sizeof(float)) {
            fprintf(stderr, "[SAVESTATE] widescreen blob truncated in entry %u -- skipped\n", k);
            return;
        }
        if (hdr3[0] >= 4 || (int)hdr3[1] >= mp6_ws_registry_num((int)hdr3[0])) {
            p += (size_t)fc * sizeof(float);
            continue; /* registry slot the restored image does not have -- skip */
        }
        /* The restored entry's own counts are the source of truth for how
         * big its native snapshot must be -- a mismatch means the blob and
         * the image disagree, and installing it would read/write out of
         * bounds in the per-frame apply. */
        expect = mp6_ws_entry_float_count((int)hdr3[0], (int)hdr3[1]);
        if (fc != expect || fc == 0) {
            fprintf(stderr, "[SAVESTATE] widescreen blob entry %u size mismatch (%u vs %u) -- skipped\n",
                    k, fc, expect);
            p += (size_t)fc * sizeof(float);
            continue;
        }
        fresh = (float *)malloc((size_t)fc * sizeof(float));
        if (fresh == NULL) {
            p += (size_t)fc * sizeof(float);
            continue; /* entry stays NULL -> apply early-out, frozen not corrupt */
        }
        memcpy(fresh, p, (size_t)fc * sizeof(float));
        p += (size_t)fc * sizeof(float);
        mp6_ws_entry_set_native((int)hdr3[0], (int)hdr3[1], fresh);
    }
}
