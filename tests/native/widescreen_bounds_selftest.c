#include <assert.h>
#include <math.h>
#include <string.h>
#include "game/hu3d.h"

HU3D_MODEL *Hu3DData;
int mp6_widescreen_enabled(void) { return 1; }
float mp6_widescreen_scale_factor(void) { return 1.75f; }

void PSMTXIdentity(Mtx m) { memset(m,0,sizeof(Mtx)); m[0][0]=m[1][1]=m[2][2]=1; }
void PSMTXScale(Mtx m, float x,float y,float z) { PSMTXIdentity(m); m[0][0]=x;m[1][1]=y;m[2][2]=z; }
void PSMTXConcat(const Mtx a, const Mtx b, Mtx out)
{
    Mtx tmp;
    for (int i=0;i<3;++i) for (int j=0;j<4;++j) {
        tmp[i][j]= j==3 ? a[i][3] : 0;
        for (int k=0;k<3;++k) tmp[i][j]+=a[i][k]*b[k][j];
    }
    memcpy(out,tmp,sizeof(tmp));
}
void PSMTXMultVec(const Mtx m,const Vec *v,Vec *out)
{
    Vec p = {m[0][0]*v->x+m[0][1]*v->y+m[0][2]*v->z+m[0][3],
             m[1][0]*v->x+m[1][1]*v->y+m[1][2]*v->z+m[1][3],
             m[2][0]*v->x+m[2][1]*v->y+m[2][2]*v->z+m[2][3]};
    *out=p;
}
void mtxTransCat(Mtx m,float x,float y,float z) { m[0][3]+=x;m[1][3]+=y;m[2][3]+=z; }
void mtxRot(Mtx m,float x,float y,float z)
{
    assert(x==0 && y==0);
    float a=z*0.017453292519943295f;
    PSMTXIdentity(m); m[0][0]=m[1][1]=cosf(a);m[0][1]=-sinf(a);m[1][0]=sinf(a);
}
#include "bounds_subject.inc"
int main(void)
{
    HuVecF points[4]={{-2,-1,0},{2,-1,0},{-2,1,0},{2,1,0}};
    HSF_BUFFER vertices={0}; vertices.data=points;vertices.count=4;
    HSF_OBJECT root={0}, mesh={0}, *children[1]={&mesh};
    HSF_DATA data={0}; data.root=&root;
    root.mesh.child=children;root.mesh.childNum=1;
    root.mesh.base.scale=(HuVecF){1,2,1};root.mesh.base.pos=(HuVecF){-7,5,0};
    mesh.type=HSF_OBJ_MESH;mesh.mesh.vertex=&vertices;
    mesh.mesh.base.scale=(HuVecF){2,1,1};mesh.mesh.base.pos=(HuVecF){10,3,0};mesh.mesh.base.rot.z=45;
    /* Deliberately inflated cumulative cull bounds, as in the retail assets. */
    mesh.mesh.mesh.min=(HuVecF){-10000,-10000,-10000};
    mesh.mesh.mesh.max=(HuVecF){20000,20000,20000};
    Mtx identity; PSMTXIdentity(identity);
    HuVecF lo,hi,center;BOOL any=FALSE;
    mp6_ws_accum_bbox_r(&root,identity,&lo,&hi,&any);
    assert(any);
    assert(fabsf(lo.x-(3-5/sqrtf(2)))<0.001f);
    assert(fabsf(hi.x-(3+5/sqrtf(2)))<0.001f);
    assert(fabsf(lo.y-(11-10/sqrtf(2)))<0.001f);
    assert(fabsf(hi.y-(11+10/sqrtf(2)))<0.001f);
    assert(mp6_ws_model_center(&data,&center));
    assert(fabsf(center.x-3)<0.001f && fabsf(center.y-11)<0.001f && center.z==0);
    mesh.name="sky";
    MP6WSCenterWidenCtx ctx={0};ctx.name="sky";
    mp6_ws_center_widened_r(&root,identity,&ctx);
    assert(ctx.anyObj && !ctx.anyOther);
    assert(fabsf(ctx.objMinX-lo.x)<0.001f && fabsf(ctx.objMaxX-hi.x)<0.001f);
    memset(&ctx,0,sizeof(ctx));ctx.name="other";
    mp6_ws_center_widened_r(&root,identity,&ctx);
    assert(!ctx.anyObj && ctx.anyOther);
    assert(fabsf(ctx.otherMin.x-lo.x)<0.001f && fabsf(ctx.otherMax.y-hi.y)<0.001f);
    vertices.data=NULL;
    assert(!mp6_ws_model_center(&data,&center));
    return 0;
}
