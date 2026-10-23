/* Included inside the port's hsfdraw adaptation, after its private declarations.
 * Reuses ONLY the original material/mesh submission, never model/event hooks.
 * The normal EFB draw remains byte-for-byte unchanged. */
#include "mp6_ambient_occlusion.h"
#include "mp6_ao_foreground_policy.h"
#include "mp6_enhancements.h"

#ifndef MP6_HEADLESS_BUILD
/* Aurora extension; this adaptation uses the decomp's Dolphin headers. */
extern void GX2SetPolygonOffset(f32 frontOffset, f32 frontScale,
                                f32 backOffset, f32 backScale, f32 clamp);
#define MP6_AO_FOLIAGE_MAX 2048
typedef struct {
    HU3D_DRAW_OBJ draw;
    HSF_FACE *face;
    int batch, camera, water, background, unshaded, character;
} Mp6AoFoliageDraw;
static Mp6AoFoliageDraw mp6AoFoliage[MP6_AO_FOLIAGE_MAX];
static int mp6AoFoliageCount, mp6AoFoliageDrawing;
static int mp6AoWaterDrawing, mp6AoBackgroundDrawing;
static long mp6AoFoliageTick=-1;

static void mp6AoFoliageRemember(HU3D_DRAW_OBJ *draw, HSF_FACE *face, int batch)
{
    if (mp6AoFoliageDrawing || shadowModelDrawF || !mp6_enh_ambient_occlusion()) return;
    if (mp6AoFoliageTick!=mp6_tick_count) {
        mp6AoFoliageTick=mp6_tick_count; mp6AoFoliageCount=0;
    }
    /* Includes event props such as DK's cp37 tree, on every board. Exact W01
     * sky identities are protected too: replay only their private mask
     * coverage, while ordinary background geometry remains untouched. */
    int water=mp6_ao_water_surface(draw);
    int background=mp6_ground_is_background(draw->model,draw->object);
    /* Exact authored trunk receivers retain their original RGB without
     * erasing scene depth: they still occlude AO on other surfaces. */
    int unshaded=mp6_ground_is_ao_unshaded(draw->model,draw->object);
    /* Character models: only CharModelCreate sets a motion timing hook
     * (players, hosts, CPUs). Board terrain can be envelope-skinned too.
     * Toon-shaded faces under AO read as dirt at phone resolution. Like the
     * sky panels, they replay only alpha coverage (no textures or RGB), which
     * excludes them from AO; their depth still shades the ground. */
    static int characters=-1; /* MP6_AO_CHARACTERS=0: shade them (comparison runs) */
    if (characters<0) { const char *e=getenv("MP6_AO_CHARACTERS"); characters=!(e && e[0]=='0'); }
    int character=characters && !water && !unshaded && !background && draw->model->timingHook &&
        !(draw->model->attr & (HU3D_ATTR_HOOKFUNC|HU3D_ATTR_COLOR_NOUPDATE|HU3D_ATTR_REFLECT_MODEL|
                               HU3D_ATTR_ZWRITE_OFF|HU3D_ATTR_ZCMP_OFF));
    int protect=water || background || unshaded || character;
    if (!protect && !mp6_ao_foreground_eligible(draw,face)) return;
    if (mp6AoFoliageCount<MP6_AO_FOLIAGE_MAX)
        mp6AoFoliage[mp6AoFoliageCount++]=(Mp6AoFoliageDraw){*draw,face,batch,Hu3DCameraNo,water,
                                                             background || character,unshaded,character};
}

static void mp6AoFoliageMaskState(void)
{
    if (!mp6AoFoliageDrawing) return;
    /* Keep original TEV color AND alpha, UV animation, filtering and cull for
     * ordinary foliage and registered unshaded trunks. Test against final scene depth (read-only attachment,
     * or a private seed on compatibility paths), so foreground actors reject
     * background grass without changing the game's depth buffer. Depth writes
     * MUST stay off. Water and exact sky panels instead need unconditional
     * private alpha coverage: their RGB is intentionally omitted because alpha
     * excludes them from AO. */
    GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_NOOP);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE);
    if (mp6AoWaterDrawing || mp6AoBackgroundDrawing) {
        /* mizu3 is a continuous water surface and the exact W01 sky panels
         * are background AO exclusions. Their authored alpha is not a mask:
         * mark the full mesh footprint, still rejected by final opaque scene
         * depth. This only changes the private mask, never the game's draw. */
        GXSetBlendMode(GX_BM_BLEND,GX_BL_ONE,GX_BL_ZERO,GX_LO_NOOP);
        GXSetColorUpdate(GX_FALSE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);
        GXSetTevColorIn(GX_TEVSTAGE0,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO);
        GXSetTevColorOp(GX_TEVSTAGE0,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE0,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_KONST);
        GXSetTevKAlphaSel(GX_TEVSTAGE0,GX_TEV_KASEL_1);
        GXSetTevAlphaOp(GX_TEVSTAGE0,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
        GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    }
}

