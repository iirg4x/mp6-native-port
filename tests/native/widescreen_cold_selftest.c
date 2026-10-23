#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game/hu3d.h"
#include "mp6_widescreen.h"

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
static HSF_DATA data[7];
static HSF_OBJECT objects[7];
static HSF_BUFFER vb[7], uvb[7];
static HuVecF vertices[7][4], native[7][4], boundsMin[7], boundsMax[7];
static HuVec2f uvs[7][4], nativeUV[7][4];
static int enabled, invalidations, cameraUpdates, extensionUpdates;
static int motionReapplyCalls, motionForgetCalls, tileForgetCalls;
static float factor = 1.75f;
int mp6_widescreen_enabled(void) { return enabled; }
float mp6_widescreen_scale_factor(void) { return enabled ? factor : 1.0f; }
void mp6_widescreen_motion_reapply(void) { ++motionReapplyCalls; }
void mp6_widescreen_motion_forget(int16_t modelId) { (void)modelId; ++motionForgetCalls; }
void mp6_widescreen_tile_forget(int16_t modelId) { (void)modelId; ++tileForgetCalls; }
void GXInvalidateVtxCache(void) { ++invalidations; }
void mp6_widescreen_camera_reapply(void) { ++cameraUpdates; }
static void mp6_ws_extend_reapply_all(float k) { ++extensionUpdates; }
static void mp6_ws_outer_x_reapply_all(float k) { (void)k; } /* own fixture-free contract test */
void Hu3DModelPosGet(HU3D_MODELID id, HuVecF *p) { *p=models[id].pos; }
void Hu3DModelScaleGet(HU3D_MODELID id, HuVecF *p) { *p=models[id].scale; }
void Hu3DModelPosSet(HU3D_MODELID id,float x,float y,float z) { models[id].pos=(HuVecF){x,y,z}; }
void Hu3DModelScaleSet(HU3D_MODELID id,float x,float y,float z) { models[id].scale=(HuVecF){x,y,z}; }
void Hu3DModelAttrSet(HU3D_MODELID id,u32 flags) { models[id].attr|=flags; }
void Hu3DModelAttrReset(HU3D_MODELID id,u32 flags) { models[id].attr&=~flags; }
void PSMTXIdentity(Mtx m) { memset(m,0,sizeof(Mtx));m[0][0]=m[1][1]=m[2][2]=1; }
void PSMTXCopy(const Mtx a,Mtx b) { memcpy(b,a,sizeof(Mtx)); }
void PSMTXScale(Mtx m,float x,float y,float z) { PSMTXIdentity(m);m[0][0]=x;m[1][1]=y;m[2][2]=z; }
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
        out[i][i]=1/m[i][i];out[i][3]=-m[i][3]/m[i][i];
    }
    return 1;
}
void mtxRot(Mtx m,float x,float y,float z) { assert(x==0 && y==0 && z==0);PSMTXIdentity(m); }
void mtxTransCat(Mtx m,float x,float y,float z) { m[0][3]+=x;m[1][3]+=y;m[2][3]+=z; }
#include "cold_subject.inc"

