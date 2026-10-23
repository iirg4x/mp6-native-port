#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game/hu3d.h"
#include "mp6_widescreen.h"

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
static HSF_DATA hsf;
static HSF_OBJECT objects[2];
static HuVecF vertices[24], normals[1];
static HSF_FACE faces[6];
static HSF_BUFFER vb, fb, nb;
static int enabled=1, allocs, failAlloc, invalidations;
static float scale=1.75f;
int mp6_widescreen_enabled(void) { return enabled; }
float mp6_widescreen_scale_factor(void) { return enabled ? scale : 1.0f; }
void GXInvalidateVtxCache(void) { ++invalidations; }
static void mp6_ws_extend_refuse(const char *a, const char *b) {}
static BOOL mp6_ws_model_live(HU3D_MODELID id, const HSF_DATA *data)
{ return data && id>=0 && id<HU3D_MODEL_MAX && models[id].hsf==data; }
void PSMTXIdentity(Mtx m) { memset(m,0,sizeof(Mtx));m[0][0]=m[1][1]=m[2][2]=1; }
void PSMTXScale(Mtx m,float x,float y,float z) { PSMTXIdentity(m);m[0][0]=x;m[1][1]=y;m[2][2]=z; }
void PSMTXCopy(const Mtx a,Mtx b) { memcpy(b,a,sizeof(Mtx)); }
void PSMTXConcat(const Mtx a,const Mtx b,Mtx out)
{
    Mtx tmp;
    for(int i=0;i<3;++i) for(int j=0;j<4;++j) {
        tmp[i][j]=j==3?a[i][3]:0;
        for(int k=0;k<3;++k) tmp[i][j]+=a[i][k]*b[k][j];
    }
    memcpy(out,tmp,sizeof(tmp));
}
void PSMTXMultVec(const Mtx m,const Vec *v,Vec *out)
{
    Vec p={m[0][0]*v->x+m[0][1]*v->y+m[0][2]*v->z+m[0][3],
           m[1][0]*v->x+m[1][1]*v->y+m[1][2]*v->z+m[1][3],
           m[2][0]*v->x+m[2][1]*v->y+m[2][2]*v->z+m[2][3]};
    *out=p;
}
u32 PSMTXInverse(const Mtx m,Mtx out)
{
    PSMTXIdentity(out);
    for(int i=0;i<3;++i) {
        if(m[i][i]==0) return 0;
        out[i][i]=1/m[i][i]; out[i][3]=-m[i][3]/m[i][i];
    }
    return 1;
}
void mtxRot(Mtx m,float x,float y,float z) { assert(x==0 && y==0 && z==0);PSMTXIdentity(m); }
void mtxTransCat(Mtx m,float x,float y,float z) { m[0][3]+=x;m[1][3]+=y;m[2][3]+=z; }
static void *test_malloc(size_t n) { ++allocs;return failAlloc?NULL:malloc(n); }
#define malloc test_malloc
#include "components_subject.inc"
#undef malloc

static void reset(void)
{
    for(int i=0;i<g_mp6WsSelectiveNum;++i) free(g_mp6WsSelective[i].native);
    memset(g_mp6WsSelective,0,sizeof(g_mp6WsSelective)); g_mp6WsSelectiveNum=0;
    memset(&hsf,0,sizeof(hsf)); memset(objects,0,sizeof(objects)); memset(models,0,sizeof(models));
    memset(faces,0,sizeof(faces));
    hsf.root=hsf.object=objects;hsf.objectNum=1;models[0].hsf=&hsf;
    objects[0].type=HSF_OBJ_MESH;objects[0].name="nami";
    objects[0].mesh.base.scale=(HuVecF){1,1,1};objects[0].mesh.base.pos.y=-5;
    vb=(HSF_BUFFER){.count=24,.data=vertices};fb=(HSF_BUFFER){.count=6,.data=faces};nb=(HSF_BUFFER){.count=1,.data=normals};
    objects[0].mesh.vertex=&vb;objects[0].mesh.face=&fb;objects[0].mesh.normal=&nb;
    /* Six independent pieces with INTERLEAVED indices, matching the cave's
     * three inner contacts and three outer rock shorelines. No ID ranges. */
    const float x[6]={-550,550,0,-1200,1200,0}, z[6]={0,0,0,0,0,-1200};
    for(int f=0;f<6;++f) {
        faces[f].typeSrc=f==2?HSF_FACE_TRI:HSF_FACE_QUAD;
        for(int c=0;c<4;++c) {
            int n=f+6*c;
            vertices[n]=(HuVecF){x[f]+(c&1?50:-50),-150,z[f]+(c&2?50:-50)};
            faces[f].index[c].vertex=n;
        }
    }
    enabled=1;scale=1.75f;allocs=failAlloc=invalidations=0;
    objects[0].mesh.mesh.min=(HuVecF){-1300,-150,-1300};
    objects[0].mesh.mesh.max=(HuVecF){1300,-150,100};
}