static void SetHiliteTexMtx(HU3D_DRAW_OBJ *drawObj);
static void mp6AoFoliageMatrices(HU3D_DRAW_OBJ *draw)
{
    /* Same per-object texture transforms as ObjDraw/Hu3DDrawPost. The old
     * black coverage pass did not need these RGB-only shadow/projector inputs. */
    Mtx normal, invCamera, world, projected;
    GXLoadPosMtxImm(draw->matrix,GX_PNMTX0);
    PSMTXInvXpose(draw->matrix,normal);
    GXLoadNrmMtxImm(normal,0);
    if ((Hu3DShadowF && shadowNum && (Hu3DObjInfoP->attr&HU3D_CONST_SHADOW_MAP)) || draw->model->projBit) {
        PSMTXInverse(Hu3DCameraMtx,invCamera);
        PSMTXConcat(invCamera,draw->matrix,world);
        if (Hu3DShadowF && shadowNum && (Hu3DObjInfoP->attr&HU3D_CONST_SHADOW_MAP)) {
            PSMTXConcat(Hu3DShadow->projMtx,Hu3DShadow->lookAtMtx,projected);
            PSMTXConcat(projected,world,projected);
            GXLoadTexMtxImm(projected,GX_TEXMTX9,GX_MTX3x4);
        }
        for (int i=0;i<HU3D_PROJ_MAX;i++) if (draw->model->projBit&(1<<i)) {
            PSMTXConcat(Hu3DProjection[i].projMtx,Hu3DProjection[i].lookAtMtx,projected);
            PSMTXConcat(projected,world,projected);
            GXLoadTexMtxImm(projected,texMtxTbl[i+3],GX_MTX3x4);
        }
    }
    if ((draw->model->attr&HU3D_ATTR_HILITE) || (Hu3DObjInfoP->attr&HU3D_CONST_HILITE))
        SetHiliteTexMtx(draw);
}

/* Character coverage needs geometry only. FaceDraw's material, texture and
 * lighting setup was half the replay's CPU time on the S22+ (2026-10-22).
 * Bind the same vertex layout FaceDraw binds for these display lists
 * and draw them under a constant-alpha TEV; colour writes are off. */
static void mp6AoCoverageFaceDraw(HU3D_DRAW_OBJ *draw, HSF_FACE *face, int batch)
{
    HSF_OBJECT *o=draw->object;
    HU3D_MODEL *m=draw->model;
    HSF_MATERIAL *mat=&o->mesh.material[face->mat & 0xFFF];
    s16 vtxMode=mat->attrNum==0 ? (mat->vtxMode==5 ? 4 : 0) :
        (s16)(((mat->vtxMode==5) ? 5 : 1) | ((DrawData[batch].flags & 2) ? 2 : 0));
    if (vtxMode!=vtxModeBak) {
        vtxModeBak=vtxMode;
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
        GXSetArray(GX_VA_POS,mp6_ground_vertices(m,o),sizeof(HuVecF));
        if (vtxMode & 2) {
            GXSetVtxDesc(GX_VA_NBT,GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NBT,GX_NRM_NBT,GX_S16,8);
        } else {
            GXSetVtxDesc(GX_VA_NRM,GX_INDEX16);
            if (m->hsf->cenvNum==0) {
                GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_S8,0);
                GXSetArray(GX_VA_NRM,o->mesh.normal->data,3*sizeof(s8));
            } else {
                GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_F32,0);
                GXSetArray(GX_VA_NRM,o->mesh.normal->data,sizeof(HuVecF));
            }
        }
        if (mat->attrNum) {
            GXSetVtxDesc(GX_VA_TEX0,GX_INDEX16);
            GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
            GXSetArray(GX_VA_TEX0,o->mesh.st->data,sizeof(HuVec2f));
        }
        if (vtxMode & 4) {
            GXSetVtxDesc(GX_VA_CLR0,GX_INDEX16);
            GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
            GXSetArray(GX_VA_CLR0,o->mesh.color->data,sizeof(GXColor));
        }
    }
    GXCallDisplayList((u8 *)DLBufStartP+DrawData[batch].dlOfs,DrawData[batch].dlSize);
}

