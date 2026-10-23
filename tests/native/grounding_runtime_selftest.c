#include "mp6_grounding.h"
#include "game/board/player.h"
#include "game/board/masu.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData=models;
Mtx Hu3DCameraMtx;
s16 Hu3DCameraNo;
BOOL shadowModelDrawF;
GW_PLAYER GwPlayer[GW_PLAYER_MAX];
static MASU spaces[3];
static int ao=2, motion=1;
static int registered;
unsigned mp6_ground_test_active_calls;
void mp6_gxarray_register(const void *data, unsigned int size) { assert(data && size); registered++; }
int mp6_enh_ambient_occlusion(void) { return ao; }
int mbPlayerMotionGet(int p) { return motion; }
int mbMasuNumGet(void) { return 2; }
MASU *mbMasuGet(s16 id) { return spaces+id; }
s16 mbMasuFind_AttrIdGet(s16 id,u16 attr) {
    (void)id;
    for (int i=1;i<=2;i++) if (spaces[i].flag&attr) return i;
    return MASU_NULL;
}

void PSMTXScale(Mtx m,float x,float y,float z) {
    memset(m,0,sizeof(Mtx)); m[0][0]=x; m[1][1]=y; m[2][2]=z;
}
void PSMTXConcat(const Mtx a,const Mtx b,Mtx out) {
    Mtx tmp;
    for(int i=0;i<3;i++) for(int j=0;j<4;j++) {
        tmp[i][j]=j==3?a[i][3]:0;
        for(int k=0;k<3;k++) tmp[i][j]+=a[i][k]*b[k][j];
    }
    memcpy(out,tmp,sizeof(Mtx));
}
void PSMTXMultVec(const Mtx m,const Vec *v,Vec *out) {
    Vec t={m[0][0]*v->x+m[0][1]*v->y+m[0][2]*v->z+m[0][3],
           m[1][0]*v->x+m[1][1]*v->y+m[1][2]*v->z+m[1][3],
           m[2][0]*v->x+m[2][1]*v->y+m[2][2]*v->z+m[2][3]}; *out=t;
}
unsigned int PSMTXInverse(const Mtx m,Mtx out) {
    /* This fixture uses only diagonal scales and translations. */
    if (!m[0][0] || !m[1][1] || !m[2][2]) return 0;
    memset(out,0,sizeof(Mtx));
    for(int i=0;i<3;i++) { out[i][i]=1/m[i][i]; out[i][3]=-m[i][3]/m[i][i]; }
    return 1;
}
void mtxRot(Mtx m,float x,float y,float z) { assert(x==0 && y==0 && z==0); PSMTXScale(m,1,1,1); }
void mtxRotCat(Mtx m,float x,float y,float z) { assert(x==0 && y==0 && z==0); }
void mtxScaleCat(Mtx m,float x,float y,float z) {
    for(int i=0;i<4;i++) { m[0][i]*=x; m[1][i]*=y; m[2][i]*=z; }
}
void mtxTransCat(Mtx m,float x,float y,float z) { m[0][3]+=x; m[1][3]+=y; m[2][3]+=z; }

static void model_init(int id,HSF_DATA *hsf) {
    Hu3DData[id].hsf=hsf;
    Hu3DData[id].scale=(Vec){1,1,1};
    Hu3DData[id].motIdShift=HU3D_MOTIONID_NONE;
    PSMTXScale(Hu3DData[id].mtx,1,1,1);
}
static void world_at(Mtx world,float y) { PSMTXScale(world,1,1,1); world[1][3]=y; }
static Mtx submitted;
void mp6_fi_capture_model_begin(int model) { (void)model; }
void Hu3DDraw(HU3D_MODEL *model, Mtx matrix, HuVecF *scale) {
    (void)model; (void)scale; memcpy(submitted,matrix,sizeof(Mtx));
}
#include "grounding_render_path.h"


