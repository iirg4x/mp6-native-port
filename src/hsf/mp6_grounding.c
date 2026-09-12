/* Selective W01 visual grounding for AO. These are measured art-placement
 * corrections, not fixes to the original simulation. Keep all storage in
 * game-state sections: registries contain game-heap pointers and must rewind
 * together with those heaps on quick-state restore. No GPU/OS allocations. */
#include "mp6_grounding.h"
#include "mp6_enhancements.h"
#include "mp6_gxarray_registry.h"
#include "game/board/masu.h"
#include "grounding_math.h"
#include <string.h>
#include <stdio.h>

#define GROUND_TRI_MAX 2048
#define GROUND_PROP_MAX 128
static GroundTri floorTriangles[GROUND_TRI_MAX];
static int floorCount;
/* Keep original support for candidate selection. Raising the floor must not
 * pull previously ineligible upper log/tree vertices into the repair band. */
static GroundTri originalFloorTriangles[GROUND_TRI_MAX];
static int originalFloorCount;
static HU3D_MODELID terrainId = HU3D_MODELID_NONE;
static HSF_DATA *terrainData;
typedef struct { HU3D_MODELID id; HSF_DATA *hsf; HSF_OBJECT *object; float dy; } GroundProp;
static GroundProp props[GROUND_PROP_MAX];
static int propCount;

/* The measured W01 trunks/tree combine bases, slides and branches in one mesh.
 * Translating that mesh would move its playable top surfaces.
 * Fit just the supported bottom vertices using a separate draw array instead.
 * Bounded, game-owned storage also rewinds safely with quick states. */
#define GROUND_BASE_MAX 19 /* start, ten background meshes, three logs, four tree bases, links */
#define GROUND_BASE_VERTEX_MAX 768
typedef struct {
    HU3D_MODELID id;
    HSF_DATA *hsf;
    HSF_OBJECT *object;
    HuVecF *source;
    int count;
    HuVecF modelPos, modelRot, modelScale;
    Mtx modelMtx;
    HuVecF unchanged[GROUND_BASE_VERTEX_MAX];
    HuVecF vertices[GROUND_BASE_VERTEX_MAX];
} GroundBase;
static GroundBase bases[GROUND_BASE_MAX];
static int baseCount;
static HU3D_MODELID treeId=HU3D_MODELID_NONE;
static HSF_DATA *treeData;
static struct { HU3D_MODELID id; HSF_DATA *hsf; } scenery[8];
static int sceneryCount;
static struct {
    HSF_OBJECT *object;
    HuVecF *source;
    int count;
    HuVecF original[64];
    float floorY, lift, pathLift, pathTopY;
} lawnAnchor;

static int owner_unchanged(const GroundBase *base, const HU3D_MODEL *m)
{
    return !memcmp(&m->pos,&base->modelPos,sizeof(HuVecF)) &&
        !memcmp(&m->rot,&base->modelRot,sizeof(HuVecF)) &&
        !memcmp(&m->scale,&base->modelScale,sizeof(HuVecF)) &&
        !memcmp(m->mtx,base->modelMtx,sizeof(Mtx));
}

static int active(void)
{
#ifdef MP6_HEADLESS_BUILD
    return 0;
#else
    if (terrainId < 0 || terrainId >= HU3D_MODEL_MAX || !terrainData ||
        Hu3DData[terrainId].hsf != terrainData || mp6_enh_ambient_occlusion() == 0) return 0;
    if (lawnAnchor.object) {
        HSF_OBJECT *o=lawnAnchor.object;
        if (!owner_unchanged(bases,Hu3DData+terrainId) || !o->mesh.vertex ||
            o->mesh.vertex->data!=lawnAnchor.source || o->mesh.vertex->count!=lawnAnchor.count ||
            memcmp(lawnAnchor.source,lawnAnchor.original,lawnAnchor.count*sizeof(HuVecF))) return 0;
        for (int depth=0;o;depth++,o=o->mesh.parent)
            if (depth>=64 || memcmp(&o->mesh.curr,&o->mesh.base,sizeof(HSF_TRANSFORM))) return 0;
    }
    return 1;
#endif
}

static void object_matrix(HSF_DATA *hsf, HSF_OBJECT *o, Mtx parent, Mtx out, int posed)
{
    /* Same transform order as hsfdraw's objMesh. Enveloped vertices are
     * already skinned; their mesh matrix is model-relative, not joint-local. */
    HSF_TRANSFORM *t = posed ? &o->mesh.curr : &o->mesh.base;
    PSMTXScale(out,t->scale.x,t->scale.y,t->scale.z);
    mtxRotCat(out,t->rot.x,t->rot.y,t->rot.z);
    mtxTransCat(out,t->pos.x,t->pos.y,t->pos.z);
    PSMTXConcat(parent,out,out);
}