static void mp6AoCoverageState(void)
{
    GXSetNumTexGens(0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
    GXSetCullMode(GX_CULL_NONE); /* scene depth still rejects hidden faces */
    mp6AoBackgroundDrawing=1;
    mp6AoFoliageMaskState();
    /* 0.25, not 1: AO depth preparation erases only alpha > 0.5 (water and
     * sky), so characters still occlude the ground around them. */
    GXSetTevKAlphaSel(GX_TEVSTAGE0,GX_TEV_KASEL_1_4);
    /* Mark only pixels the character itself won: pushed slightly away and
     * tested GEQUAL, its silhouette's edge pixels (ground behind) fail
     * instead of leaving a bright unshaded rim. */
    GXSetZMode(GX_TRUE,GX_GEQUAL,GX_FALSE);
    GX2SetPolygonOffset(2.0f,0.5f,2.0f,0.5f,0.0f);
}

/* Reuse only adjacent FaceDraw material/texture cache state. Evaluate these
 * checks before FaceDraw: a material callback may modify its own state, so
 * eligibility must not be inferred after it runs. Unused model/object hook
 * pointers are not callbacks in this direct replay path. */
static HSF_MATERIAL *mp6AoFoliageReuseMaterial(const Mp6AoFoliageDraw *saved)
{
    if (!saved->draw.object || !saved->face) return NULL;
    return &saved->draw.object->mesh.material[saved->face->mat & 0xFFF];
}

static int mp6AoFoliageReuseEligible(const Mp6AoFoliageDraw *saved,
                                     HSF_MATERIAL *material)
{
    HU3D_MODEL *model=saved->draw.model;
    HSF_OBJECT *object=saved->draw.object;
    HSF_CONSTDATA *constData=object ? object->constData : NULL;
    unsigned flags;
    if (!model || !object || !material) return 0;
    flags=object->flags|material->flags;

    /* Keep reuse bounded to ordinary replayed meshes, without callbacks. */
    if (model->attr & HU3D_ATTR_HOOKFUNC) return 0;
    if ((flags & HSF_MATERIAL_MATHOOK) && model->matHook) return 0;
    if (material->refAlpha != 0.0f) return 0;
    if ((model->attr & HU3D_ATTR_HILITE) ||
        (constData && (constData->attr & HU3D_CONST_HILITE)) ||
        (flags & HSF_MATERIAL_HILITE)) return 0;
    if (model->projBit || (model->attr & HU3D_ATTR_TOON_MAP)) return 0;
    if (Hu3DShadowF && shadowNum && constData &&
        (constData->attr & HU3D_CONST_SHADOW_MAP)) return 0;

    if (!object->mesh.attribute) return 1;
    for (int i=0; i<material->attrNum; i++) {
        HSF_ATTRIBUTE *attribute=&object->mesh.attribute[material->attr[i]];
        if (attribute->animWorkP) return 0;
        if (attribute->bitmap && attribute->bitmap->dataFmt==HSF_BMPFMT_CI_IA8)
            return 0;
    }
    /* Oversized bitmaps intentionally remain eligible. FaceDraw invalidates
     * BmpPtrBak for their upload, not the bound texture or materialBak. Its
     * existing same-material fast path already preserves that binding. */
    return 1;
}

static unsigned mp6AoFoliageReuseBatchFlags(const Mp6AoFoliageDraw *saved)
{
    HSF_CONSTDATA *constData=saved->draw.object ? saved->draw.object->constData : NULL;
    if (!constData || !constData->drawData || saved->batch<0) return 0;
    return constData->drawData[saved->batch].flags & 2;
}
#else
#define mp6AoFoliageRemember(draw,face,batch) ((void)0)
#define mp6AoFoliageMaskState() ((void)0)
#endif

void mp6_ao_foliage_render(int camera, int enabled)
{
#ifndef MP6_HEADLESS_BUILD
    if (mp6AoFoliageTick!=mp6_tick_count) { mp6AoFoliageCount=0; return; }
    int count=mp6AoFoliageCount, found=0;
    for (int i=0;i<count;i++) if (mp6AoFoliage[i].camera==camera) found++;
    if (enabled && found && mp6_ao_foliage_begin(camera)) {
        u32 savedPoly=totalPolyCnt,savedMat=totalMatCnt,savedTex=totalTexCnt,savedCache=totalTexCacheCnt;
        mp6AoFoliageDrawing=1;
        const int msaaSeed=mp6_ao_foliage_uses_msaa_seed();
        Hu3DCameraSet(camera,Hu3DCameraMtx);
        mp6_ao_foliage_camera(camera);
        HU3D_MODEL *lastModel=NULL;
        HSF_OBJECT *lastObject=NULL;
        const Mp6AoFoliageDraw *previousSaved=NULL;
        HSF_MATERIAL *previousMaterial=NULL;
        unsigned previousBatchFlags=0;
        int previousEligible=0;
        for (int i=0;i<count;i++) {
            Mp6AoFoliageDraw *saved=mp6AoFoliage+i;
            if (saved->camera!=camera) continue;
            HU3D_DRAW_OBJ *draw=&saved->draw;
            HSF_MATERIAL *currentMaterial=mp6AoFoliageReuseMaterial(saved);
            unsigned currentBatchFlags=mp6AoFoliageReuseBatchFlags(saved);
            int currentEligible=mp6AoFoliageReuseEligible(saved,currentMaterial);
            int reuse=previousSaved && previousEligible && currentEligible &&
                draw->model==previousSaved->draw.model &&
                draw->object==previousSaved->draw.object &&
                currentMaterial==previousMaterial &&
                memcmp(&draw[0],&previousSaved->draw,sizeof(HU3D_DRAW_OBJ))==0 &&
                saved->water==previousSaved->water &&
                saved->background==previousSaved->background &&
                saved->unshaded==previousSaved->unshaded &&
                !saved->character && !previousSaved->character &&
                currentBatchFlags==previousBatchFlags &&
                materialBak==currentMaterial;
            mp6AoWaterDrawing=saved->water;
            mp6AoBackgroundDrawing=saved->background;
            Hu3DObjInfoP=draw->object->constData;
            DLBufStartP=Hu3DObjInfoP->dlBuf;
            DrawData=Hu3DObjInfoP->drawData;
            drawCnt=saved->batch;
            if (!reuse) {
                materialBak=PTR_INVALID;
                for (int t=0;t<8;t++) BmpPtrBak[t]=PTR_INVALID;
                shadingBak=vtxModeBak=-1;
                matHookCallF=FALSE;
            }
            if (draw->model!=lastModel || draw->object!=lastObject) {
                int modelId=(int)(draw->model-Hu3DData);
                mp6_fi_capture_auxiliary_begin(modelId,mp6_fi_object_index(modelId,draw->object));
                lastModel=draw->model; lastObject=draw->object;
            }
            mp6AoFoliageMatrices(draw);
            /* The 4x-MSAA seed uses sample zero, while this private mask is
             * rasterized at the pixel center. On a sloping receiver they differ
             * by up to half a pixel of depth slope. Cover that sampling
             * displacement plus two depth ULPs, without moving geometry or
             * writing depth. Nearer scene geometry still rejects the replay.
             * Only exact registered sky and opaque unshaded trunks receive
             * this tolerance; ordinary transparent foliage does not. */
            const int receiverSampleBias=msaaSeed && !saved->character && (saved->background || saved->unshaded);
            if (receiverSampleBias) GX2SetPolygonOffset(-2.0f,-0.5f,-2.0f,-0.5f,0.0f);
            if (saved->character) {
                if (!previousSaved || !previousSaved->character) mp6AoCoverageState();
                mp6AoCoverageFaceDraw(draw,saved->face,saved->batch);
                materialBak=PTR_INVALID;
            } else {
                if (previousSaved && previousSaved->character) {
                    shadingBak=vtxModeBak=-1;
                    GX2SetPolygonOffset(0.0f,0.0f,0.0f,0.0f,0.0f);
                }
                if (reuse) mp6AoFoliageMaskState();
                FaceDraw(draw,saved->face);
            }
            if (receiverSampleBias) GX2SetPolygonOffset(0.0f,0.0f,0.0f,0.0f,0.0f);
            previousSaved=saved;
            previousMaterial=currentMaterial;
            previousBatchFlags=currentBatchFlags;
            previousEligible=currentEligible;
        }
        mp6_fi_capture_model_end();
        if (previousSaved && previousSaved->character) GX2SetPolygonOffset(0.0f,0.0f,0.0f,0.0f,0.0f);
        mp6AoFoliageDrawing=0;
        mp6AoWaterDrawing=0;
        mp6AoBackgroundDrawing=0;
        mp6_ao_foliage_end(camera);
        Hu3DCameraSet(camera,Hu3DCameraMtx);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        materialBak=PTR_INVALID;
        for (int t=0;t<8;t++) BmpPtrBak[t]=PTR_INVALID;
        totalPolyCnt=savedPoly; totalMatCnt=savedMat; totalTexCnt=savedTex; totalTexCacheCnt=savedCache;
    }
    int kept=0;
    for (int i=0;i<count;i++) if (mp6AoFoliage[i].camera!=camera) mp6AoFoliage[kept++]=mp6AoFoliage[i];
    mp6AoFoliageCount=kept;
#else
    (void)camera; (void)enabled;
#endif
}