static BOOL match(const char *name) { return strcmp(name,"plane")==0; }
static void register_all(void)
{
    mp6_widescreen_extrude_model(0);
    mp6_widescreen_extrude_model_repeat(1);
    mp6_widescreen_extrude_model_selective_horizontal(2,match);
    mp6_widescreen_extrude_model_reposition_x_clamped(3,match,2,0,15);
    mp6_widescreen_extrude_model_border_xz(4,"plane");
    mp6_widescreen_split_separate_x(5);
}
static void native_unchanged(void)
{
    assert(!memcmp(vertices,native,sizeof(native)));
    assert(!memcmp(uvs,nativeUV,sizeof(nativeUV)));
    assert(models[0].pos.x==3 && models[0].pos.y==4 && models[0].pos.z==5);
    assert(models[0].scale.x==1 && models[0].scale.y==1 && models[0].scale.z==1);
    assert(objects[3].mesh.base.pos.x==7);
    for(int i=0;i<7;++i) {
        assert(!memcmp(&objects[i].mesh.mesh.min,&boundsMin[i],sizeof(HuVecF)));
        assert(!memcmp(&objects[i].mesh.mesh.max,&boundsMax[i],sizeof(HuVecF)));
    }
}
int main(void)
{
    for(int id=0;id<7;++id) {
        models[id].hsf=&data[id]; models[id].scale=(HuVecF){1,1,1};
        models[id].pos=(HuVecF){3,4,5};
        data[id].root=data[id].object=&objects[id];data[id].objectNum=1;
        objects[id].type=HSF_OBJ_MESH;objects[id].name="plane";
        objects[id].mesh.base.scale=(HuVecF){1,1,1};
        objects[id].mesh.vertex=&vb[id];objects[id].mesh.st=&uvb[id];
        vb[id]=(HSF_BUFFER){.count=4,.data=vertices[id]};
        uvb[id]=(HSF_BUFFER){.count=4,.data=uvs[id]};
        for(int v=0;v<4;++v) {
            vertices[id][v]=(HuVecF){v&1?6.75f:-1.25f,3.5f,v&2?4.5f:-3.5f};
            uvs[id][v]=(HuVec2f){v&1?1.3f:-0.2f,v&2?1.1f:-0.1f};
        }
        objects[id].mesh.mesh.min=boundsMin[id]=(HuVecF){-1.25f,3.5f,-3.5f};
        objects[id].mesh.mesh.max=boundsMax[id]=(HuVecF){6.75f,3.5f,4.5f};
    }
    objects[3].mesh.base.pos.x=7;
    /* Retail cull bounds can be larger than the actual vertex bounds. */
    objects[2].mesh.mesh.min=boundsMin[2]=(HuVecF){-100,-100,-100};
    objects[2].mesh.mesh.max=boundsMax[2]=(HuVecF){100,100,100};
    memcpy(native,vertices,sizeof(native));memcpy(nativeUV,uvs,sizeof(nativeUV));
    /* The real cold-disabled call sites must retain native snapshots. */
    register_all();
    assert(g_mp6WsModelNum==1 && g_mp6WsRepeatNum==4 && g_mp6WsSelectiveNum==1);
    assert(g_mp6WsRepositionNum==1 && g_mp6WsBorderNum==1 && g_mp6WsSplitNum==1);
    native_unchanged();
    /* Floor fill has independent visibility ownership. */
    g_mp6WsFloorDupNum=1;g_mp6WsFloorDup[0].dupModelId=6;g_mp6WsFloorDup[0].dupHsf=&data[6];
    models[6].attr=HU3D_ATTR_DISPOFF | HU3D_MOTATTR_LOOP;
    mp6_widescreen_reapply();
    for(int cycle=0;cycle<100;++cycle) {
        enabled=1;factor=cycle%2?1.325f:1.75f;mp6_widescreen_reapply();
        assert(models[0].scale.x==factor);
        assert(vertices[1][0].x < native[1][0].x && uvs[1][0].x < nativeUV[1][0].x);
        assert(vertices[2][0].x < native[2][0].x && objects[3].mesh.base.pos.x>=15);
        assert(vertices[4][0].x < native[4][0].x && vertices[5][0].x < native[5][0].x);
        assert(!(models[6].attr & HU3D_ATTR_DISPOFF) && (models[6].attr & HU3D_MOTATTR_LOOP));
        enabled=0;mp6_widescreen_reapply();native_unchanged();
        assert(models[6].attr & HU3D_ATTR_DISPOFF);
        int oldInvalidations=invalidations,oldExtensions=extensionUpdates,oldCamera=cameraUpdates;
        mp6_widescreen_reapply();
        assert(invalidations==oldInvalidations && extensionUpdates==oldExtensions);
        assert(cameraUpdates==oldCamera+1); /* native cameras still track animation */
    }
    register_all();native_unchanged(); /* duplicate registration cannot accumulate */
    assert(g_mp6WsModelNum==1 && g_mp6WsRepeatNum==4 && g_mp6WsSplitNum==1);
    for(int id=0;id<7;++id) mp6_widescreen_forget_model(id);
    enabled=1;mp6_widescreen_reapply();native_unchanged(); /* no stale resurrection */
    return 0;
}
