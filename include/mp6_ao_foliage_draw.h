/* Included inside the port's hsfdraw adaptation, after its private declarations.
 * Reuses ONLY the original material/mesh submission, never model/event hooks.
 * The normal EFB draw remains byte-for-byte unchanged. */
#include "mp6_ambient_occlusion.h"
#include "mp6_ao_foreground_policy.h"
#include "mp6_enhancements.h"

#ifndef MP6_HEADLESS_BUILD
#define MP6_AO_FOLIAGE_MAX 2048
typedef struct {
    HU3D_DRAW_OBJ draw;
    HSF_FACE *face;
    int batch, camera, water;
} Mp6AoFoliageDraw;
static Mp6AoFoliageDraw mp6AoFoliage[MP6_AO_FOLIAGE_MAX];
static int mp6AoFoliageCount, mp6AoFoliageDrawing;
static int mp6AoWaterDrawing;
static long mp6AoFoliageTick=-1;

static void mp6AoFoliageRemember(HU3D_DRAW_OBJ *draw, HSF_FACE *face, int batch)
{
    if (mp6AoFoliageDrawing || shadowModelDrawF || !mp6_enh_ambient_occlusion()) return;
    if (mp6AoFoliageTick!=mp6_tick_count) {
        mp6AoFoliageTick=mp6_tick_count; mp6AoFoliageCount=0;
    }
    /* Includes event props such as DK's cp37 tree, on every board. Never
     * replay model hooks, or apply background AO to their ordinary alpha color. */
    int water=mp6_ao_water_surface(draw);
    if (!water && !mp6_ao_foreground_eligible(draw,face)) return;
    if (mp6AoFoliageCount<MP6_AO_FOLIAGE_MAX)
        mp6AoFoliage[mp6AoFoliageCount++]=(Mp6AoFoliageDraw){*draw,face,batch,Hu3DCameraNo,water};
}

static void mp6AoFoliageMaskState(void)
{
    if (!mp6AoFoliageDrawing) return;
    /* Keep original TEV color AND alpha, UV animation, filtering and cull.
     * Original RGB over black accumulates the premultiplied foreground color.
     * A transmission-only mask still tints partially transparent leaf edges
     * with background AO; the compositor must preserve their color contribution.
     * Test against final scene depth (read-only attachment, or a private seed
     * on compatibility paths), so foreground actors reject background grass
     * without changing the game's depth buffer. Depth writes MUST stay off. */
    GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_NOOP);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE);
    if (mp6AoWaterDrawing) {
        /* mizu3 is a continuous water surface. Its animated alpha is ripple
         * intensity, NOT holes in the receiver. Replaying that alpha left AO
         * between the ripples (and later layers could erase earlier coverage).
         * Mark the mesh footprint, still rejected by final opaque scene depth.
         * This only changes the private mask, never the game's water draw. */
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
        Hu3DCameraSet(camera,Hu3DCameraMtx);
        mp6_ao_foliage_camera(camera);
        HU3D_MODEL *lastModel=NULL;
        HSF_OBJECT *lastObject=NULL;
        for (int i=0;i<count;i++) {
            Mp6AoFoliageDraw *saved=mp6AoFoliage+i;
            if (saved->camera!=camera) continue;
            HU3D_DRAW_OBJ *draw=&saved->draw;
            mp6AoWaterDrawing=saved->water;
            Hu3DObjInfoP=draw->object->constData;
            DLBufStartP=Hu3DObjInfoP->dlBuf;
            DrawData=Hu3DObjInfoP->drawData;
            drawCnt=saved->batch;
            materialBak=PTR_INVALID;
            for (int t=0;t<8;t++) BmpPtrBak[t]=PTR_INVALID;
            shadingBak=vtxModeBak=-1;
            matHookCallF=FALSE;
            if (draw->model!=lastModel || draw->object!=lastObject) {
                mp6_fi_capture_auxiliary_begin(draw->model-Hu3DData,draw->object-draw->model->hsf->object);
                lastModel=draw->model; lastObject=draw->object;
            }
            mp6AoFoliageMatrices(draw);
            FaceDraw(draw,saved->face);
        }
        mp6_fi_capture_model_end();
        mp6AoFoliageDrawing=0;
        mp6AoWaterDrawing=0;
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
