#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int s32;
typedef unsigned char u8;
typedef float Mtx[3][4];
typedef struct { float x, y, z; } HuVecF;
typedef struct { void *data; int count; } HSF_BUFFER;
typedef struct {
    struct { HSF_BUFFER *vertex; struct { HuVecF min, max; } mesh; } mesh;
} HSF_OBJECT;
#define HEAP_HEAP 0
#define HU_MEMNUM_OVL 0
#define HU_DISP_WIDTH 576.0f
#define HU_DISP_HEIGHT 480.0f
#define HU_DISP_CENTERX (HU_DISP_WIDTH/2)
#define HU_DISP_CENTERY (HU_DISP_HEIGHT/2)

static int allocations;
static void *HuMemDirectMallocNum(int heap, size_t bytes, int tag) {
    (void)heap; (void)tag;
    allocations++;
    return malloc(bytes);
}
static void HuMemDirectFree(void *p) { allocations--; free(p); }
static float HuCos(float degrees) { return cosf(degrees * 0.017453292519943295f); }
static void MTXIdentity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    for (int i=0; i<3; ++i) m[i][i]=1;
}
static void MTXRotTrig(Mtx m, u8 axis, float sine, float cosine) {
    MTXIdentity(m);
    int a=0, b=1;
    if (axis=='x' || axis=='X') { a=1; b=2; }
    if (axis=='y' || axis=='Y') { a=2; b=0; }
    m[a][a]=cosine; m[b][b]=cosine;
    m[a][b]=-sine; m[b][a]=sine;
}

#include "board_subject.inc"

static void require(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); exit(1); }
}
static void near(float actual, float expected, const char *what) {
    require(isfinite(actual) && fabsf(actual-expected) <= 0.00001f*(1+fabsf(expected)), what);
}
static void matrix_near(Mtx actual, Mtx expected, const char *what) {
    for (int i=0; i<3; ++i) for (int j=0; j<4; ++j) near(actual[i][j], expected[i][j], what);
}
static void reference_rotate(Mtx m, char axis, float s, float c) {
    Mtx rotation, old;
    MTXRotTrig(rotation, axis, s, c);
    memcpy(old, m, sizeof(Mtx));
    for (int i=0; i<3; ++i) for (int j=0; j<4; ++j) {
        m[i][j]=0;
        for (int k=0; k<3; ++k) m[i][j] += rotation[i][k]*old[k][j];
    }
}
static float reference_trig(float angle, int radians, int sine) {
    float scale=radians ? 1303.7972412109375f : 8192.0f/360.0f;
    int byte_offset=((int)(angle*scale)+(sine ? -2046 : 2)) & 8188;
    return HuCos((360.0f/2048.0f)*(byte_offset/4));
}