int main(void) {
    HuVecF groundVertices[4]={{-100,-10,-100},{-100,-10,100},{100,-10,-100},{100,-10,100}};
    HSF_FACE face={0}; face.type=HSF_FACE_QUAD;
    for(int i=0;i<4;i++) face.index[i].vertex=i;
    HSF_BUFFER v={.count=4,.data=groundVertices},f={.count=1,.data=&face};
    HSF_OBJECT floor={.name="haikei17",.type=HSF_OBJ_MESH};
    floor.mesh.base.scale=floor.mesh.curr.scale=(HuVecF){1,1,1};
    floor.mesh.vertex=&v; floor.mesh.face=&f;
    HSF_DATA terrain={.root=&floor,.object=&floor,.objectNum=1};
    HuVecF logVertices[5]={{-20,0,-20},{20,0,20},{-20,50,-20},{20,50,20},{150,0,0}};
    HuVecF originalLog[5]; memcpy(originalLog,logVertices,sizeof(logVertices));
    HSF_BUFFER logBuffer={.count=5,.data=logVertices};
    HSF_OBJECT log={.name="kirikabu",.type=HSF_OBJ_MESH};
    log.mesh.base.scale=log.mesh.curr.scale=(HuVecF){1,1,1};
    log.mesh.vertex=&logBuffer;
    HSF_OBJECT *children[]={&log}; floor.mesh.childNum=1; floor.mesh.child=children;
    model_init(1,&terrain);
    mp6_ground_w01_begin(1);
    HuVecF *renderLog=mp6_ground_vertices(Hu3DData+1,&log);
#ifdef MP6_HEADLESS_BUILD
    assert(renderLog==logVertices);
#else
    assert(registered && renderLog!=logVertices);
    assert(fabsf(renderLog[0].y+10.1f)<.001f && fabsf(renderLog[1].y+10.1f)<.001f);
    assert(!memcmp(renderLog+2,logVertices+2,3*sizeof(HuVecF))); /* tops/unsupported untouched */
    assert(!memcmp(originalLog,logVertices,sizeof(logVertices))); /* asset untouched */
    ao=0; assert(mp6_ground_vertices(Hu3DData+1,&log)==logVertices); ao=2;
    log.mesh.curr.pos.y=5; assert(mp6_ground_vertices(Hu3DData+1,&log)==logVertices);
    log.mesh.curr.pos.y=0;
#endif
    /* The normal tree is separate from the static terrain, and its combined
     * slide/base mesh exceeds the former 512-vertex log limit. The two
     * foreground AO exclusions are the measured 546-vertex root body and
     * 149-vertex child body; similarly named detail meshes must stay shaded. */
    HuVecF treeVertices[600];
    for (int i=0;i<600;i++) treeVertices[i]=(HuVecF){0,100,0};
    treeVertices[0]=(HuVecF){-20,2,-20};
    treeVertices[1]=(HuVecF){20,16,20};
    treeVertices[2]=(HuVecF){0,-12,0};
    treeVertices[3]=(HuVecF){150,2,0};
    HSF_BUFFER treeRootBuffer={.count=546,.data=treeVertices};
    HSF_BUFFER treeChildBuffer={.count=149,.data=treeVertices};
    HSF_OBJECT treeMesh[2]={0};
    treeMesh[0].name=treeMesh[1].name="b01_m240";
    for(int i=0;i<2;i++) {
        treeMesh[i].type=HSF_OBJ_MESH;
        treeMesh[i].mesh.base.scale=treeMesh[i].mesh.curr.scale=(HuVecF){1,1,1};
    }
    treeMesh[0].mesh.vertex=&treeRootBuffer;
    treeMesh[1].mesh.vertex=&treeChildBuffer;
    HSF_OBJECT treeDetail[4]={{.name="nose",.type=HSF_OBJ_MESH},
                              {.name="eye",.type=HSF_OBJ_MESH},
                              {.name="branch",.type=HSF_OBJ_MESH},
                              {.name="b01_m240",.type=HSF_OBJ_MESH}};
    HuVecF detailVertices[4][149];
    HSF_BUFFER detailBuffers[4];
    for (int i=0;i<4;i++) {
        for (int j=0;j<149;j++) detailVertices[i][j]=(HuVecF){0,100,0};
        detailBuffers[i]=(HSF_BUFFER){.count=i==3 ? 148 : 149,
                                      .data=detailVertices[i]};
        treeDetail[i].mesh.base.scale=treeDetail[i].mesh.curr.scale=(HuVecF){1,1,1};
        treeDetail[i].mesh.vertex=detailBuffers+i;
    }
    HSF_OBJECT *treeChildren[]={treeMesh+1,treeDetail,treeDetail+1,
                                treeDetail+2,treeDetail+3};
    treeMesh[0].mesh.childNum=5; treeMesh[0].mesh.child=treeChildren;
    treeMesh[1].mesh.parent=treeMesh;
    for (int i=0;i<4;i++) treeDetail[i].mesh.parent=treeMesh;
    HSF_DATA tree={.root=treeMesh,.object=treeMesh,.objectNum=6};
    model_init(3,&tree);
    assert(mp6_ground_vertices(Hu3DData+3,treeMesh)==treeVertices); /* explicit registration */
    mp6_ground_w01_tree(3);
    HuVecF *renderTree=mp6_ground_vertices(Hu3DData+3,treeMesh+1);
#ifdef MP6_HEADLESS_BUILD
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh));
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1));
#else
    assert(mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh));
    assert(mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1));
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeDetail)); /* nose */
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeDetail+1)); /* eye */
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeDetail+2)); /* branch */
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeDetail+3)); /* name only */
    treeMesh[1].mesh.parent=treeDetail;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* hierarchy */
    treeMesh[1].mesh.parent=treeMesh;
    HuVecF alternateTreeVertices[149];
    memcpy(alternateTreeVertices,treeVertices,sizeof(alternateTreeVertices));
    HSF_BUFFER alternateTreeBuffer={.count=149,.data=alternateTreeVertices};
    treeMesh[1].mesh.vertex=&alternateTreeBuffer;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* source */
    treeMesh[1].mesh.vertex=&treeChildBuffer;
    treeChildBuffer.count=148;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* count */
    treeChildBuffer.count=149;
    treeMesh[1].mesh.curr.pos.y=1;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* pose */
    treeMesh[1].mesh.curr.pos.y=0;
    Hu3DData[3].pos.y=1;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* model */
    Hu3DData[3].pos.y=0;
    Hu3DData[3].attr=HU3D_ATTR_HOOKFUNC;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* hook */
    Hu3DData[3].attr=HU3D_ATTR_COLOR_NOUPDATE;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* no color */
    Hu3DData[3].attr=0;
    ao=0;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* AO off */
    ao=2;
    HSF_DATA registryReplacement=tree;
    Hu3DData[3].hsf=&registryReplacement;
    assert(!mp6_ground_is_ao_unshaded(Hu3DData+3,treeMesh+1)); /* reused HSF */
    Hu3DData[3].hsf=&tree;
