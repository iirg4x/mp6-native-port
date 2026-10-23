#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game/hu3d.h"
#include "mp6_widescreen.h"
#include "edge_types.inc"

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
static HSF_DATA hsf;
static HSF_OBJECT object;
static HSF_BUFFER vertex, face, color, normal;
static HSF_MATERIAL material;
static HSF_FACE_INDEX stripTail[3];
static MP6WSExtendEntry entry;
static float scale = 1.75f;
static int enabled=1, allocs, failAt, bakes, registered, refused;
int mp6_widescreen_enabled(void) { return enabled; }
float mp6_widescreen_scale_factor(void) { return enabled ? scale : 1.0f; }
void GXInvalidateVtxCache(void) {}
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 tag)
{
    assert(heap == HEAP_MODEL && tag == 123 && bytes > 0);
    return ++allocs == failAt ? NULL : malloc(bytes);
}
void HuMemDirectFree(void *p) { assert(p); free(p); }
void mp6_gxarray_register(const void *p, uint32_t bytes) { assert(p && bytes); ++registered; }
void MakeDisplayList(HU3D_MODELID id, u32 tag) { assert(id==0 && tag==123); ++bakes; }
static MP6WSExtendEntry *mp6_ws_extend_find(HSF_OBJECT *o) { return entry.obj == o ? &entry : NULL; }
static MP6WSExtendEntry *mp6_ws_extend_alloc(void) { return &entry; }
static void mp6_ws_extend_refuse(const char *name, const char *why) { ++refused; }
#include "color_subject.inc"

static void reset(int axis)
{
    free(vertex.data); free(face.data); free(color.data); free(normal.data);
    memset(&object,0,sizeof(object)); memset(&entry,0,sizeof(entry));
    memset(&hsf,0,sizeof(hsf)); memset(models,0,sizeof(models));
    hsf.object=&object; hsf.objectNum=1; models[0].hsf=&hsf; models[0].mallocNo=123;
    object.type=HSF_OBJ_MESH; object.name="gradient";
    object.mesh.vertex=&vertex; object.mesh.face=&face;
    object.mesh.color=&color; object.mesh.normal=&normal;
    vertex.count=color.count=9; face.count=8; normal.count=1;
    vertex.data=calloc(9,sizeof(HuVecF)); color.data=calloc(9,sizeof(GXColor));
    normal.data=calloc(1,sizeof(HuVecF)); face.data=calloc(8,sizeof(HSF_FACE));
    /* Inner four-triangle gradient + outer four-quad gradient, no ST buffer.
     * Distinct rim colors check exact per-corner color continuation. */
    const float coords[9][2]={{7,-3},{6,-4},{8,-4},{8,-2},{6,-2},
                             {5,-5},{9,-5},{9,-1},{5,-1}};
    for (int i=0;i<9;++i) {
        HuVecF p={coords[i][0],4,coords[i][1]};
        if(axis==2) p=(HuVecF){p.x,p.z,p.y};
        ((HuVecF *)vertex.data)[i]=p;
        ((GXColor *)color.data)[i]=(GXColor){20+i*20,230-i*15,190,255};
    }
    for(int i=0;i<4;++i) {
        int next=(i+1)%4;
        HSF_FACE *tri=(HSF_FACE *)face.data+i, *quad=tri+4;
        tri->typeSrc=HSF_FACE_TRI; quad->typeSrc=HSF_FACE_QUAD;
        tri->mat=quad->mat=3;
        /* Actual emitted TRI 0,2,1 and QUAD 0,2,3,1. */
        int t[3]={0,next+1,i+1}, q[4]={i+1,i+5,next+1,next+5};
        for(int c=0;c<4;++c) {
            quad->index[c]=(HSF_FACE_INDEX){.vertex=q[c],.normal=0,.color=q[c],.st=-1};
            if(c<3) tri->index[c]=(HSF_FACE_INDEX){.vertex=t[c],.normal=0,.color=t[c],.st=-1};
        }
    }
    object.mesh.mesh.min=(HuVecF){5,axis==1?4:-5,axis==1?-5:4};
    object.mesh.mesh.max=(HuVecF){9,axis==1?4:-1,axis==1?-1:4};
    enabled=1; scale=1.75f; allocs=failAt=bakes=registered=refused=0;
}