static int support_in(const GroundTri *triangles, int count, float x, float y, float z, float *floorY)
{
    int found=0;
    float best=-1e30f;
    for (int i=0;i<count;i++) {
        float h;
        if (ground_height(triangles+i,x,z,&h) && h <= y+0.5f &&
            y-h <= 32.0f && h>best) { best=h; found=1; }
    }
    if (found) *floorY=best;
    return found;
}

static int support(float x, float y, float z, float *floorY)
{
    return support_in(floorTriangles,floorCount,x,y,z,floorY);
}

static int contact_support(HuVecF p, float *originalY, float *drawY)
{
    if (!originalFloorCount) {
        if (!support(p.x,p.y,p.z,originalY)) return 0;
        *drawY=*originalY;
        return 1;
    }
    if (!support_in(originalFloorTriangles,originalFloorCount,p.x,p.y,p.z,originalY) ||
        !support(p.x,p.y+lawnAnchor.lift,p.z,drawY)) return 0;
    return *drawY>=*originalY-0.05f && *drawY<=*originalY+lawnAnchor.lift+0.05f;
}

static void append_triangle(HuVecF *v, int count, int a, int b, int c, Mtx world)
{
    if (floorCount==GROUND_TRI_MAX || a<0 || b<0 || c<0 || a>=count || b>=count || c>=count) return;
    HuVecF p[3];
    PSMTXMultVec(world,v+a,p); PSMTXMultVec(world,v+b,p+1); PSMTXMultVec(world,v+c,p+2);
    GroundTri t={{p[0].x,p[0].y,p[0].z},{p[1].x,p[1].y,p[1].z},{p[2].x,p[2].y,p[2].z}};
    float h;
    if (ground_height(&t,(p[0].x+p[1].x+p[2].x)/3,(p[0].z+p[1].z+p[2].z)/3,&h))
        floorTriangles[floorCount++]=t;
}

static void floor_mesh(HSF_OBJECT *o, Mtx world)
{
    if (!o->mesh.vertex || !o->mesh.face) return;
    HuVecF *v=o->mesh.vertex->data;
    /* The second cache pass follows the raised draw geometry, even if AO is
     * currently Off, so the live toggle and all prop contacts agree. */
    for (int i=0;i<baseCount;i++)
        if (bases[i].object==o && bases[i].source==v) { v=bases[i].vertices; break; }
    HSF_FACE *faces=o->mesh.face->data;
    for (int i=0;i<o->mesh.face->count;i++) {
        HSF_FACE *f=faces+i;
        int a=f->index[0].vertex, b=f->index[2].vertex, c=f->index[1].vertex;
        int type=f->typeSrc & HSF_FACE_MASK;
        if (type==HSF_FACE_TRI) append_triangle(v,o->mesh.vertex->count,a,b,c,world);
        else if (type==HSF_FACE_QUAD) {
            int d=f->index[3].vertex;
            append_triangle(v,o->mesh.vertex->count,a,b,d,world);
            append_triangle(v,o->mesh.vertex->count,a,d,c,world);
        } else if (type==HSF_FACE_TRISTRIP) {
            append_triangle(v,o->mesh.vertex->count,a,b,c,world);
            for (unsigned j=0; f->strip.data && j<f->strip.count;j++) {
                a=b; b=c; c=f->strip.data[j].vertex;
                append_triangle(v,o->mesh.vertex->count,a,b,c,world);
            }
        }
    }
}

static void keep_base_copy(HU3D_MODELID id, HSF_OBJECT *o, GroundBase *base)
{
    HU3D_MODEL *model=Hu3DData+id;
    base->id=id; base->hsf=model->hsf;
    base->object=o; base->source=o->mesh.vertex->data; base->count=o->mesh.vertex->count;
    base->modelPos=model->pos; base->modelRot=model->rot; base->modelScale=model->scale;
    memcpy(base->modelMtx,model->mtx,sizeof(Mtx));
    mp6_gxarray_register(base->vertices,base->count*sizeof(HuVecF));
    baseCount++;
}