#endif
#ifdef MP6_HEADLESS_BUILD
    assert(renderTree==treeVertices);
#else
    assert(renderTree!=treeVertices);
    assert(fabsf(renderTree[0].y+10.1f)<.001f && fabsf(renderTree[1].y+10.1f)<.001f);
    assert(!memcmp(renderTree+2,treeVertices+2,147*sizeof(HuVecF)));
    assert(treeVertices[0].y==2 && treeVertices[1].y==16 && Hu3DData[3].pos.y==0);
    treeMesh[0].mesh.curr.pos.y=5;
    assert(mp6_ground_vertices(Hu3DData+3,treeMesh+1)==treeVertices); /* moving ancestor */
    treeMesh[0].mesh.curr.pos.y=0;
    treeVertices[4].y=110;
    assert(mp6_ground_vertices(Hu3DData+3,treeMesh+1)==treeVertices); /* shape change */
    treeVertices[4].y=100;
    Hu3DData[3].pos.y=5;
    assert(mp6_ground_vertices(Hu3DData+3,treeMesh+1)==treeVertices); /* moved owner */
    Hu3DData[3].pos.y=0;
    HSF_DATA replacement=tree;
    Hu3DData[3].hsf=&replacement;
    assert(mp6_ground_vertices(Hu3DData+3,treeMesh+1)==treeVertices); /* reused id */
    Hu3DData[3].hsf=&tree;
    assert(mp6_ground_vertices(Hu3DData+3,treeMesh+1)==renderTree);
    ao=0; assert(mp6_ground_vertices(Hu3DData+3,treeMesh+1)==treeVertices); ao=2;
    mp6_ground_end();
    assert(mp6_ground_vertices(Hu3DData+3,treeMesh+1)==treeVertices);
    mp6_ground_w01_begin(1);