static void reset_strip(void)
{
    reset(1);
    memset(&material, 0, sizeof(material));
    material.vtxMode=1;
    hsf.materialNum=1; object.mesh.material=&material;
    vertex.count=6; face.count=1;
    for(int i=0;i<6;++i) {
        ((HuVecF *)vertex.data)[i]=(HuVecF){(i/2-1)*2, (i%2)*2-1, i/2==1?-1:0};
    }
    HSF_FACE *f=face.data;
    memset(f,0,sizeof(*f));
    f->typeSrc=HSF_FACE_TRISTRIP;
    const int first[3]={0,2,1};
    for(int i=0;i<3;++i) {
        f->index[i]=(HSF_FACE_INDEX){.vertex=first[i],.normal=0,.color=first[i],.st=-1};
        stripTail[i]=(HSF_FACE_INDEX){.vertex=i+3,.normal=0,.color=i+3,.st=-1};
    }
    f->strip.count=3; f->strip.data=stripTail;
    object.mesh.mesh.min=(HuVecF){-2,-1,-1};
    object.mesh.mesh.max=(HuVecF){2,1,0};
}

static void test_backdrop_strip(void)
{
    reset_strip();
    HuVecF nativeV[6], nativeMin=object.mesh.mesh.min, nativeMax=object.mesh.mesh.max;
    HSF_FACE nativeF=*(HSF_FACE *)face.data;
    HSF_FACE_INDEX nativeTail[3];
    memcpy(nativeV,vertex.data,sizeof(nativeV)); memcpy(nativeTail,stripTail,sizeof(nativeTail));
    enabled=0;
    mp6_widescreen_extend_backdrop_strip(0,"gradient",2);
    assert(bakes==1 && allocs==2 && registered==1 && !refused);
    assert(vertex.count==10 && face.count==3 && !object.mesh.st && entry.edgeCount==2);
    assert(!memcmp(nativeV,vertex.data,sizeof(nativeV)));
    for(int i=0;i<2;++i) for(int j=0;j<2;++j)
        assert(!memcmp(&((HuVecF *)vertex.data)[6+i*2+j],&entry.edges[i].pos[j],sizeof(HuVecF)));
    enabled=1;
    for(int n=0;n<100;++n) {
        scale=n%3==0?1:n%3==1?1.75f:2;
        mp6_widescreen_extend_backdrop_strip(0,"gradient",2);
        assert(!memcmp(nativeV,vertex.data,sizeof(nativeV)));
        assert(!memcmp(&nativeF,face.data,sizeof(nativeF)));
        assert(!memcmp(nativeTail,stripTail,sizeof(nativeTail)));
        assert(bakes==1 && allocs==2 && registered==1);
        for(int i=0;i<2;++i) {
            HSF_FACE *f=(HSF_FACE *)face.data+i+1;
            assert(f->typeSrc==HSF_FACE_QUAD && f->mat==0);
            for(int c=0;c<4;++c) assert(f->index[c].normal==0 && f->index[c].st==-1);
            assert(f->index[0].color==f->index[1].color && f->index[2].color==f->index[3].color);
            for(int j=0;j<2;++j) {
                HuVecF p=((HuVecF *)vertex.data)[6+i*2+j], old=entry.edges[i].pos[j];
                assert(p.x==old.x*(1+(scale-1)*2) && p.y==old.y && p.z==old.z);
            }
            HuVecF a=((HuVecF *)vertex.data)[f->index[0].vertex];
            HuVecF b=((HuVecF *)vertex.data)[f->index[2].vertex];
            HuVecF c=((HuVecF *)vertex.data)[f->index[3].vertex];
            /* Both side panels face the same direction as the native strip. */
            float crossZ=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
            assert(scale==1?crossZ==0:crossZ<0);
        }
        if(scale==1) {
            assert(!memcmp(&nativeMin,&object.mesh.mesh.min,sizeof(nativeMin)));
            assert(!memcmp(&nativeMax,&object.mesh.mesh.max,sizeof(nativeMax)));
        }
    }
    for(int fail=1;fail<=2;++fail) {
        reset_strip(); failAt=fail;
        void *v=vertex.data,*f=face.data;
        mp6_widescreen_extend_backdrop_strip(0,"gradient",2);
        assert(vertex.data==v && face.data==f && !entry.obj && !bakes && !registered);
    }
    /* Reject incompatible/deformed assets before allocating or changing them. */
    for(int invalid=0;invalid<13;++invalid) {
        reset_strip();
        HSF_FACE *f=face.data;
        switch(invalid) {
        case 0: material.attrNum=1; break;
        case 1: material.invAlpha=0.7f; break;
        case 2: material.vtxMode=5; break;
        case 3: material.pass=1; break;
        case 4: f->mat=1; break;
        case 5: f->strip.data=NULL; break;
        case 6: stripTail[2].vertex=0; break;
        case 7: stripTail[2].normal=1; break;
        case 8: ((HuVecF *)vertex.data)[3].x+=1; break;
        case 9: ((HuVecF *)vertex.data)[3].y+=1; break;
        case 10: ((HuVecF *)vertex.data)[2].x=((HuVecF *)vertex.data)[3].x=3; break;
        case 11: ((HuVecF *)vertex.data)[4].z=NAN; break;
        case 12: object.mesh.cenvNum=1; break;
        }
        mp6_widescreen_extend_backdrop_strip(0,"gradient",2);
        assert(!allocs && !bakes && !entry.obj);
    }
    reset_strip(); mp6_widescreen_extend_backdrop_strip(0,"gradient",NAN); assert(!allocs);
    reset_strip(); mp6_widescreen_extend_backdrop_strip(0,"gradient",0); assert(!allocs);
}