/* The ISO's lawn/path artwork sits below the authored start-space floor.
 * Raise their draw vertices to that fixed floor, never to a moving actor pose.
 * A small path/lawn clearance avoids coplanar flicker. No hierarchy matrix moves.
 * Unsupported paths, slopes and interior ridges fail closed. */
static void calibrate_lawn(HSF_OBJECT *o, Mtx world)
{
    if (lawnAnchor.object || !o->mesh.vertex || !o->mesh.vertex->data ||
        o->mesh.vertex->count<3 || o->mesh.vertex->count>64 || !o->mesh.face) return;
    HuVecF lo={1e30f,1e30f,1e30f}, hi={-1e30f,-1e30f,-1e30f};
    HuVecF *v=o->mesh.vertex->data;
    for (int i=0;i<o->mesh.vertex->count;i++) {
        HuVecF p; float h;
        PSMTXMultVec(world,v+i,&p);
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) ||
            !support(p.x,p.y,p.z,&h) || p.y-h<=0.5f || p.y-h>=20) return;
        lo.x=fminf(lo.x,p.x); lo.y=fminf(lo.y,p.y); lo.z=fminf(lo.z,p.z);
        hi.x=fmaxf(hi.x,p.x); hi.y=fmaxf(hi.y,p.y); hi.z=fmaxf(hi.z,p.z);
    }
    if (hi.y-lo.y>0.25f) return;
    float floorLo=1e30f, floorHi=-1e30f;
    for (int i=0;i<floorCount;i++) {
        const GroundTri *t=floorTriangles+i;
        if (fmaxf(t->a.x,fmaxf(t->b.x,t->c.x))<lo.x ||
            fminf(t->a.x,fminf(t->b.x,t->c.x))>hi.x ||
            fmaxf(t->a.z,fmaxf(t->b.z,t->c.z))<lo.z ||
            fminf(t->a.z,fminf(t->b.z,t->c.z))>hi.z) continue;
        floorLo=fminf(floorLo,fminf(t->a.y,fminf(t->b.y,t->c.y)));
        floorHi=fmaxf(floorHi,fmaxf(t->a.y,fmaxf(t->b.y,t->c.y)));
    }
    float lift=lo.y-0.5f-floorHi;
    if (floorHi<floorLo || floorHi-floorLo>0.05f || lift<=0 || lift>=20) return;
    /* MB1_Create loads spaces before registering this terrain. Read the same
     * flagged start position used by OpeningPlayerInit; never modify it. Keep
     * the surface just below the nominal sole origin, not 8-9 units beneath it. */
    s16 startId=mbMasuFind_AttrIdGet(-1,MASU_FLAG_START);
    if (startId<1 || startId>mbMasuNumGet()) return;
    const MASU *start=mbMasuGet(startId);
    if (!start || start->useMtxF || start->rot.x!=0 || start->rot.z!=0 ||
        !isfinite(start->pos.y) || start->pos.x<lo.x || start->pos.x>hi.x ||
        start->pos.z<lo.z || start->pos.z>hi.z) return;
    float pathLift=start->pos.y-0.25f-hi.y;
    if (!isfinite(pathLift) || pathLift<=0 || pathLift>=12 || lift+pathLift>=24) return;
    lawnAnchor.object=o; lawnAnchor.source=v; lawnAnchor.count=o->mesh.vertex->count;
    memcpy(lawnAnchor.original,v,lawnAnchor.count*sizeof(HuVecF));
    lawnAnchor.floorY=(floorLo+floorHi)*0.5f;
    lawnAnchor.pathLift=pathLift; lawnAnchor.lift=lift+pathLift;
    lawnAnchor.pathTopY=hi.y+pathLift;
}

/* All nine connected background sections and their lower ground plane must
 * share one translation, including cliffs and distant slopes. Moving only
 * the three flat lawn tops separates the landscape during the slide camera.
 * These are the measured terrain meshes, not the sky, tree, stairs or props. */
static int background_mesh(const char *name)
{
    static const char *const names[]={"haikei1","haikei10","haikei11","haikei12",
        "haikei14","haikei15","haikei16","haikei17","haikei2","grid309"};
    for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)
        if (!strcmp(name,names[i])) return 1;
    return 0;
}