#endif
    HuVecF feet[2]={{-1,-1,0},{1,-1,0}};
    HSF_BUFFER bodyVertices={.count=2,.data=feet};
    HSF_OBJECT body={.type=HSF_OBJ_MESH};
    body.mesh.base.scale=body.mesh.curr.scale=(HuVecF){1,1,1};
    body.mesh.vertex=&bodyVertices;
    HSF_DATA character={.root=&body,.object=&body,.objectNum=1};
    model_init(2,&character);
    Hu3DData[2].pos.y=6;
    spaces[1].pos.y=spaces[2].pos.y=6;
    GwPlayer[0].masuId=GwPlayer[0].masuIdNext=1;

    /* AO must not move a rendered actor independently of effects spawned at
     * its real position. This compiles the actual patched Hu3DExec draw block. */
    PSMTXScale(Hu3DCameraMtx,1,1,1);
    for (int level=0;level<=2;level++) for (int camera=0;camera<3;camera++) {
        ao=level; Hu3DCameraNo=camera;
        for (int frame=0;frame<12;frame++) {
            /* Walk off the cached lawn, jump, ride a support, change player
             * order/space association, and squash/stretch: no AO translation. */
            Hu3DData[2].pos=(HuVecF){frame*45.0f,6+frame*10.0f,frame*-15.0f};
            Hu3DData[2].scale.y=frame&1 ? 0.5f : 1.5f;
            Hu3DData[2].mtx[1][3]=frame;
            GwPlayer[0].masuId=frame&1 ? 2 : 1;
            GwPlayer[0].masuIdNext=1;
            spaces[1].useMtxF=frame&1;
            motion=frame;
            shadowModelDrawF=frame&1;
            HU3D_MODEL before=Hu3DData[2];
            production_model_draw(Hu3DData+2);
            assert(submitted[0][3]==before.pos.x && submitted[2][3]==before.pos.z);
            assert(submitted[1][3]==before.pos.y+before.scale.y*before.mtx[1][3]);
            assert(!memcmp(&before,Hu3DData+2,sizeof(before)));
            assert(mp6_ground_vertices(Hu3DData+2,&body)==feet);
            assert(feet[0].y==-1 && spaces[1].pos.y==6);
            /* An effect with the same original transform stays aligned. */
            model_init(4,&character);
            Hu3DData[4]=before;
            Mtx actor; memcpy(actor,submitted,sizeof(actor));
            production_model_draw(Hu3DData+4);
            assert(!memcmp(actor,submitted,sizeof(actor)));
        }
    }
    /* The starting floor graphic is an ancestor of the landscape. It must
     * keep its original transform and must never displace children. */
    HSF_OBJECT start=body; start.name="start";
    HSF_DATA startData={.root=&start,.object=&start,.objectNum=1};
    model_init(5,&startData);
    Hu3DData[5].pos.y=6;
    mp6_ground_w01_scenery(5);
    for (int level=0;level<=2;level++) for (int descendants=0;descendants<=1;descendants++) {
        ao=level;
        Mtx world,expected; world_at(world,6); memcpy(expected,world,sizeof(world));
        mp6_ground_object(Hu3DData+5,&start,world,descendants);
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_vertices(Hu3DData+5,&start)==feet);
    }
    /* Raise the lawn AND path to the authored start-space floor. The path is
     * also the lawn's parent: only its draw vertices may move, not its matrix. */
    HuVecF lawnVertices[5]={{-100,-10,-100},{-100,-10,100},
                          {100,-10,-100},{100,-10,100},{100,-400,100}};
    HuVecF pathVertices[4]={{-20,-3,-20},{-20,-3,20},{20,-3,-20},{20,-3,20}};
    HuVecF lawnOriginal[5],pathOriginal[4];
    memcpy(lawnOriginal,lawnVertices,sizeof(lawnOriginal));
    memcpy(pathOriginal,pathVertices,sizeof(pathOriginal));
    HSF_BUFFER lawnBuffer={.count=5,.data=lawnVertices};
    HSF_BUFFER pathBuffer={.count=4,.data=pathVertices};
    HSF_OBJECT lawn=floor; lawn.mesh.vertex=&lawnBuffer;
    HSF_OBJECT path=floor; path.name="start"; path.mesh.vertex=&pathBuffer;
    /* Far slopes/cliff bottoms and the water-level plane must receive the
     * same lift, despite not intersecting the start surface at all. */
    const char *backgroundNames[]={"haikei1","haikei10","haikei11","haikei12",
        "haikei14","haikei15","haikei16","haikei2","grid309"};
    HSF_OBJECT background[9];
    HuVecF backgroundVertices[9][4],backgroundOriginal[9][4];
    HSF_BUFFER backgroundBuffers[9];
    HSF_OBJECT *pathChildren[10]={&lawn};
    for(int i=0;i<9;i++) {
        background[i]=floor; background[i].name=(char *)backgroundNames[i];
        background[i].mesh.parent=&path; background[i].mesh.childNum=0;
        for(int j=0;j<4;j++) backgroundVertices[i][j]=(HuVecF){500+j*10,-510-j*50,900+i*100};
        memcpy(backgroundOriginal[i],backgroundVertices[i],sizeof(backgroundVertices[i]));
        backgroundBuffers[i]=(HSF_BUFFER){.count=4,.data=backgroundVertices[i]};
        background[i].mesh.vertex=&backgroundBuffers[i];
        pathChildren[i+1]=background+i;
    }
    path.mesh.childNum=10; path.mesh.child=pathChildren; lawn.mesh.parent=&path;
    HSF_OBJECT lawnLog=log; lawnLog.mesh.parent=&lawn;
    HuVecF liftedLogVertices[6]; memcpy(liftedLogVertices,logVertices,sizeof(logVertices));
    liftedLogVertices[5]=(HuVecF){0,25,0}; /* outside original repair band, inside the raised one */
    HSF_BUFFER liftedLogBuffer={.count=6,.data=liftedLogVertices};
    lawnLog.mesh.vertex=&liftedLogBuffer;
    HSF_OBJECT clover=body; clover.name="clover";
    clover.mesh.vertex=&logBuffer; clover.mesh.parent=&lawn;
    HSF_OBJECT *lawnChildren[]={&lawnLog,&clover};
    lawn.mesh.childNum=2; lawn.mesh.child=lawnChildren;
    HSF_DATA lawnData={.root=&path,.object=&path,.objectNum=1};
    model_init(6,&lawnData);
    spaces[1].pos=(HuVecF){0,6,0}; spaces[1].flag=MASU_FLAG_START; spaces[1].useMtxF=0;
    mp6_ground_w01_begin(6);
    /* The white links are a separate mesh, below the raised path unless they
     * are fitted too. Other elevations, partial supports and slopes stay put. */
    HuVecF links[16]={
        {-20,2,-20},{-20,2,20},{20,2,-20},{20,2,20},
        {-20,152,-20},{-20,152,20},{20,152,-20},{20,152,20},
        {90,2,-20},{90,2,20},{120,2,-20},{120,2,20},
        {-20,2,-20},{-20,2,20},{20,2.5f,-20},{20,2.5f,20}};
    HuVecF originalLinks[16]; memcpy(originalLinks,links,sizeof(links));
    HSF_FACE linkFaces[4]={0};
    for(int j=0;j<4;j++) {
        linkFaces[j].type=HSF_FACE_QUAD;
        for(int k=0;k<4;k++) linkFaces[j].index[k].vertex=4*j+k;
    }
    HSF_BUFFER linkVertexBuffer={.count=16,.data=links},linkFaceBuffer={.count=4,.data=linkFaces};
    HSF_OBJECT linkObject={.name="b01_m001",.type=HSF_OBJ_MESH};
    linkObject.mesh.base.scale=linkObject.mesh.curr.scale=(HuVecF){1,1,1};
    linkObject.mesh.vertex=&linkVertexBuffer; linkObject.mesh.face=&linkFaceBuffer;
    HSF_DATA linkData={.root=&linkObject,.object=&linkObject,.objectNum=1};
    model_init(7,&linkData);
    mp6_ground_w01_scenery(7);
    /* The authored W01 daisy patch is not a generic flat object: it is one
     * cutout-depth-write quad under b01_m002/flw_low2 using s3b01t42. Its
     * source remains untouched; only the registered draw copy follows the
     * calibrated lawn lift. */
    HuVecF patchVertices[4]={{-10,-5,-10},{-10,-5,10},{10,-5,-10},{10,-5,10}};
    HSF_FACE patchFace={0}; patchFace.type=HSF_FACE_QUAD; patchFace.mat=228;
    for(int i=0;i<4;i++) patchFace.index[i].vertex=i;
    HSF_BUFFER patchVertexBuffer={.count=4,.data=patchVertices};
    HSF_BUFFER patchFaceBuffer={.count=1,.data=&patchFace};
    HSF_BITMAP patchBitmap={.name="s3b01t42"};
    HSF_ATTRIBUTE patchAttribute={.bitmap=&patchBitmap};
    s32 patchAttrIndex[]={0};
    HSF_MATERIAL patchMaterials[229]={0};
    patchMaterials[228].attrNum=1; patchMaterials[228].attr=patchAttrIndex;
    patchMaterials[228].invAlpha=.309091f;
    HSF_OBJECT patchRoot={.name="b01_m002",.type=HSF_OBJ_ROOT};
    HSF_OBJECT patchParent={.name="flw_low2",.type=HSF_OBJ_ROOT};
    HSF_OBJECT patch={.name="grid439",.type=HSF_OBJ_MESH,.flags=512};
    patchRoot.mesh.base.scale=patchRoot.mesh.curr.scale=(HuVecF){1,1,1};
    patchParent.mesh.base.scale=patchParent.mesh.curr.scale=(HuVecF){1,1,1};
    patch.mesh.base.scale=patch.mesh.curr.scale=(HuVecF){1,1,1};
    patch.mesh.parent=&patchParent; patch.mesh.vertex=&patchVertexBuffer;
    patch.mesh.face=&patchFaceBuffer; patch.mesh.material=patchMaterials;
    patch.mesh.attribute=&patchAttribute;
    HSF_OBJECT *patchRootChildren[]={&patchParent};
    HSF_OBJECT *patchParentChildren[]={&patch};
    patchRoot.mesh.childNum=1; patchRoot.mesh.child=patchRootChildren;
    patchParent.mesh.childNum=1; patchParent.mesh.child=patchParentChildren;
    HSF_DATA patchData={.root=&patchRoot,.attribute=&patchAttribute,
        .material=patchMaterials,.object=&patchRoot,.materialNum=229,
        .attributeNum=1,.objectNum=3};
    model_init(8,&patchData);
    mp6_ground_w01_scenery(8);
    PSMTXScale(Hu3DCameraMtx,1,1,1);
    /* The calibrated start path is the sole opaque receiver decal. Matching
     * its name or flat artwork is insufficient: the production predicate is
     * tied to this exact registered object, model owner, unchanged hierarchy
     * and source/copy calibration. */
    HSF_OBJECT sameName=path;
    HSF_OBJECT patchClone=patch;
    for(int level=0;level<=2;level++) {
        ao=level;
        assert(mp6_ground_is_decal(Hu3DData+6,&path)==
#ifdef MP6_HEADLESS_BUILD
            0
#else
            (level!=0)
#endif
        );
        assert(!mp6_ground_is_decal(Hu3DData+6,&sameName)); /* same name/copy */
        assert(!mp6_ground_is_decal(Hu3DData+6,&lawn));
        assert(!mp6_ground_is_decal(Hu3DData+3,treeMesh));
        assert(!mp6_ground_is_decal(Hu3DData+1,&log));
        assert(!mp6_ground_is_decal(Hu3DData+2,&body));
        assert(!mp6_ground_is_decal(Hu3DData+7,&linkObject));
        assert(mp6_ground_is_decal(Hu3DData+8,&patch)==
#ifdef MP6_HEADLESS_BUILD
            0
#else
            (level!=0)
#endif
        );
        assert(!mp6_ground_is_decal(Hu3DData+8,&patchClone)); /* same shape/copy */
        HuVecF *drawLawn=mp6_ground_vertices(Hu3DData+6,&lawn);
        HuVecF *drawPath=mp6_ground_vertices(Hu3DData+6,&path);
        HuVecF *drawLinks=mp6_ground_vertices(Hu3DData+7,&linkObject);
        HuVecF *drawPatch=mp6_ground_vertices(Hu3DData+8,&patch);
#ifndef MP6_HEADLESS_BUILD
        if(level) {
            assert(drawLawn!=lawnVertices);
            assert(drawPath!=pathVertices);
            assert(drawLinks!=links);
            assert(drawPatch!=patchVertices);
            for(int i=0;i<4;i++) {
                assert(fabsf(drawPatch[i].y-(patchVertices[i].y+15.25f))<.001f);
                assert(drawPatch[i].x==patchVertices[i].x && drawPatch[i].z==patchVertices[i].z);
            }
            for(int i=0;i<4;i++) {
                assert(fabsf(drawLawn[i].y-5.25f)<.001f);
                assert(drawLawn[i].x==lawnVertices[i].x && drawLawn[i].z==lawnVertices[i].z);
                assert(fabsf(drawPath[i].y-5.75f)<.001f);
                assert(drawPath[i].x==pathVertices[i].x && drawPath[i].z==pathVertices[i].z);
                assert(fabsf(drawLinks[i].y-6.25f)<.001f);
                assert(drawLinks[i].x==links[i].x && drawLinks[i].z==links[i].z);
            }
            assert(!memcmp(drawLinks+4,links+4,12*sizeof(HuVecF)));
            assert(fabsf(drawLawn[4].y-(lawnVertices[4].y+15.25f))<.001f);
            HuVecF *drawLog=mp6_ground_vertices(Hu3DData+6,&lawnLog);
            assert(fabsf(drawLog[0].y-5.15f)<.001f); /* raise existing bases to the new lawn */
            assert(!memcmp(drawLog+2,logVertices+2,3*sizeof(HuVecF))); /* no new vertex candidates */
            assert(!memcmp(drawLog+5,liftedLogVertices+5,sizeof(HuVecF)));
            Mtx flower; world_at(flower,0);
            mp6_ground_object(Hu3DData+6,&clover,flower,1);
            assert(fabsf(flower[1][3]-5.25f)<.001f);
        } else
#endif
        { assert(drawLawn==lawnVertices); assert(drawPath==pathVertices); assert(drawLinks==links);
          assert(drawPatch==patchVertices); }
        for(int i=0;i<9;i++) {
            HuVecF *draw=mp6_ground_vertices(Hu3DData+6,background+i);
#ifndef MP6_HEADLESS_BUILD
            if(level) {
                assert(draw!=backgroundVertices[i]);
                for(int j=0;j<4;j++) {
                    assert(fabsf(draw[j].y-(backgroundVertices[i][j].y+15.25f))<.001f);
                    assert(draw[j].x==backgroundVertices[i][j].x && draw[j].z==backgroundVertices[i][j].z);
                }
            } else
#endif
            assert(draw==backgroundVertices[i]);
            assert(!memcmp(backgroundOriginal[i],backgroundVertices[i],sizeof(backgroundVertices[i])));
        }
        for(int descendants=0;descendants<=1;descendants++) {
            Mtx world,expected; world_at(world,0); memcpy(expected,world,sizeof(world));
            mp6_ground_object(Hu3DData+6,&path,world,descendants);
            assert(!memcmp(world,expected,sizeof(world)));
        }
        assert(!memcmp(lawnOriginal,lawnVertices,sizeof(lawnOriginal)));
        assert(!memcmp(pathOriginal,pathVertices,sizeof(pathOriginal)));
        assert(!memcmp(originalLinks,links,sizeof(links)));
    }
    ao=2;
    /* Object grounding only validates terrain for an exact registered root.
     * No cached validity may hide a live terrain/pose/config change. */
    {
        Mtx world, expected;
        world_at(expected,0);
        mp6_ground_test_active_calls=0;
        memcpy(world,expected,sizeof(world));
        mp6_ground_object(Hu3DData+6,&clover,world,0);
        assert(!memcmp(world,expected,sizeof(world)));
        mp6_ground_object(Hu3DData+2,&body,world,1);
        mp6_ground_object(Hu3DData+2,&clover,world,1);
        HSF_OBJECT otherClover=clover;
        mp6_ground_object(Hu3DData+6,&otherClover,world,1);
        HSF_DATA otherLawn=lawnData;
        Hu3DData[6].hsf=&otherLawn;
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        Hu3DData[6].hsf=&lawnData;
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_test_active_calls==0);
#ifndef MP6_HEADLESS_BUILD
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        assert(fabsf(world[1][3]-5.25f)<.001f);
        assert(mp6_ground_test_active_calls==1);
        memcpy(world,expected,sizeof(world));
        pathVertices[0].y=1;
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        pathVertices[0]=pathOriginal[0];
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_test_active_calls==2);
        path.mesh.curr.pos.y=1;
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        path.mesh.curr.pos.y=0;
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_test_active_calls==3);
        ao=0;
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        ao=2;
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_test_active_calls==3); /* AO-off skips active(). */
        Hu3DData[6].pos.y=1;
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        Hu3DData[6].pos.y=0;
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_test_active_calls==4);
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        assert(fabsf(world[1][3]-5.25f)<.001f);
        assert(mp6_ground_test_active_calls==5);