static void test_native_bottom(void)
{
    reset(2);
    HuVecF nativeV[9]; HSF_FACE nativeF[8]; GXColor nativeC[9];
    memcpy(nativeV,vertex.data,sizeof nativeV);
    memcpy(nativeF,face.data,sizeof nativeF);
    memcpy(nativeC,color.data,sizeof nativeC);
    /* Unlike aspect-only margins, the short native asset is repaired even
     * when widescreen is off. Resizing must not accumulate Y or X changes. */
    for(int step=0;step<100;++step) {
        enabled=step%3 != 0; scale=step%2 ? 1.75f : 1.333333f;
        float k=mp6_widescreen_scale_factor();
        mp6_widescreen_extend_color_rim_bottom(0,"gradient",7,-3,-7);
        assert(bakes==1 && allocs==2 && registered==1 && !refused);
        assert(!memcmp(nativeV,vertex.data,sizeof nativeV));
        assert(!memcmp(nativeF,face.data,sizeof nativeF));
        assert(!memcmp(nativeC,color.data,sizeof nativeC));
        assert(object.mesh.mesh.min.y==-7 && object.mesh.mesh.max.y==-1);
        int bottomEdges=0;
        for(int i=0;i<entry.edgeCount;++i) {
            HSF_FACE *f=(HSF_FACE *)face.data+8+i;
            assert(f->index[0].color==f->index[1].color);
            assert(f->index[2].color==f->index[3].color);
            int bottom=0;
            for(int j=0;j<2;++j) {
                HuVecF old=entry.edges[i].pos[j], p=((HuVecF *)vertex.data)[9+2*i+j];
                assert(p.x==7+(old.x-7)*k && p.z==old.z);
                assert(p.y==(old.y==-5 ? -7 : old.y));
                bottom+=old.y==-5;
            }
            bottomEdges+=bottom==2;
        }
        assert(bottomEdges==1);
    }
    reset(2);
    mp6_widescreen_extend_color_rim_bottom(0,"gradient",7,-3,NAN);
    assert(!allocs && !entry.obj);
}