static int lawn_mesh(HU3D_MODELID id, HSF_OBJECT *o, Mtx world)
{
    if (baseCount==GROUND_BASE_MAX || !o->mesh.vertex || !o->mesh.vertex->data ||
        o->mesh.vertex->count<=0 || o->mesh.vertex->count>GROUND_BASE_VERTEX_MAX) return 0;
    Mtx inverse;
    if (!PSMTXInverse(world,inverse)) return 0;
    GroundBase *base=bases+baseCount;
    HuVecF *v=o->mesh.vertex->data;
    const int path=o==lawnAnchor.object;
    const float lift=path ? lawnAnchor.pathLift : lawnAnchor.lift;
    int changed=0;
    for (int i=0;i<o->mesh.vertex->count;i++) {
        base->vertices[i]=base->unchanged[i]=v[i];
        base->vertices[i].x+=inverse[0][1]*lift;
        base->vertices[i].y+=inverse[1][1]*lift;
        base->vertices[i].z+=inverse[2][1]*lift;
        changed++;
    }
    if (changed) keep_base_copy(id,o,base);
    return changed!=0;
}

static void base_mesh(HU3D_MODELID id, HSF_OBJECT *o, Mtx world)
{
    const int tree=id==treeId && Hu3DData[id].hsf==treeData;
    if (baseCount==GROUND_BASE_MAX || !o->mesh.vertex || !o->mesh.vertex->data ||
        o->mesh.vertex->count<=0 || o->mesh.vertex->count>GROUND_BASE_VERTEX_MAX) return;
    if (tree) {
        if (strcmp(o->name,"b01_m240") && strcmp(o->name,"r_ashi") && strcmp(o->name,"l_ashi")) return;
    } else if (id!=terrainId ||
        (strcmp(o->name,"obj34") && strcmp(o->name,"kirikabu") && strcmp(o->name,"obj37"))) return;
    Mtx inverse;
    if (!PSMTXInverse(world,inverse)) return;
    GroundBase *base=bases+baseCount;
    HuVecF *v=o->mesh.vertex->data;
    int changed=0;
    for (int i=0;i<o->mesh.vertex->count;i++) {
        base->vertices[i]=base->unchanged[i]=v[i];
        HuVecF p;
        PSMTXMultVec(world,v+i,&p);
        float h,originalY;
        if (contact_support(p,&originalY,&h)) {
            float gap=p.y-originalY;
            if (gap>0.5f && gap<(tree?32.0f:20.0f)) {
                /* Match the supported bottom ring to the current lawn. A tiny
                 * overlap closes rasterization cracks. Tops/branches above the original band
                 * are bit-identical, and children never inherit an offset. */
                float dy=h-p.y-0.1f;
                base->vertices[i].x+=inverse[0][1]*dy;
                base->vertices[i].y+=inverse[1][1]*dy;
                base->vertices[i].z+=inverse[2][1]*dy;
                changed++;
            }
        }
    }
    if (changed) keep_base_copy(id,o,base);
}

/* W01's white space links are a separate additive mesh (b01_m001/I8line).
 * Its four lawn-level quads originally sit at Y=2; raising only the lawn/start
 * covers them. Fit complete supported quads above the new path, still below
 * the space art at startY+3. Never move the mesh's many elevated-board links,
 * override depth tests, change blending or alter the source vertex buffer. */
static void connector_mesh(HU3D_MODELID id, HSF_OBJECT *o, Mtx world)
{
    if (!lawnAnchor.object || baseCount==GROUND_BASE_MAX ||
        !o->mesh.vertex || !o->mesh.vertex->data || !o->mesh.face || !o->mesh.face->data ||
        o->mesh.vertex->count<=0 || o->mesh.vertex->count>GROUND_BASE_VERTEX_MAX ||
        o->mesh.face->count!=o->mesh.vertex->count/4 || o->mesh.vertex->count%4) return;
    /* The measured asset consists of disjoint quads. Reject unknown topology
     * rather than moving shared vertices on an unsupported neighboring face. */
    unsigned char seen[GROUND_BASE_VERTEX_MAX]={0};
    HSF_FACE *faces=o->mesh.face->data;
    for (int i=0;i<o->mesh.face->count;i++) {
        if ((faces[i].typeSrc&HSF_FACE_MASK)!=HSF_FACE_QUAD) return;
        for (int j=0;j<4;j++) {
            int index=faces[i].index[j].vertex;
            if (index<0 || index>=o->mesh.vertex->count || seen[index]++) return;
        }
    }
    Mtx inverse;
    if (!PSMTXInverse(world,inverse)) return;
    HuVecF *v=o->mesh.vertex->data;
    GroundBase *base=bases+baseCount;
    memcpy(base->vertices,v,o->mesh.vertex->count*sizeof(HuVecF));
    memcpy(base->unchanged,v,o->mesh.vertex->count*sizeof(HuVecF));
    const float targetY=lawnAnchor.pathTopY+0.5f;
    int changed=0;
    for (int i=0;i<o->mesh.face->count;i++) {
        float low=1e30f, high=-1e30f;
        int supported=1;
        for (int j=0;j<4;j++) {
            HuVecF p; float originalY,drawY;
            PSMTXMultVec(world,v+faces[i].index[j].vertex,&p);
            if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) ||
                !contact_support(p,&originalY,&drawY) || p.y-originalY<0.5f ||
                p.y-originalY>20 || targetY-drawY<0.5f || targetY-drawY>2) {
                supported=0; break;
            }
            low=fminf(low,p.y); high=fmaxf(high,p.y);
        }
        if (!supported || high-low>0.05f || targetY<=high || targetY-high>12) continue;
        float dy=targetY-high;
        for (int j=0;j<4;j++) {
            HuVecF *p=base->vertices+faces[i].index[j].vertex;
            p->x+=inverse[0][1]*dy;
            p->y+=inverse[1][1]*dy;
            p->z+=inverse[2][1]*dy;
        }
        changed++;
    }
    if (changed) keep_base_copy(id,o,base);
}