int main(void)
{
    reset(); HuVecF native[24]; HSF_FACE nativeFaces[6];
    memcpy(native,vertices,sizeof(native));memcpy(nativeFaces,faces,sizeof(faces));
    enabled=0;
    mp6_widescreen_extrude_outer_components(0,"nami",910);
    assert(allocs==1 && invalidations==1 && g_mp6WsSelectiveNum==1);
    assert(!memcmp(native,vertices,sizeof(native)));
    enabled=1;
    for(int step=0;step<100;++step) {
        scale=step%3==0?1:step%3==1?1.325f:1.75f;
        mp6_widescreen_extrude_outer_components(0,"nami",910);
        assert(allocs==1);
        for(int i=0;i<24;++i) {
            if(i%6<3 || scale==1) assert(!memcmp(&vertices[i],&native[i],sizeof(HuVecF)));
            else assert(fabsf(vertices[i].x-native[i].x*scale)<.001f);
            assert(vertices[i].y==native[i].y && vertices[i].z==native[i].z);
            assert(vertices[i].x>=objects[0].mesh.mesh.min.x && vertices[i].x<=objects[0].mesh.mesh.max.x);
        }
        assert(!memcmp(faces,nativeFaces,sizeof(faces)));
    }
    reset();failAlloc=1;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!g_mp6WsSelectiveNum && !invalidations);
    reset();faces[3].index[0].vertex=0;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs); /* connected crossing */
    reset();faces[3].index[0].vertex=24;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();faces[3].typeSrc=HSF_FACE_TRISTRIP;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();vertices[3].x=NAN;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();objects[0].mesh.cenvNum=1;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();objects[1]=objects[0];hsf.objectNum=2;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();objects[0].mesh.base.rot.y=90;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();objects[0].mesh.base.pos.x=1;mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();hsf.root=&objects[1];mp6_widescreen_extrude_outer_components(0,"nami",910);assert(!allocs);
    reset();mp6_widescreen_extrude_outer_components(0,"nami",NAN);assert(!allocs);
    reset();mp6_widescreen_extrude_outer_components(HU3D_MODEL_MAX,"nami",910);assert(!allocs);
    reset();mp6_widescreen_extrude_outer_components(0,"nami",2000);assert(!allocs);
    reset();mp6_widescreen_extrude_outer_components(0,"nami",1);assert(!allocs);
    reset();failAlloc=1;
    assert(!mp6_ws_selective_register(0,&hsf,objects,(HuVecF){0,0,0},0));assert(!g_mp6WsSelectiveNum);
    failAlloc=0;MP6WSSelectiveEntry *e=mp6_ws_selective_register(0,&hsf,objects,(HuVecF){0,0,0},0);
    assert(e && e->outerRadiusSq==0);mp6_ws_selective_apply(e,1.75f);
    assert(vertices[0].x==native[0].x*1.75f); /* original whole-object API still widens everything */
    reset();
    HSF_DATA rockHsf={0}; HSF_OBJECT rock={0}; HSF_FACE rockFaces[3]={0}; HuVecF rockV[12];
    HSF_BUFFER rockVB={.count=12,.data=rockV}, rockFB={.count=3,.data=rockFaces};
    rockHsf.root=rockHsf.object=&rock;rockHsf.objectNum=1;models[1].hsf=&rockHsf;
    rock.type=HSF_OBJ_MESH;rock.name="iwa";rock.mesh.base.scale=(HuVecF){1,1,1};
    rock.mesh.vertex=&rockVB;rock.mesh.face=&rockFB;rock.mesh.normal=&nb;
    for(int f=0;f<3;++f) for(int c=0;c<4;++c) {
        int n=f+3*c;rockFaces[f].typeSrc=HSF_FACE_QUAD;rockFaces[f].index[c].vertex=n;
        float x=f==1?-1200:f==2?1200:0, half=f==0?1600:25;
        rockV[n]=(HuVecF){x+(c&1?half:-half),-150,c&2?half:-half};
    }
    HuVecF nativeRocks[12];memcpy(nativeRocks,rockV,sizeof(rockV));memcpy(native,vertices,sizeof(native));
    MP6WSSelectiveEntry *rockEntry=mp6_ws_selective_register(1,&rockHsf,&rock,(HuVecF){0,0,0},0);
    mp6_ws_selective_apply(rockEntry,scale);
    mp6_widescreen_extrude_outer_components(0,"nami",910);
    for(int tick=0;tick<100;++tick) {
        scale=tick%3==0?1:tick%3==1?1.325f:1.75f;
        mp6_widescreen_pair_rigid_components(1,"iwa",0,"nami",500);
        assert(allocs==2);
        for(int i=0;i<12;++i) {
            float expected=i%3==0?nativeRocks[i].x*scale:nativeRocks[i].x+(scale-1)*(i%3==1?-1200:1200);
            assert(fabsf(rockV[i].x-expected)<.001f && rockV[i].y==nativeRocks[i].y && rockV[i].z==nativeRocks[i].z);
        }
        for(int i=0;i<24;++i) {
            float expected=native[i].x;
            if(i%6==3 || i%6==4) expected+=(scale-1)*(i%6==3?-1200:1200);
            else if(i%6==5) expected*=scale;
            assert(fabsf(vertices[i].x-expected)<.001f && vertices[i].y==native[i].y && vertices[i].z==native[i].z);
        }
    }
    /* The two different mesh contours share ONE translation, not their own
     * slightly different bounding-box centres. Failure cannot half-apply. */
    MP6WSSelectiveEntry *rimEntry=mp6_ws_selective_find(vertices);
    u8 rockLabels[MP6_WS_COMPONENT_VERTICES], rimLabels[MP6_WS_COMPONENT_VERTICES];
    memcpy(rockLabels,rockEntry->rigidPart,sizeof(rockLabels));memcpy(rimLabels,rimEntry->rigidPart,sizeof(rimLabels));
    rockFaces[0].typeSrc=HSF_FACE_TRISTRIP;
    mp6_widescreen_pair_rigid_components(1,"iwa",0,"nami",500);
    assert(!memcmp(rockLabels,rockEntry->rigidPart,sizeof(rockLabels)) && !memcmp(rimLabels,rimEntry->rigidPart,sizeof(rimLabels)));
    reset();
    return 0;
}