int main(void)
{
    /* Explicit duplicate selection must not silently change first-match
     * behavior or bypass native deformation/shared-buffer validation. */
    reset(2);
    HSF_OBJECT duplicates[2] = {0};
    duplicates[0].type = HSF_OBJ_MESH;
    duplicates[0].name = "gradient";
    duplicates[1] = object;
    hsf.object = duplicates;
    hsf.objectNum = 2;
    assert(mp6_ws_extend_locate_mesh(0,"gradient",FALSE) == NULL);
    assert(mp6_ws_extend_locate_mesh_at(0,"gradient",FALSE,1) == &duplicates[1]);
    assert(mp6_ws_extend_locate_mesh_at(0,"gradient",FALSE,-1) == NULL);
    assert(mp6_ws_extend_locate_mesh_at(0,"gradient",FALSE,2) == NULL);
    duplicates[0].mesh.vertex = duplicates[1].mesh.vertex;
    assert(mp6_ws_extend_locate_mesh_at(0,"gradient",FALSE,1) == NULL);
    duplicates[0].mesh.vertex = NULL;
    duplicates[1].mesh.cenvNum = 1;
    assert(mp6_ws_extend_locate_mesh_at(0,"gradient",FALSE,1) == NULL);

    test_backdrop_strip();
    test_native_bottom();
    for(int axis=1;axis<=2;++axis) {
        reset(axis);
        HuVecF nativeV[9], nativeMin=object.mesh.mesh.min, nativeMax=object.mesh.mesh.max;
        HSF_FACE nativeF[8]; GXColor nativeC[9];
        memcpy(nativeV,vertex.data,sizeof(nativeV)); memcpy(nativeF,face.data,sizeof(nativeF));
        memcpy(nativeC,color.data,sizeof(nativeC));
        enabled=0;
        mp6_widescreen_extend_color_rim(0,"gradient",axis,7,-3);
        assert(bakes==1 && allocs==2 && registered==1 && !refused);
        assert(vertex.count==17 && face.count==12 && !object.mesh.st && entry.edgeCount==4);
        assert(!memcmp(nativeV,vertex.data,sizeof(nativeV)) && !memcmp(nativeF,face.data,sizeof(nativeF)));
        assert(!memcmp(nativeC,color.data,sizeof(nativeC)) && color.count==9);
        for(int i=0;i<4;++i) for(int j=0;j<2;++j)
            assert(!memcmp(&((HuVecF *)vertex.data)[9+i*2+j],&entry.edges[i].pos[j],sizeof(HuVecF)));
        enabled=1; mp6_widescreen_extend_color_rim(0,"gradient",axis,7,-3);
        for(int i=8;i<12;++i) {
            HSF_FACE *f=(HSF_FACE *)face.data+i;
            assert(f->index[0].color==f->index[1].color && f->index[2].color==f->index[3].color);
            for(int c=0;c<4;++c) assert(f->index[c].st==-1 && f->index[c].normal==0);
            for(int j=0;j<2;++j) {
                HuVecF p=((HuVecF *)vertex.data)[9+(i-8)*2+j], old=entry.edges[i-8].pos[j];
                assert(p.x==7+(old.x-7)*scale && p.y==old.y && p.z==old.z);
            }
        }
        enabled=0; mp6_widescreen_extend_color_rim(0,"gradient",axis,7,-3);
        assert(bakes==1 && allocs==2);
        assert(!memcmp(&nativeMin,&object.mesh.mesh.min,sizeof(nativeMin)));
        assert(!memcmp(&nativeMax,&object.mesh.mesh.max,sizeof(nativeMax)));
        for(int i=0;i<4;++i) for(int j=0;j<2;++j)
            assert(!memcmp(&((HuVecF *)vertex.data)[9+i*2+j],&entry.edges[i].pos[j],sizeof(HuVecF)));
    }
    for(int fail=1;fail<=2;++fail) {
        reset(1); failAt=fail;
        void *v=vertex.data,*f=face.data;
        mp6_widescreen_extend_color_rim(0,"gradient",1,7,-3);
        assert(vertex.data==v && face.data==f && !entry.obj && !bakes && !registered);
    }
    reset(1); mp6_widescreen_extend_color_rim(HU3D_MODEL_MAX,"gradient",1,7,-3); assert(!allocs);
    reset(1); ((HuVecF *)vertex.data)[3].y+=1; mp6_widescreen_extend_color_rim(0,"gradient",1,7,-3); assert(!allocs);
    reset(1); ((HSF_FACE *)face.data)[0].index[0].color=9; mp6_widescreen_extend_color_rim(0,"gradient",1,7,-3); assert(!allocs);
    reset(1); object.mesh.cenvNum=1; mp6_widescreen_extend_color_rim(0,"gradient",1,7,-3); assert(!allocs && refused);
    reset(1); ((HSF_FACE *)face.data)[0].typeSrc=HSF_FACE_TRISTRIP; mp6_widescreen_extend_color_rim(0,"gradient",1,7,-3); assert(!allocs);
    reset(1); mp6_widescreen_extend_color_rim(0,"gradient",0,7,-3); assert(!allocs);
    reset(1); mp6_widescreen_extend_color_rim(0,"gradient",1,NAN,-3); assert(!allocs);
    free(vertex.data); free(face.data); free(color.data); free(normal.data);
    return 0;
}