#else
        /* Headless object submissions skip active() before the prop scan. */
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_test_active_calls==0);
        ao=0;
        mp6_ground_object(Hu3DData+6,&clover,world,1);
        ao=2;
        assert(!memcmp(world,expected,sizeof(world)));
        assert(mp6_ground_test_active_calls==0);
#endif
    }
    /* Unrelated geometry skips the expensive lawn validation entirely. The
     * optimization must not cache its result for registered receivers. */
    mp6_ground_test_active_calls=0;
    assert(mp6_ground_vertices(Hu3DData+2,&body)==feet);
    assert(mp6_ground_test_active_calls==0);
    pathVertices[0].y=1;
    assert(mp6_ground_vertices(Hu3DData+2,&body)==feet);
    assert(mp6_ground_test_active_calls==0);
    pathVertices[0]=pathOriginal[0];
#ifndef MP6_HEADLESS_BUILD
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)!=lawnVertices);
    assert(mp6_ground_test_active_calls==1);
    pathVertices[0].y=1;
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    assert(mp6_ground_test_active_calls==2);
    pathVertices[0]=pathOriginal[0];
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)!=lawnVertices);
    assert(mp6_ground_test_active_calls==3);
    mp6_ground_test_active_calls=0;
    assert(mp6_ground_vertices(Hu3DData+8,&patch)!=patchVertices);
    assert(mp6_ground_test_active_calls==1); /* Helper validates exactly once. */