int main(void) {
    mbMathInit();
    require(allocations==1, "trig table allocated once");
    const float angles[]={-720.0f,-360.1f,-95.2f,-0.04f,0,0.04f,32.6f,90,179.9f,360,719};
    void (*degree[3])(Mtx,float)={mbMtxRotXDeg,mbMtxRotYDeg,mbMtxRotZDeg};
    void (*radian[3])(Mtx,float)={mbMtxRotXRad,mbMtxRotYRad,mbMtxRotZRad};
    const Mtx seed={{2,3,4,5},{6,7,8,9},{10,11,12,13}};
    for (unsigned n=0; n<sizeof(angles)/sizeof(angles[0]); ++n) {
        float angle=angles[n];
        near(mbCosDeg(angle),reference_trig(angle,0,0),"degree cosine");
        near(mbSinDeg(angle),reference_trig(angle,0,1),"degree sine");
        near(mbCosRad(angle),reference_trig(angle,1,0),"radian cosine");
        near(mbSinRad(angle),reference_trig(angle,1,1),"radian sine");
        for (int axis=0; axis<3; ++axis) {
            Mtx actual, expected;
            for (int rad=0; rad<2; ++rad) {
                memcpy(actual,seed,sizeof(Mtx)); memcpy(expected,seed,sizeof(Mtx));
                (rad ? radian[axis] : degree[axis])(actual,angle);
                reference_rotate(expected,"xyz"[axis],reference_trig(angle,rad,1),reference_trig(angle,rad,0));
                matrix_near(actual,expected,"in-place rotation including translation");
                if (rad) mbMtxRotAxisRad(actual,"xyz"[axis],angle);
                else mbMtxRotAxisDeg(actual,"xyz"[axis],angle);
                MTXRotTrig(expected,"xyz"[axis],reference_trig(angle,rad,1),reference_trig(angle,rad,0));
                matrix_near(actual,expected,"axis rotation");
            }
            HuVecF scale={2,-3,0.5f};
            if (axis == 0) mbMtxScaleRotXDeg(actual,&scale,angle);
            else if (axis == 1) mbMtxScaleRotYDeg(actual,angle,&scale);
            else mbMtxScaleRotZDeg(actual,angle,&scale);
            MTXRotTrig(expected,"xyz"[axis],mbSinDeg(angle),mbCosDeg(angle));
            for (int i=0; i<3; ++i) {
                expected[i][0]*=scale.x; expected[i][1]*=scale.y; expected[i][2]*=scale.z;
            }
            matrix_near(actual,expected,"nonuniform rotation scale");
        }
    }
    Mtx actual, expected;
    mbMtxRot(actual,0,0,0); MTXIdentity(expected);
    matrix_near(actual,expected,"zero Euler rotation");
    mbMtxRot(actual,32,61,-17); MTXIdentity(expected);
    reference_rotate(expected,'x',mbSinDeg(32),mbCosDeg(32));
    reference_rotate(expected,'y',mbSinDeg(61),mbCosDeg(61));
    reference_rotate(expected,'z',mbSinDeg(-17),mbCosDeg(-17));
    matrix_near(actual,expected,"Euler rotation order");
    memcpy(actual,seed,sizeof(Mtx)); memcpy(expected,seed,sizeof(Mtx));
    mbMtxTransCat(actual,1.25f,-4,32);
    expected[0][3]+=1.25f; expected[1][3]-=4; expected[2][3]+=32;
    matrix_near(actual,expected,"translation preserves basis");
    // The source position can alias the translation column and next row.
    float px=actual[0][3], py=actual[1][0], pz=actual[1][1];
    MathMtxTranslationSet(actual,(HuVecF*)&actual[0][3]);
    expected[0][3]=px; expected[1][3]=py; expected[2][3]=pz;
    matrix_near(actual,expected,"translation snapshots aliased input");
    for (int x=-1; x<=1; ++x) for (int y=-1; y<=1; ++y) {
        HuVecF screen={(float)x,(float)y,-123};
        mbNormPosto2D(&screen,&screen);
        near(screen.x,(x+1)*288.0f,"normalized screen X");
        near(screen.y,(1-y)*240.0f,"normalized screen Y");
        near(screen.z,-123,"projection preserves depth");
    }
    HuVecF vertices[]={{2,-5,8},{-7,4,-9},{3,12,0},{0,-1,25}};
    HSF_BUFFER buffer={vertices,4};
    HSF_OBJECT object={0}; object.mesh.vertex=&buffer;
    ObjectBBoxUpdate(&object);
    near(object.mesh.mesh.min.x,-7,"bbox min X"); near(object.mesh.mesh.max.x,3,"bbox max X");
    near(object.mesh.mesh.min.y,-5,"bbox min Y"); near(object.mesh.mesh.max.y,12,"bbox max Y");
    near(object.mesh.mesh.min.z,-9,"bbox min Z"); near(object.mesh.mesh.max.z,25,"bbox max Z");
    buffer.count=1; ObjectBBoxUpdate(&object);
    near(object.mesh.mesh.min.y,-5,"bbox resets minimum"); near(object.mesh.mesh.max.z,8,"bbox resets maximum");
    mbMathClose();
    require(allocations==0 && cosTab==NULL,"trig table lifetime");
    puts("PASS: portable board math, projection, rotations and bounds");
    return 0;
}