static void scan(HU3D_MODELID id, HSF_OBJECT *o, Mtx parent, int mode, int depth)
{
    if (!o || depth>64 || o->type==HSF_OBJ_CAMERA || o->type==HSF_OBJ_LIGHT || o->type==HSF_OBJ_REPLICA) return;
    HSF_DATA *hsf=Hu3DData[id].hsf;
    Mtx world;
    object_matrix(hsf,o,parent,world,0);
    if (o->type==HSF_OBJ_MESH && o->name) {
        /* Explicit W01 ground surfaces, not tree platforms, stairs, water,
         * decals, collision proxies or arbitrary nearby scenery. */
        int lawn=!strcmp(o->name,"haikei10") || !strcmp(o->name,"haikei11") || !strcmp(o->name,"haikei17");
        if (mode==0 && lawn)
            floor_mesh(o,world);
        if (mode==2 && id==terrainId && !strcmp(o->name,"start")) calibrate_lawn(o,world);
        if (mode==3 && (background_mesh(o->name) || o==lawnAnchor.object) && lawnAnchor.object &&
            !lawn_mesh(id,o,world)) lawnAnchor.lift=0;
        if (mode==1 && (id==terrainId || id==treeId)) base_mesh(id,o,world);
        if (mode==1 && id!=terrainId && id!=treeId && !strcmp(o->name,"b01_m001"))
            connector_mesh(id,o,world);
        if (mode==1 && o->mesh.vertex &&
            (!strncmp(o->name,"flower_",7) || !strncmp(o->name,"clover",6) ||
             !strncmp(o->name,"jyarasi_",8) || !strcmp(o->name,"kanban"))) {
            HuVecF bottom={0,1e30f,0};
            HuVecF *v=o->mesh.vertex->data;
            for (int i=0;i<o->mesh.vertex->count;i++) {
                HuVecF p; PSMTXMultVec(world,v+i,&p);
                if (p.y<bottom.y) bottom=p;
            }
            float h,originalY;
            if (propCount<GROUND_PROP_MAX && contact_support(bottom,&originalY,&h)) {
                float gap=bottom.y-originalY;
                if (gap>0.5f && gap<20) {
                    props[propCount++]=(GroundProp){id,hsf,o,h-bottom.y};
                    /* This whole branch moves together, including flower
                     * heads/leaves and wind animation. Never double-apply. */
                    return;
                }
            }
        }
    }
    for (unsigned i=0;i<o->mesh.childNum;i++) scan(id,o->mesh.child[i],world,mode,depth+1);
}

static void scan_model(HU3D_MODELID id, int mode)
{
    if (id<0 || id>=HU3D_MODEL_MAX) return;
    HU3D_MODEL *m=Hu3DData+id;
    if (!m->hsf || (m->attr&HU3D_ATTR_HOOKFUNC) || m->hsf->cenvNum) return;
    Mtx world;
    mtxRot(world,m->rot.x,m->rot.y,m->rot.z);
    mtxScaleCat(world,m->scale.x,m->scale.y,m->scale.z);
    mtxTransCat(world,m->pos.x,m->pos.y,m->pos.z);
    PSMTXConcat(world,m->mtx,world);
    scan(id,m->hsf->root,world,mode,0);
}