#endif
    mp6_ground_test_active_calls=0;
    ao=0;
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    ao=2;
    Hu3DData[6].attr|=HU3D_ATTR_HOOKFUNC;
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    Hu3DData[6].attr&=~HU3D_ATTR_HOOKFUNC;
    assert(mp6_ground_test_active_calls==0);
    patch.mesh.parent=&body;
    assert(!mp6_ground_is_decal(Hu3DData+8,&patch));
    patch.mesh.parent=&patchParent;
    patchVertices[0].y=-4;
    assert(!mp6_ground_is_decal(Hu3DData+8,&patch));
    patchVertices[0].y=-5;
    HSF_DATA patchReplacement=patchData;
    Hu3DData[8].hsf=&patchReplacement;
    assert(!mp6_ground_is_decal(Hu3DData+8,&patch));
    Hu3DData[8].hsf=&patchData;
    /* A matching HSF copy in the same model slot is not the registered owner. */
    HSF_DATA reusedLawn=lawnData;
    Hu3DData[6].hsf=&reusedLawn;
    assert(!mp6_ground_is_decal(Hu3DData+6,&path));
    Hu3DData[6].hsf=&lawnData;
#ifdef MP6_HEADLESS_BUILD
    assert(!mp6_ground_is_decal(Hu3DData+6,&path));
