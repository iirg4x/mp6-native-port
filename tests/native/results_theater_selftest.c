#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define assert(c) do { if (!(c)) { printf("FAIL: %s\n",#c); return 1; } } while (0)
typedef int BOOL;
typedef int16_t s16; typedef int32_t s32; typedef uint16_t u16;
#define TRUE 1
#define FALSE 0
#define HSF_FACE_TRI 2
#define HSF_FACE_QUAD 3
#define HSF_FACE_TRISTRIP 4
#define HSF_FACE_MASK 7
#define MP6_WS_EXTEND_RIM_MAX 16
typedef struct {float x,y,z;} HuVecF;
typedef struct {HuVecF pos,rot,scale;} HSF_TRANSFORM;
typedef struct {s16 vertex,normal,color,st;} HSF_FACE_INDEX;
typedef struct {
    u16 typeSrc; s16 mat;
    union {HSF_FACE_INDEX index[4]; struct {HSF_FACE_INDEX index[3]; uint32_t count; HSF_FACE_INDEX *data;} strip;};
    float nbt[3];
} HSF_FACE;
typedef struct {s32 count; void *data;} HSF_BUFFER;
typedef struct {struct {
    HSF_BUFFER *vertex,*face,*st,*normal,*color;
    HSF_TRANSFORM base;
    struct {HuVecF min,max;} mesh;
} mesh;} HSF_OBJECT;
typedef struct {
    const char *name; s32 vertexCount,edgeCount,quadCount; float edgeX,posX,rotY;
} MP6WSResultsBorder;
typedef struct {
    HSF_OBJECT *obj; HuVecF *vtx; s32 baseVtx; s16 rimCount,rimVtx[16];
    float rimX[16],rimY[16],rimZ[16]; HuVecF nativeMin,nativeMax;
} MP6WSExtendEntry;
static struct {s16 vertex[10][4],st[10][4],normal[10][4],srcFace[10];} g_mp6WsPillarScratch;
#include "board_subject.inc"
int main(void) {
    HuVecF vertices[10]={{262.671f,0,10},{262.671f,250,10},{262.671f,511,10},
                         {200,0,10},{200,250,10},{200,511,10}};
    HSF_FACE faces[2]={0}, appended[2];
    for(int i=0;i<2;i++) {
        faces[i].typeSrc=HSF_FACE_TRI; faces[i].mat=7;
        faces[i].index[0]=(HSF_FACE_INDEX){i,3*i,-1,3*i};
        faces[i].index[1]=(HSF_FACE_INDEX){i+3,3*i+1,-1,3*i+1};
        faces[i].index[2]=(HSF_FACE_INDEX){i+1,3*i+2,-1,3*i+2};
    }
    HuVecF nativeVertices[6]; HSF_FACE nativeFaces[2];
    memcpy(nativeVertices,vertices,sizeof(nativeVertices)); memcpy(nativeFaces,faces,sizeof(faces));
    HSF_BUFFER vb={6,vertices},fb={2,faces},sb={6,0},nb={6,0};
    HSF_OBJECT object={0}; object.mesh.vertex=&vb; object.mesh.face=&fb;
    object.mesh.st=&sb; object.mesh.normal=&nb;
    object.mesh.base.pos.x=537.329f; object.mesh.base.scale=(HuVecF){1,1,1};
    MP6WSResultsBorder profile={"synthetic-column",6,3,2,262.671f,537.329f,0};
    MP6WSExtendEntry e={0}; e.obj=&object; e.vtx=vertices; e.baseVtx=6;
    e.nativeMin=(HuVecF){-158,0,-40}; e.nativeMax=(HuVecF){262.671f,511,10};
    assert(mp6_ws_results_border_edges(&object,&profile,&e)); assert(e.rimCount==4);
    mp6_ws_extend_write_faces_results(&e,appended,faces);
    for(int i=0;i<2;i++) {
        HSF_FACE *f=&appended[i];
        assert(f->typeSrc==HSF_FACE_QUAD && f->mat==7);
        /* The shared edge reverses native traversal. Outer UVs exactly
         * repeat that FACE's loops, including seams between source faces. */
        assert(f->index[0].vertex==i+1 && f->index[2].vertex==i);
        assert(f->index[3].vertex==6+2*i && f->index[1].vertex==7+2*i);
        assert(f->index[0].st==3*i+2 && f->index[2].st==3*i);
        assert(f->index[3].st==f->index[2].st && f->index[1].st==f->index[0].st);
        assert(f->index[3].normal==f->index[2].normal && f->index[1].normal==f->index[0].normal);
    }
    const float aspects[]={1,4.f/3.f,1.8f,1,0.8f,2};
    for(int step=0;step<6;step++) {
        float k=aspects[step], dx=800*(k>1?k-1:0);
        mp6_ws_extend_results_apply(&e,k);
        assert(!memcmp(nativeVertices,vertices,sizeof(nativeVertices)));
        assert(!memcmp(nativeFaces,faces,sizeof(faces)));
        for(int i=0;i<4;i++) {
            HuVecF v=vertices[6+i],src=vertices[e.rimVtx[i]];
            assert(fabsf(v.x-(src.x+dx))<.0002f && v.y==src.y && v.z==src.z);
            if(k<=1) assert(!memcmp(&v,&src,sizeof(v)));
        }
        assert(object.mesh.mesh.min.x==e.nativeMin.x);
        assert(fabsf(object.mesh.mesh.max.x-(e.nativeMax.x+dx))<.0002f);
    }
    profile.edgeCount++; assert(!mp6_ws_results_border_edges(&object,&profile,&e)); profile.edgeCount--;
    profile.vertexCount++; assert(!mp6_ws_results_border_edges(&object,&profile,&e)); profile.vertexCount--;
    object.mesh.base.scale.x=2; assert(!mp6_ws_results_border_edges(&object,&profile,&e)); object.mesh.base.scale.x=1;
    object.mesh.base.pos.x=0; assert(!mp6_ws_results_border_edges(&object,&profile,&e)); object.mesh.base.pos.x=profile.posX;
    faces[0].index[0].st=6; assert(!mp6_ws_results_border_edges(&object,&profile,&e)); faces[0]=nativeFaces[0];
    faces[1]=faces[0]; assert(!mp6_ws_results_border_edges(&object,&profile,&e)); faces[1]=nativeFaces[1];
    faces[0].typeSrc=HSF_FACE_TRISTRIP; faces[0].strip.count=0;
    assert(!mp6_ws_results_border_edges(&object,&profile,&e));
    puts("Results theater: pinned UVs, winding, native art, aspect resize and layout refusal PASS");
    return 0;
}
