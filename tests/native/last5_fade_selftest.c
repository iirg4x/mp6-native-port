/* Execute the actual fade hook: its plane needs positions, never mesh UVs. */
#include "dolphin.h"
#include "game/hu3d.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int base_setup, base_stages, base_gens, num_stages, num_gens;
static int gen_id, gen_type, gen_src, gen_matrix, gen_normalize, gen_post;
static int order_stage, order_coord, order_map, order_channel, alpha_ref;
static Mtx loaded_matrix;
Mtx Hu3DCameraMtx;

static void qa_identity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    for (int i=0;i<3;i++) m[i][i]=1;
}
static void qa_trans(Mtx m, float x, float y, float z) {
    qa_identity(m); m[0][3]=x; m[1][3]=y; m[2][3]=z;
}
static void qa_scale(Mtx m, float x, float y, float z) {
    memset(m, 0, sizeof(Mtx)); m[0][0]=x; m[1][1]=y; m[2][2]=z;
}
static void qa_concat(const Mtx a, const Mtx b, Mtx out) {
    Mtx tmp;
    for (int i=0;i<3;i++) for (int j=0;j<4;j++) {
        tmp[i][j]=j==3 ? a[i][3] : 0;
        for (int k=0;k<3;k++) tmp[i][j]+=a[i][k]*b[k][j];
    }
    memcpy(out,tmp,sizeof(tmp));
}
/* Test inputs use rigid transforms, so inverse is transpose plus translation. */
static void qa_inverse(const Mtx m, Mtx out) {
    Mtx tmp;
    for (int i=0;i<3;i++) {
        tmp[i][3]=0;
        for (int j=0;j<3;j++) {tmp[i][j]=m[j][i];tmp[i][3]-=m[j][i]*m[j][3];}
    }
    memcpy(out,tmp,sizeof(tmp));
}
static void qa_rotation(Mtx m, float x, float y, float z) {
    (void)x;(void)y; /* Fixture exercises rotation about the fade plane's Z axis. */
    float angle=z*0.017453292519943295f;
    qa_identity(m);m[0][0]=m[1][1]=cosf(angle);m[0][1]=-sinf(angle);m[1][0]=sinf(angle);
}
static void GetStarNoTexTevStage(HU3D_DRAW_OBJ *d, HSF_MATERIAL *m, int *s, int *t) {
    (void)d;(void)m;*s=base_stages;*t=base_gens;
}
#define GetStarTexTevStage GetStarNoTexTevStage
#define Hu3DTevStageNoTexSet(d,m) (base_setup=0)
#define Hu3DTevStageTexSet(d,m) (base_setup=1)
#define HuSprTexLoad(...) ((void)0)
#define PSMTXInverse qa_inverse
#define PSMTXConcat qa_concat
#define PSMTXTrans qa_trans
#define PSMTXScale qa_scale
#define mbMtxRot qa_rotation
#define GXLoadTexMtxImm(m,id,t) memcpy(loaded_matrix,m,sizeof(Mtx))
#define GXSetNumTexGens(n) (num_gens=(n))
#define GXSetNumTevStages(n) (num_stages=(n))
#define GXSetTexCoordGen2(i,t,s,m,n,p) (gen_id=(i),gen_type=(t),gen_src=(s),gen_matrix=(m),gen_normalize=(n),gen_post=(p))
#define GXSetTevOrder(s,c,m,ch) (order_stage=(s),order_coord=(c),order_map=(m),order_channel=(ch))
#define GXSetTevKColor(...) ((void)0)
#define GXSetTevKColorSel(...) ((void)0)
#define GXSetTevColorIn(...) ((void)0)
#define GXSetTevColorOp(...) ((void)0)
#define GXSetTevAlphaIn(...) ((void)0)
#define GXSetTevAlphaOp(...) ((void)0)
#define GXSetAlphaCompare(c,r,o,c2,r2) (alpha_ref=(r))
#include "board_subject.inc"

#define CHECK(c) do {if (!(c)) {printf("FAIL line %d: %s\n",__LINE__,#c);return 1;}} while(0)
#define NEAR(a,b) CHECK(fabsf((a)-(b))<0.0001f)
int main(void) {
    MBOBJFADEWORK work={0};
    HU3D_MODEL model={0};
    HSF_OBJECT object={0};
    HSF_MATERIAL material={0};
    HU3D_DRAW_OBJ draw={0};
    model.hookData=&work;draw.model=&model;draw.object=&object;
    work.pos=(HuVecF){100,200,300};work.alpha=0.5f;
    qa_trans(Hu3DCameraMtx,-400,-500,-600);
    Mtx model_world;
    qa_trans(model_world,100,200,300);
    qa_concat(Hu3DCameraMtx,model_world,draw.matrix);
    for (int textured=0;textured<2;textured++) for (int extra=0;extra<2;extra++) {
        material.attrNum=textured;
        base_stages=1+2*extra;base_gens=textured+2*extra;
        FadeMatHook(&draw,&material);
        CHECK(base_setup==textured);
        CHECK(gen_src==GX_TG_POS); /* Old hook asks for unavailable GX_VA_TEX0. */
        CHECK(gen_type==GX_TG_MTX2x4 && gen_matrix==GX_TEXMTX4);
        CHECK(!gen_normalize && gen_post==GX_PTIDENTITY);
        CHECK(gen_id==base_gens && num_gens==base_gens+1 && num_stages==base_stages+1);
        CHECK(order_stage==base_stages && order_coord==base_gens);
        CHECK(order_map==GX_TEXMAP4 && order_channel==GX_COLOR_NULL && alpha_ref==1);
        NEAR(loaded_matrix[0][0],0.001f);NEAR(loaded_matrix[1][1],-0.02f);
        NEAR(loaded_matrix[0][3],0);NEAR(loaded_matrix[1][3],0.96875f);
        /* Two positions 25 units apart must sample different fade-plane heights. */
        NEAR(loaded_matrix[1][1]*25+loaded_matrix[1][3],0.46875f);
    }
    object.flags=HSF_MATERIAL_NEAR;
    work.rot.z=90;
    FadeMatHook(&draw,&material);
    CHECK(alpha_ref==128);
    NEAR(loaded_matrix[1][0],0.02f);NEAR(loaded_matrix[1][1],0);
    puts("Last Five Turns fade: position input, textured/untextured materials and world plane: PASS");
    return 0;
}