#else
    assert(mp6_ground_is_decal(Hu3DData+6,&path));
#endif
    /* Pose, source vertices, and hierarchy changes invalidate the receiver
     * even when the model slot and HSF pointer are still live. */
    path.mesh.curr.pos.y=1;
    assert(!mp6_ground_is_decal(Hu3DData+6,&path));
    path.mesh.curr.pos.y=0;
    path.mesh.parent=&body;
    assert(!mp6_ground_is_decal(Hu3DData+6,&path));
    path.mesh.parent=NULL;
    pathVertices[0].y=1;
    assert(!mp6_ground_is_decal(Hu3DData+6,&path));
    pathVertices[0]=pathOriginal[0];
#ifdef MP6_HEADLESS_BUILD
    assert(!mp6_ground_is_decal(Hu3DData+6,&path));
#else
    assert(mp6_ground_is_decal(Hu3DData+6,&path));
#endif
    ao=2;
    linkObject.mesh.curr.pos.y=1;
    assert(mp6_ground_vertices(Hu3DData+7,&linkObject)==links);
    linkObject.mesh.curr.pos.y=0;
    links[0].y=3;
    assert(mp6_ground_vertices(Hu3DData+7,&linkObject)==links);
    links[0].y=2;
    path.mesh.curr.pos.y=5;
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    path.mesh.curr.pos.y=0;
    Hu3DData[6].pos.y=5;
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    Hu3DData[6].pos.y=0;
    pathVertices[0].y=1; /* Invalidated reference shape must not leave raised lawn. */
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    pathVertices[0]=pathOriginal[0];
    pathVertices[0].x=150; /* Unsupported path: no heuristic lawn lift. */
    mp6_ground_w01_begin(6);
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    pathVertices[0]=pathOriginal[0];
    lawnVertices[0].y=-7; /* No flattening over a sloped/ridged lawn. */
    mp6_ground_w01_begin(6);
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    lawnVertices[0].y=-10;
    spaces[1].flag=0; /* No authored start-space reference: do not guess foot height. */
    mp6_ground_w01_begin(6);
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    assert(mp6_ground_vertices(Hu3DData+6,&path)==pathVertices);
    spaces[1].flag=MASU_FLAG_START;
    spaces[1].useMtxF=1; /* Moving/platform start space is not a static reference. */
    mp6_ground_w01_begin(6);
    assert(mp6_ground_vertices(Hu3DData+6,&path)==pathVertices);
    spaces[1].useMtxF=0;
    mp6_ground_w01_begin(6);
    mp6_ground_end();
    assert(mp6_ground_vertices(Hu3DData+6,&lawn)==lawnVertices);
    assert(mp6_ground_vertices(Hu3DData+1,&log)==logVertices);
    assert(!mp6_ground_is_decal(Hu3DData+6,&path)); /* registry lifetime */
    assert(!mp6_ground_is_decal(Hu3DData+8,&patch));

    /* Sky AO protection is an independent, exact terrain-object registry;
     * it must not depend on a successful lawn lift. The real W01 terrain
     * root is b01_m000 and owns exactly sora1/sora2/sora3 plus its upper
     * cloud panel. */
    HSF_OBJECT skyRoot={.name="b01_m000",.type=HSF_OBJ_ROOT};
    HSF_OBJECT sky[3]={{.name="sora1",.type=HSF_OBJ_MESH},
                       {.name="sora2",.type=HSF_OBJ_MESH},
                       {.name="sora3",.type=HSF_OBJ_MESH}};
    HSF_OBJECT cloud={.name="cloud",.type=HSF_OBJ_MESH};
    HuVecF skyVertices[3][3]={
        {{-1,1,-1},{1,1,-1},{0,1,1}},
        {{-1,2,-1},{1,2,-1},{0,2,1}},
        {{-1,3,-1},{1,3,-1},{0,3,1}}};
    HSF_BUFFER skyBuffers[3];
    HSF_OBJECT *skyChildren[3]={sky,sky+1,sky+2};
    HSF_OBJECT *cloudChildren[]={&cloud};
    skyRoot.mesh.childNum=3; skyRoot.mesh.child=skyChildren;
    for(int i=0;i<3;i++) {
        sky[i].mesh.base.scale=sky[i].mesh.curr.scale=(HuVecF){1,1,1};
        sky[i].mesh.parent=&skyRoot;
        skyBuffers[i]=(HSF_BUFFER){.count=3,.data=skyVertices[i]};
        sky[i].mesh.vertex=skyBuffers+i;
    }
    sky[0].mesh.childNum=1; sky[0].mesh.child=cloudChildren;
    HuVecF cloudVertices[3]={{-2,4,-2},{2,4,-2},{0,4,2}};
    HSF_BUFFER cloudBuffer={.count=3,.data=cloudVertices};
    cloud.mesh.base.scale=cloud.mesh.curr.scale=(HuVecF){1,1,1};
    cloud.mesh.parent=sky;
    cloud.mesh.vertex=&cloudBuffer;
    HSF_DATA skyData={.root=&skyRoot,.object=&skyRoot,.objectNum=5};
    model_init(8,&skyData);
    mp6_ground_w01_begin(8); /* no start/lawn mesh: sky registration still lives */
    ao=2;