void mp6_ground_end(void)
{
    originalFloorCount=floorCount=propCount=baseCount=sceneryCount=0;
    terrainId=HU3D_MODELID_NONE; terrainData=NULL;
    treeId=HU3D_MODELID_NONE; treeData=NULL;
    memset(&lawnAnchor,0,sizeof(lawnAnchor));
}

void mp6_ground_w01_begin(HU3D_MODELID terrain)
{
    mp6_ground_end();
    if (terrain<0 || terrain>=HU3D_MODEL_MAX) return;
    terrainId=terrain; terrainData=Hu3DData[terrain].hsf;
    scan_model(terrain,0); /* original floor supports the reference path */
    scan_model(terrain,2); /* calibrate the flat-lawn lift */
    if (lawnAnchor.object) {
        scan_model(terrain,3);
        /* Do not leave seams if any selected lawn could not be copied. */
        if (!baseCount || lawnAnchor.lift==0) {
            baseCount=0;
            memset(&lawnAnchor,0,sizeof(lawnAnchor));
        } else {
            originalFloorCount=floorCount;
            memcpy(originalFloorTriangles,floorTriangles,floorCount*sizeof(GroundTri));
        }
        floorCount=0;
        scan_model(terrain,0);
    }
    scan_model(terrain,1); /* props use the same lawn as the renderer */
}

void mp6_ground_w01_scenery(HU3D_MODELID model) {
    if (sceneryCount<8 && model>=0 && model<HU3D_MODEL_MAX &&
        !(Hu3DData[model].attr&HU3D_ATTR_HOOKFUNC)) {
        scenery[sceneryCount].id=model;
        scenery[sceneryCount++].hsf=Hu3DData[model].hsf;
    }
    scan_model(model,1);
}

void mp6_ground_w01_tree(HU3D_MODELID model)
{
    /* W01's normal tree (day/night archive entry 0x2B), not its event/transition
     * replacements. The same named root contains the slide exit and ground skirt. */
    if (model<0 || model>=HU3D_MODEL_MAX || treeData) return;
    HU3D_MODEL *m=Hu3DData+model;
    if (!m->hsf || m->attr&HU3D_ATTR_HOOKFUNC || !m->hsf->root ||
        !m->hsf->root->name || strcmp(m->hsf->root->name,"b01_m240")) return;
    treeId=model; treeData=m->hsf;
    scan_model(model,1);
}

int mp6_ground_is_scenery(HU3D_MODEL *m)
{
    if (!active() || (m->attr&HU3D_ATTR_HOOKFUNC)) return 0;
    if (m-Hu3DData==terrainId && m->hsf==terrainData) return 1;
    for (int i=0;i<sceneryCount;i++)
        if (m-Hu3DData==scenery[i].id && m->hsf==scenery[i].hsf) return 1;
    return 0;
}

void mp6_ground_object(HU3D_MODEL *m, HSF_OBJECT *o, Mtx view, int descendants)
{
    if (!active()) return;
    for (int i=0;i<propCount;i++) {
        GroundProp *p=props+i;
        if (p->id==m-Hu3DData && p->hsf==m->hsf && p->object==o) {
            if (!descendants) return;
            /* View-space displacement of world up; works for both the real
             * camera and original shadow camera. Descendants inherit it. */
            for (int j=0;j<3;j++) view[j][3]+=Hu3DCameraMtx[j][1]*p->dy;
            return;
        }
    }
}

HuVecF *mp6_ground_vertices(HU3D_MODEL *m, HSF_OBJECT *o)
{
    HuVecF *original=o->mesh.vertex->data;
    if (!active() || (m->attr&HU3D_ATTR_HOOKFUNC)) return original;
    for (int i=0;i<baseCount;i++) {
        GroundBase *base=bases+i;
        if (base->id!=m-Hu3DData || base->hsf!=m->hsf || base->object!=o ||
            base->source!=original || base->count!=o->mesh.vertex->count) continue;
        if (!owner_unchanged(base,m)) return original;
        HSF_OBJECT *ancestor=o;
        for (int depth=0;ancestor;depth++,ancestor=ancestor->mesh.parent)
            if (depth>=64 || memcmp(&ancestor->mesh.curr,&ancestor->mesh.base,sizeof(HSF_TRANSFORM))) return original;
        /* Never freeze a shape/cluster update or use a base calibrated for a
         * different pose. This also covers animations with an unchanged matrix. */
        if (memcmp(original,base->unchanged,base->count*sizeof(HuVecF))) return original;
        return base->vertices;
    }
    return original;
}