#ifdef MP6_HEADLESS_BUILD
    assert(!mp6_ground_is_background(Hu3DData+8,sky));
    assert(!mp6_ground_is_background(Hu3DData+8,&cloud));
#else
    assert(mp6_ground_is_background(Hu3DData+8,sky));
    assert(mp6_ground_is_background(Hu3DData+8,sky+1));
    assert(mp6_ground_is_background(Hu3DData+8,sky+2));
    assert(mp6_ground_is_background(Hu3DData+8,&cloud));
#endif
    HSF_OBJECT sameSkyName=sky[0];
    assert(!mp6_ground_is_background(Hu3DData+8,&sameSkyName)); /* clone */
    assert(!mp6_ground_is_background(Hu3DData+7,sky)); /* wrong owner */
    sky[0].mesh.vertex=NULL;
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* killed source */
    sky[0].mesh.vertex=skyBuffers;
    sky[0].mesh.curr.pos.y=1;
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* animated pose */
    sky[0].mesh.curr.pos.y=0;
    sky[0].mesh.parent=NULL;
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* changed hierarchy */
    sky[0].mesh.parent=&skyRoot;
    Hu3DData[8].pos.y=1;
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* moved owner */
    Hu3DData[8].pos.y=0;
    ao=0;
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* AO gate */
    ao=2;
    Hu3DData[8].attr=HU3D_ATTR_COLOR_NOUPDATE;
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* no color update */
    Hu3DData[8].attr=0;
#ifndef MP6_HEADLESS_BUILD
    assert(mp6_ground_is_background(Hu3DData+8,sky));
#endif
    HSF_DATA reusedSky=skyData;
    Hu3DData[8].hsf=&reusedSky;
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* reused HSF */
    Hu3DData[8].hsf=&skyData;
    mp6_ground_end();
    assert(!mp6_ground_is_background(Hu3DData+8,sky)); /* registry lifetime */
    return 0;
}
