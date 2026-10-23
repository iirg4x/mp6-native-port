#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game/hu3d.h"
#include "mp6_widescreen.h"
#include "extrude_types.inc"

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
static HSF_DATA hsf;
static HSF_OBJECT object;
static HSF_BUFFER vertex, normal, st, face;
static HuVecF normalF32Data[1];
static HSF_S8VEC normalS8Data[1];
static MP6WSExtendEntry entry;
static float scale = 1.0f;
void InitVtxParm(HSF_DATA *data) { assert(data == &hsf); }
void EnvelopeProc(HSF_DATA *data) { assert(data == &hsf); }
static int grows, refuses, kills, attrSets, rebakes;

float mp6_widescreen_scale_factor(void) { return scale; }
void GXInvalidateVtxCache(void) {}
void Hu3DModelAttrSet(HU3D_MODELID modelId, u32 attr)
{
    assert(modelId >= 0 && modelId < HU3D_MODEL_MAX);
    models[modelId].attr |= attr;
    ++attrSets;
}
void MakeDisplayList(HU3D_MODELID modelId, u32 mallocNo)
{
    assert(modelId >= 0 && modelId < HU3D_MODEL_MAX);
    (void)mallocNo;
    ++rebakes;
}

static HSF_OBJECT *mp6_ws_extend_locate(s16 modelId, const char *name)
{
    if (modelId != 0 || models[0].hsf == NULL || name == NULL ||
        strcmp(name, "meshstage_si13") != 0) {
        return NULL;
    }
    return &object;
}

static HSF_OBJECT *mp6_ws_extend_locate_mesh_at(s16 modelId, const char *name, BOOL needsST, int occurrence)
{
    assert(needsST);
    return occurrence == 0 ? mp6_ws_extend_locate(modelId, name) : NULL;
}

static MP6WSExtendEntry *mp6_ws_extend_find(HSF_OBJECT *candidate)
{
    return entry.obj == candidate && entry.hsf == models[0].hsf ? &entry : NULL;
}

static MP6WSExtendEntry *mp6_ws_extend_find_mode(HSF_OBJECT *candidate,
                                                  MP6WSExtendMode mode)
{
    return entry.obj == candidate && entry.hsf == models[0].hsf && entry.mode == mode
        ? &entry : NULL;
}

static MP6WSExtendEntry *mp6_ws_extend_alloc(void)
{
    memset(&entry, 0, sizeof(entry));
    return &entry;
}

static void mp6_ws_extend_refuse(const char *name, const char *why)
{
    (void)name;
    (void)why;
    ++refuses;
}

static BOOL mp6_ws_extend_grow(s16 modelId, HSF_OBJECT *o, s32 addVtx, s32 addSt,
    s32 addQuads, void (*writer)(MP6WSExtendEntry *, HSF_FACE *, const HSF_FACE *),
    MP6WSExtendEntry *e)
{
    HSF_FACE *newFaces;
    (void)modelId;
    (void)o;
    newFaces = calloc((size_t)(face.count + addQuads), sizeof(*newFaces));
    assert(newFaces != NULL);
    memcpy(newFaces, face.data, (size_t)face.count * sizeof(*newFaces));
    writer(e, newFaces + face.count, face.data);
    free(face.data);
    face.data = newFaces;
    face.count += addQuads;
    vertex.data = realloc(vertex.data, (size_t)(vertex.count + addVtx) * sizeof(HuVecF));
    st.data = realloc(st.data, (size_t)(st.count + addSt) * sizeof(HuVec2f));
    assert(vertex.data != NULL && st.data != NULL);
    vertex.count += addVtx;
    st.count += addSt;
    ++grows;
    return TRUE;
}

#include "extrude_subject.inc"

enum { NATIVE_VERTICES = 15, NATIVE_FACES = 4 };
static HuVecF nativeVertices[NATIVE_VERTICES];
static HuVec2f nativeSt[NATIVE_VERTICES];
static HSF_FACE nativeFaces[NATIVE_FACES];
static HuVecF nativeMin, nativeMax;
enum { WALL_VERTICES = 15, WALL_FACES = 4 };
static HuVecF wallNativeVertices[WALL_VERTICES];
static HuVec2f wallNativeSt[WALL_VERTICES];
static HSF_FACE wallNativeFaces[WALL_FACES];
static HuVecF wallNativeMin, wallNativeMax;
enum { REFLECT_VERTICES = 8, REFLECT_FACES = 2 };
static HuVecF reflectNativeVertices[REFLECT_VERTICES];
static HuVec2f reflectNativeSt[REFLECT_VERTICES];
static HSF_FACE reflectNativeFaces[REFLECT_FACES];
static HuVecF reflectNativeMin, reflectNativeMax;

static void set_face(HSF_FACE *out, int type, int material, const int *indices, int count)
{
    memset(out, 0, sizeof(*out));
    out->typeSrc = (u16)type;
    out->mat = (s16)material;
    for (int i = 0; i < count; ++i) {
        out->index[i].vertex = (s16)indices[i];
        out->index[i].st = (s16)indices[i];
        out->index[i].normal = 0;
        out->index[i].color = -1;
    }
}

static void reset_fixture(void)
{
    static const HuVecF points[NATIVE_VERTICES] = {
        /* selected material 3: sloped QUAD at the left edge */
        {-10.0f, 0.0f, -2.0f}, {-10.0f, 1.0f, 2.0f},
        {-6.0f, 1.5f, 2.0f}, {-6.0f, 0.5f, -2.0f},
        /* selected material 3: sloped TRI at the right edge */
        {10.0f, 0.0f, -2.0f}, {10.0f, 0.8f, 2.0f}, {8.0f, 1.2f, 2.0f},
        /* material 7 scenery beyond the selected floor's outer planes */
        {-20.0f, 0.0f, -2.0f}, {-20.0f, 0.4f, 2.0f},
        {-16.0f, 0.6f, 2.0f}, {-16.0f, 0.2f, -2.0f},
        {16.0f, 0.0f, -2.0f}, {16.0f, 0.4f, 2.0f},
        {20.0f, 0.6f, 2.0f}, {20.0f, 0.2f, -2.0f}
    };
    static const int faceIndices[NATIVE_FACES][4] = {
        {0, 1, 2, 3}, {4, 5, 6, 0}, {7, 8, 9, 10}, {11, 12, 13, 14}
    };

    free(vertex.data);
    free(st.data);
    free(face.data);
    memset(&hsf, 0, sizeof(hsf));
    memset(&object, 0, sizeof(object));
    memset(&entry, 0, sizeof(entry));
    memset(models, 0, sizeof(models));
    vertex.count = st.count = NATIVE_VERTICES;
    face.count = NATIVE_FACES;
    vertex.data = calloc(NATIVE_VERTICES, sizeof(HuVecF));
    st.data = calloc(NATIVE_VERTICES, sizeof(HuVec2f));
    face.data = calloc(NATIVE_FACES, sizeof(HSF_FACE));
    assert(vertex.data != NULL && st.data != NULL && face.data != NULL);
    for (int i = 0; i < NATIVE_VERTICES; ++i) {
        ((HuVecF *)vertex.data)[i] = points[i];
        ((HuVec2f *)st.data)[i] = (HuVec2f){points[i].x * 0.1f, points[i].z * 0.25f};
    }
    set_face(&((HSF_FACE *)face.data)[0], HSF_FACE_QUAD, 3, faceIndices[0], 4);
    set_face(&((HSF_FACE *)face.data)[1], HSF_FACE_TRI, 3, faceIndices[1], 3);
    set_face(&((HSF_FACE *)face.data)[2], HSF_FACE_QUAD, 7, faceIndices[2], 4);
    set_face(&((HSF_FACE *)face.data)[3], HSF_FACE_QUAD, 7, faceIndices[3], 4);

    object.name = "meshstage_si13";
    object.type = HSF_OBJ_MESH;
    object.mesh.vertex = &vertex;
    object.mesh.normal = &normal;
    object.mesh.st = &st;
    object.mesh.face = &face;
    object.mesh.mesh.min = (HuVecF){-20.0f, 0.0f, -2.0f};
    object.mesh.mesh.max = (HuVecF){20.0f, 1.5f, 2.0f};
    hsf.materialNum = 8;
    models[0].hsf = &hsf;
    memcpy(nativeVertices, vertex.data, sizeof(nativeVertices));
    memcpy(nativeSt, st.data, sizeof(nativeSt));
    memcpy(nativeFaces, face.data, sizeof(nativeFaces));
    nativeMin = object.mesh.mesh.min;
    nativeMax = object.mesh.mesh.max;
    scale = 1.0f;
    normalS8Data[0] = (HSF_S8VEC){0, 64, 0};
    normal.count = 1;
    normal.data = normalS8Data;
    hsf.cenvNum = 0;
    grows = refuses = attrSets = rebakes = 0;
}

static void reset_wall_fixture(void)
{
    static const HuVecF points[WALL_VERTICES] = {
        /* selected material 3: XY QUAD leaning in Z at the left edge */
        {-10.0f, 0.0f, -2.0f}, {-10.0f, 4.0f, 2.0f},
        {-6.0f, 4.0f, 3.0f}, {-6.0f, 0.0f, -1.0f},
        /* selected material 3: XY TRI leaning in Z at the right edge */
        {10.0f, 0.0f, 1.0f}, {10.0f, 4.0f, 5.0f}, {6.0f, 0.0f, 0.0f},
        /* material 7 scenery beyond the selected wall's outer planes */
        {-20.0f, 0.0f, -3.0f}, {-20.0f, 4.0f, 1.0f},
        {-16.0f, 4.0f, 2.0f}, {-16.0f, 0.0f, -2.0f},
        {16.0f, 0.0f, 0.0f}, {16.0f, 4.0f, 4.0f},
        {20.0f, 4.0f, 6.0f}, {20.0f, 0.0f, 2.0f}
    };
    static const int faceIndices[WALL_FACES][4] = {
        {0, 1, 2, 3}, {4, 5, 6, 0}, {7, 8, 9, 10}, {11, 12, 13, 14}
    };

    free(vertex.data);
    free(st.data);
    free(face.data);
    memset(&hsf, 0, sizeof(hsf));
    memset(&object, 0, sizeof(object));
    memset(&entry, 0, sizeof(entry));
    memset(models, 0, sizeof(models));
    vertex.count = st.count = WALL_VERTICES;
    face.count = WALL_FACES;
    vertex.data = calloc(WALL_VERTICES, sizeof(HuVecF));
    st.data = calloc(WALL_VERTICES, sizeof(HuVec2f));
    face.data = calloc(WALL_FACES, sizeof(HSF_FACE));
    assert(vertex.data != NULL && st.data != NULL && face.data != NULL);
    for (int i = 0; i < WALL_VERTICES; ++i) {
        ((HuVecF *)vertex.data)[i] = points[i];
        /* UVs vary in X/Y so wall continuation checks density in both axes. */
        ((HuVec2f *)st.data)[i] = (HuVec2f){points[i].x * 0.1f, points[i].y * 0.2f};
    }
    set_face(&((HSF_FACE *)face.data)[0], HSF_FACE_QUAD, 3, faceIndices[0], 4);
    set_face(&((HSF_FACE *)face.data)[1], HSF_FACE_TRI, 3, faceIndices[1], 3);
    set_face(&((HSF_FACE *)face.data)[2], HSF_FACE_QUAD, 7, faceIndices[2], 4);
    set_face(&((HSF_FACE *)face.data)[3], HSF_FACE_QUAD, 7, faceIndices[3], 4);

    object.name = "meshstage_si13";
    object.type = HSF_OBJ_MESH;
    object.mesh.vertex = &vertex;
    object.mesh.normal = &normal;
    object.mesh.st = &st;
    object.mesh.face = &face;
    object.mesh.mesh.min = (HuVecF){-20.0f, 0.0f, -3.0f};
    object.mesh.mesh.max = (HuVecF){20.0f, 4.0f, 6.0f};
    object.mesh.cenvNum = 2;
    object.mesh.clusterNum = 3;
    object.mesh.shapeNum = 4;
    hsf.materialNum = 8;
    hsf.objectNum = 1;
    hsf.object = &object;
    hsf.cenvNum = 2;
    hsf.clusterNum = 3;
    models[0].hsf = &hsf;
    models[0].linkMdlId = HU3D_MODELID_NONE;
    memcpy(wallNativeVertices, vertex.data, sizeof(wallNativeVertices));
    memcpy(wallNativeSt, st.data, sizeof(wallNativeSt));
    memcpy(wallNativeFaces, face.data, sizeof(wallNativeFaces));
    wallNativeMin = object.mesh.mesh.min;
    wallNativeMax = object.mesh.mesh.max;
    scale = 1.0f;
    normalF32Data[0] = (HuVecF){0.0f, 1.0f, 0.0f};
    normal.count = 1;
    normal.data = normalF32Data;
    grows = refuses = attrSets = rebakes = 0;
}

static void reset_reflect_fixture(void)
{
    static const HuVecF points[REFLECT_VERTICES] = {
        /* Oblique XZ seam: v0--v1. All other selected corners lie on one side. */
        {2.0f, 0.0f, -1.0f}, {3.0f, 0.0f, -5.0f},
        {-1.0f, 0.0f, -2.0f}, {-1.0f, 0.0f, -6.0f},
        {0.0f, 0.0f, -3.0f}, {1.0f, 0.0f, -4.0f},
        {0.0f, 0.0f, -3.0f}, {1.0f, 0.0f, -4.0f}
    };
    static const int faceIndices[REFLECT_FACES][4] = {
        {2, 3, 6, 7}, {6, 7, 0, 1}
    };

    free(vertex.data);
    free(st.data);
    free(face.data);
    memset(&hsf, 0, sizeof(hsf));
    memset(&object, 0, sizeof(object));
    memset(&entry, 0, sizeof(entry));
    memset(models, 0, sizeof(models));
    vertex.count = st.count = REFLECT_VERTICES;
    face.count = REFLECT_FACES;
    vertex.data = calloc(REFLECT_VERTICES, sizeof(HuVecF));
    st.data = calloc(REFLECT_VERTICES, sizeof(HuVec2f));
    face.data = calloc(REFLECT_FACES, sizeof(HSF_FACE));
    assert(vertex.data != NULL && st.data != NULL && face.data != NULL);
    for (int i = 0; i < REFLECT_VERTICES; ++i) {
        ((HuVecF *)vertex.data)[i] = points[i];
        ((HuVec2f *)st.data)[i] = (HuVec2f){0.1f * i, 0.05f * i};
    }
    set_face(&((HSF_FACE *)face.data)[0], HSF_FACE_QUAD, 6, faceIndices[0], 4);
    set_face(&((HSF_FACE *)face.data)[1], HSF_FACE_QUAD, 6, faceIndices[1], 4);

    object.name = "meshstage_si13";
    object.type = HSF_OBJ_MESH;
    object.mesh.vertex = &vertex;
    object.mesh.normal = &normal;
    object.mesh.st = &st;
    object.mesh.face = &face;
    object.mesh.mesh.min = (HuVecF){-1.0f, 0.0f, -6.0f};
    object.mesh.mesh.max = (HuVecF){3.0f, 0.0f, -1.0f};
    object.mesh.base.scale = (HuVecF){1.0f, 1.0f, 1.0f};
    normal.count = 1;
    normalS8Data[0] = (HSF_S8VEC){0, 64, 0};
    normal.data = normalS8Data;
    hsf.materialNum = 8;
    hsf.objectNum = 1;
    hsf.object = &object;
    models[0].hsf = &hsf;
    models[0].linkMdlId = HU3D_MODELID_NONE;
    memcpy(reflectNativeVertices, vertex.data, sizeof(reflectNativeVertices));
    memcpy(reflectNativeSt, st.data, sizeof(reflectNativeSt));
    memcpy(reflectNativeFaces, face.data, sizeof(reflectNativeFaces));
    reflectNativeMin = object.mesh.mesh.min;
    reflectNativeMax = object.mesh.mesh.max;
    scale = 1.0f;
    grows = refuses = attrSets = rebakes = 0;
}

static void assert_native_prefix(void)
{
    assert(memcmp(nativeVertices, vertex.data, sizeof(nativeVertices)) == 0);
    assert(memcmp(nativeSt, st.data, sizeof(nativeSt)) == 0);
    assert(memcmp(nativeFaces, face.data, sizeof(nativeFaces)) == 0);
}

static void assert_wall_native_prefix(void)
{
    assert(memcmp(wallNativeVertices, vertex.data, sizeof(wallNativeVertices)) == 0);
    assert(memcmp(wallNativeSt, st.data, sizeof(wallNativeSt)) == 0);
    assert(memcmp(wallNativeFaces, face.data, sizeof(wallNativeFaces)) == 0);
}

static void assert_wall_edges(float expectedScale, float reach, int mirror)
{
    float distance = (expectedScale > 1.0f ? expectedScale - 1.0f : 0.0f) * reach;
    assert(entry.edgeCount == 2);
    for (int i = 0; i < entry.edgeCount; ++i) {
        MP6WSEdge *edge = &entry.edges[i];
        int base = WALL_VERTICES + i * 2;
        float sourceSpan = edge->pos[0].x;
        /* Material 7's right wall deliberately has twice the X depth slope. */
        float depthSlope = sourceSpan > 15.0f ? 0.5f : 0.25f;
        float expectedUvSlope = fabsf(edge->pos[0].x) * 0.1f;
        if (edge->pos[0].x > 0.0f) expectedUvSlope = -expectedUvSlope;
        assert(fabsf(fabsf(edge->pos[0].x) - fabsf(edge->pos[1].x)) < 0.001f);
        assert(fabsf(edge->pos[0].x - edge->pos[1].x) < 0.001f);
        assert(fabsf(edge->posSlope[0].x - sourceSpan) < 0.001f);
        assert(fabsf(edge->posSlope[1].x - sourceSpan) < 0.001f);
        assert(fabsf(edge->posSlope[0].y) < 0.001f);
        assert(fabsf(edge->posSlope[1].y) < 0.001f);
        assert(fabsf(edge->posSlope[0].z - sourceSpan * depthSlope) < 0.001f);
        assert(fabsf(edge->posSlope[1].z - sourceSpan * depthSlope) < 0.001f);
        for (int j = 0; j < 2; ++j) {
            HuVecF p = ((HuVecF *)vertex.data)[base + j];
            HuVec2f uv = ((HuVec2f *)st.data)[base + j];
            HuVecF source = edge->pos[j];
            HuVec2f sourceUv = edge->uv[j];
            assert(fabsf(p.x - (source.x + distance * edge->posSlope[j].x)) < 0.001f);
            assert(fabsf(p.y - source.y) < 0.001f);
            assert(fabsf(p.z - (source.z + distance * edge->posSlope[j].z)) < 0.001f);
            assert(fabsf(uv.x - (sourceUv.x + distance * edge->uvSlope[j].x)) < 0.001f);
            assert(fabsf(uv.y - (sourceUv.y + distance * edge->uvSlope[j].y)) < 0.001f);
            if (mirror) {
                assert(fabsf(edge->uvSlope[j].x - expectedUvSlope) < 0.001f);
                assert(fabsf(edge->uvSlope[j].y) < 0.001f);
            }
        }
    }
}

static void assert_static_margin_result(void)
{
    HSF_FACE *faces = face.data;
    assert(memcmp(wallNativeVertices, vertex.data, sizeof(wallNativeVertices)) == 0);
    assert(memcmp(wallNativeSt, st.data, sizeof(wallNativeSt)) == 0);
    assert(face.count == WALL_FACES + entry.edgeCount);
    for (int f = 0; f < WALL_FACES; ++f) {
        for (int c = 1; c < 4; ++c) {
            assert(faces[f].index[c].vertex == faces[f].index[0].vertex);
            assert(faces[f].index[c].st == faces[f].index[0].st);
            assert(faces[f].index[c].normal == faces[f].index[0].normal);
            assert(faces[f].index[c].color == faces[f].index[0].color);
        }
    }
    for (int i = 0; i < entry.edgeCount; ++i) {
        HSF_FACE *tail = &faces[WALL_FACES + i];
        assert(fabsf(fabsf(entry.edges[i].pos[0].x) - 10.0f) < 0.001f);
        assert((tail->typeSrc & HSF_FACE_MASK) == HSF_FACE_QUAD);
        assert(tail->index[1].vertex >= WALL_VERTICES);
        assert(tail->index[3].vertex >= WALL_VERTICES);
    }
    assert((models[0].attr & HU3D_ATTR_ENVELOPE_OFF) != 0);
    assert(hsf.cenvNum == 2 && hsf.clusterNum == 3);
    assert(object.mesh.cenvNum == 2 && object.mesh.clusterNum == 3 && object.mesh.shapeNum == 4);
    assert(object.flags == 0);
    assert(attrSets == 1 && rebakes == 1);
}

static void assert_selected_edges(float expectedScale)
{
    assert(entry.edgeCount == 2);
    for (int i = 0; i < entry.edgeCount; ++i) {
        MP6WSEdge *edge = &entry.edges[i];
        int base = NATIVE_VERTICES + i * 2;
        assert(fabsf(fabsf(edge->pos[0].x) - 10.0f) < 0.001f);
        assert(fabsf(edge->pos[0].x - edge->pos[1].x) < 0.001f);
        for (int j = 0; j < 2; ++j) {
            HuVecF p = ((HuVecF *)vertex.data)[base + j];
            HuVec2f uv = ((HuVec2f *)st.data)[base + j];
            HuVecF source = edge->pos[j];
            HuVec2f sourceUv = edge->uv[j];
            float deltaX = source.x * (expectedScale - 1.0f);
            assert(fabsf(p.x - source.x * expectedScale) < 0.001f);
            assert(fabsf(p.y - source.y) < 0.001f && fabsf(p.z - source.z) < 0.001f);
            assert(fabsf(p.x - source.x - deltaX) < 0.001f);
            assert(fabsf(uv.x - (sourceUv.x + deltaX * 0.1f)) < 0.001f);
            assert(fabsf(uv.y - sourceUv.y) < 0.001f);
        }
    }
}

static HuVecF reflect_fixture_point(HuVecF p)
{
    HuVecF a = reflectNativeVertices[0];
    HuVecF b = reflectNativeVertices[1];
    float dx = b.x - a.x;
    float dz = b.z - a.z;
    float length = sqrtf(dx * dx + dz * dz);
    float nx = -dz / length;
    float nz = dx / length;
    float distance = (p.x - a.x) * nx + (p.z - a.z) * nz;
    p.x -= 2.0f * distance * nx;
    p.z -= 2.0f * distance * nz;
    return p;
}

static void assert_reflected_faces(float expectedScale)
{
    HSF_FACE *faces = (HSF_FACE *)face.data;
    const int emission[4] = {0, 2, 3, 1};
    const int reflectedEmission[4] = {0, 1, 3, 2};
    HuVecF seamMid = {
        (reflectNativeVertices[0].x + reflectNativeVertices[1].x) * 0.5f,
        (reflectNativeVertices[0].y + reflectNativeVertices[1].y) * 0.5f,
        (reflectNativeVertices[0].z + reflectNativeVertices[1].z) * 0.5f
    };
    assert(entry.mode == MP6_WS_EXTEND_REFLECT_FACES);
    assert(entry.reflectFaceCount == REFLECT_FACES);
    assert(vertex.count == REFLECT_VERTICES + REFLECT_FACES * 4);
    assert(st.count == REFLECT_VERTICES);
    assert(normal.count == 1);
    assert(normal.data == (hsf.cenvNum != 0 ? (void *)normalF32Data : (void *)normalS8Data));
    assert(face.count == REFLECT_FACES * 2);
    assert(memcmp(reflectNativeVertices, vertex.data, sizeof(reflectNativeVertices)) == 0);
    assert(memcmp(reflectNativeSt, st.data, sizeof(reflectNativeSt)) == 0);
    assert(memcmp(reflectNativeFaces, face.data, sizeof(reflectNativeFaces)) == 0);
    for (int f = 0; f < REFLECT_FACES; ++f) {
        HSF_FACE *tail = &faces[REFLECT_FACES + f];
        for (int c = 0; c < 4; ++c) {
            HuVecF expected = expectedScale > 1.0f
                ? reflect_fixture_point(reflectNativeVertices[reflectNativeFaces[f].index[c].vertex])
                : seamMid;
            HuVecF actual = ((HuVecF *)vertex.data)[REFLECT_VERTICES + f * 4 + c];
            assert(fabsf(actual.x - expected.x) < 0.001f);
            assert(fabsf(actual.y - expected.y) < 0.001f);
            assert(fabsf(actual.z - expected.z) < 0.001f);
        }
        for (int e = 0; e < 4; ++e) {
            int tailSlot = emission[e];
            int sourceSlot = reflectedEmission[e];
            assert(tail->index[tailSlot].vertex
                   == REFLECT_VERTICES + f * 4 + sourceSlot);
            assert(tail->index[tailSlot].st
                   == reflectNativeFaces[f].index[sourceSlot].st);
        }
    }
}

static void kill_fixture(void)
{
    models[0].hsf = NULL;
    entry.obj = NULL;
    entry.hsf = NULL;
    free(vertex.data);
    free(st.data);
    free(face.data);
    vertex.data = st.data = face.data = NULL;
    ++kills;
}

int main(void)
{
    /* M675's asymmetric trapezoid: reflect its left silhouette, not an
     * extrapolated slope. Native corners, STs, normals and winding survive. */
    reset_fixture();
    vertex.count = st.count = 4; face.count = 1;
    HuVecF hill[4] = {{-500,-500,0},{9675,-500,0},{9675,1477,0},{-500,3150,0}};
    memcpy(vertex.data, hill, sizeof(hill));
    normalS8Data[0] = (HSF_S8VEC){0,0,127};
    HSF_FACE hillFace = {0};
    const int hillOrder[4] = {3,0,2,1};
    set_face(&hillFace, HSF_FACE_QUAD, 3, hillOrder, 4);
    memcpy(face.data, &hillFace, sizeof(hillFace));
    HuVec2f hillUV[4]; memcpy(hillUV, st.data, sizeof(hillUV));
    object.mesh.base.scale = (HuVecF){.735f,.612f,.6f};
    mp6_widescreen_extend_billboard_mirror(0, "meshstage_si13", -1);
    assert(grows == 1 && vertex.count == 8 && face.count == 2 && st.count == 4);
    for (int step = 0; step < 3; ++step) {
        scale = step == 1 ? 1.75f : 1.0f;
        mp6_widescreen_extend_billboard_mirror(0, "meshstage_si13", -1);
        assert(grows == 1 && memcmp(vertex.data, hill, sizeof(hill)) == 0);
        assert(memcmp(st.data, hillUV, sizeof(hillUV)) == 0);
        assert(memcmp(face.data, &hillFace, sizeof(hillFace)) == 0);
        HSF_FACE *mirror = &((HSF_FACE *)face.data)[1];
        const int reverse[4] = {0,2,1,3};
        for (int c = 0; c < 4; ++c) {
            HuVecF p = ((HuVecF *)vertex.data)[mirror->index[c].vertex];
            HuVecF original = hill[hillFace.index[reverse[c]].vertex];
            if (scale > 1) {
                assert(fabsf(p.x - (-1000-original.x)) < .01f);
                assert(p.y == original.y && p.z == original.z);
            } else assert(p.x == -500);
            assert(mirror->index[c].st == hillFace.index[reverse[c]].st);
        }
    }

    /* S03's bottom caps are parallelogram QUADs, not triangle fans. Every
     * outside edge must grow in XZ; a side-only extension leaves a dark join. */
    reset_fixture();
    vertex.count = st.count = 4;
    face.count = 1;
    HuVecF cap[4] = {{-4,0,1}, {-2,0,3}, {2,0,-3}, {4,0,-1}};
    memcpy(vertex.data, cap, sizeof(cap));
    for (int i = 0; i < 4; ++i)
        ((HuVec2f *)st.data)[i] = (HuVec2f){cap[i].x*0.1f,cap[i].z*0.1f};
    HSF_FACE capFace;
    const int quadIndices[4] = {0,1,2,3};
    set_face(&capFace, HSF_FACE_QUAD, 3, quadIndices, 4);
    memcpy(face.data, &capFace, sizeof(capFace));
    for (int step = 0; step < 3; ++step) {
        scale = step == 1 ? 1.75f : 1.0f;
        mp6_widescreen_extend_floor(0, "meshstage_si13", 0, 0, 0);
        assert(grows == 1 && entry.edgeCount == 4 && face.count == 5);
        assert(memcmp(vertex.data, cap, sizeof(cap)) == 0);
        assert(memcmp(face.data, &capFace, sizeof(capFace)) == 0);
        for (int e = 0; e < 4; ++e)
            for (int j = 0; j < 2; ++j) {
                HuVecF p = ((HuVecF *)vertex.data)[4+e*2+j];
                assert(fabsf(p.x-entry.edges[e].pos[j].x*scale) < 0.0001f);
                assert(fabsf(p.z-entry.edges[e].pos[j].z*scale) < 0.0001f);
                assert(p.y == 0);
            }
    }

    reset_fixture();
    vertex.count = st.count = 4;
    face.count = 1;
    memcpy(vertex.data, cap, sizeof(cap));
    memcpy(face.data, &capFace, sizeof(capFace));
    scale = 1.75f;
    mp6_widescreen_extend_floor_at(0, "meshstage_si13", 0, 0, 0, 0, 2);
    assert(grows == 1 && entry.edgeCount == 4);
    assert(memcmp(vertex.data, cap, sizeof(cap)) == 0);
    for (int e = 0; e < 4; ++e)
        for (int j = 0; j < 2; ++j) {
            HuVecF p = ((HuVecF *)vertex.data)[4+e*2+j];
            assert(fabsf(p.x-entry.edges[e].pos[j].x*2.5f) < 0.0001f);
            assert(fabsf(p.z-entry.edges[e].pos[j].z*2.5f) < 0.0001f);
        }

    /* A flared XY wall has no constant-X outside edge. Its actual silhouette
     * grows without extending internal joints or moving authored corners. */
    reset_fixture();
    vertex.count = st.count = 4;
    face.count = 1;
    HuVecF taper[4] = {{-3,0,-1}, {-5,4,0}, {3,0,-1}, {5,4,0}};
    memcpy(vertex.data, taper, sizeof(taper));
    for (int i = 0; i < 4; ++i)
        ((HuVec2f *)st.data)[i] = (HuVec2f){taper[i].x*0.1f,taper[i].y*0.1f};
    memcpy(face.data, &capFace, sizeof(capFace));
    scale = 1.75f;
    mp6_widescreen_extend_wall_outer_sides(0, "meshstage_si13", 0, 1, 1);
    assert(grows == 1 && entry.edgeCount == 2 && face.count == 3);
    assert(memcmp(vertex.data, taper, sizeof(taper)) == 0);
    assert(memcmp(face.data, &capFace, sizeof(capFace)) == 0);
    for (int e = 0; e < 2; ++e)
        for (int j = 0; j < 2; ++j) {
            HuVecF p = ((HuVecF *)vertex.data)[4+e*2+j];
            assert(fabsf(p.x-entry.edges[e].pos[j].x*scale) < 0.0001f);
            assert(p.y == entry.edges[e].pos[j].y);
        }

    reset_fixture();

    /* Invalid public filters must not fall back to the unfiltered path. */
    mp6_widescreen_extend_terrain_outer_sides_material(0, "meshstage_si13",
                                                       0.0f, 1.0f, 0, -1);
    mp6_widescreen_extend_terrain_outer_sides_material(0, "meshstage_si13",
                                                       0.0f, 1.0f, 0, 0x1000);
    assert(grows == 0);
    assert_native_prefix();

    /* The material-filtered path sees the selected sloped TRI/QUAD pair only;
     * the farther material-7 room pieces cannot move the selected planes. */
    mp6_widescreen_extend_terrain_outer_sides_material(0, "meshstage_si13",
                                                       0.0f, 1.0f, 0, 3);
    assert(refuses == 0 && grows == 1);
    assert(vertex.count == NATIVE_VERTICES + 4 && st.count == NATIVE_VERTICES + 4);
    assert(face.count == NATIVE_FACES + 2);
    assert_native_prefix();
    assert_selected_edges(1.0f);
    assert(memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)) == 0);
    assert(memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)) == 0);

    scale = 1.75f;
    for (int tick = 0; tick < 32; ++tick) {
        mp6_widescreen_extend_terrain_outer_sides_material(0, "meshstage_si13",
                                                           0.0f, 1.0f, 0, 3);
        assert(grows == 1);
        assert_native_prefix();
        assert_selected_edges(1.75f);
    }

    /* Returning to native aspect restores the generated tail and cull box. */
    scale = 1.0f;
    mp6_widescreen_extend_terrain_outer_sides_material(0, "meshstage_si13",
                                                       0.0f, 1.0f, 0, 3);
    assert_native_prefix();
    assert_selected_edges(1.0f);
    assert(memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)) == 0);
    assert(memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)) == 0);

    /* The wall planner handles a mixed TRI/QUAD XY surface whose Z plane
     * leans with X/Y. Material 7 sits farther out but must not affect the
     * selected material-3 boundary, and mirror sampling reverses seam UVs. */
    reset_wall_fixture();
    mp6_widescreen_extend_wall_outer_sides(0, "meshstage_si13", 0.0f, 1.0f, 1);
    assert(refuses == 0 && grows == 1);
    assert(vertex.count == WALL_VERTICES + 4 && st.count == WALL_VERTICES + 4);
    assert(face.count == WALL_FACES + 2);
    assert_wall_native_prefix();
    assert_wall_edges(1.0f, 1.0f, 1);
    assert(memcmp(&wallNativeMin, &object.mesh.mesh.min, sizeof(wallNativeMin)) == 0);
    assert(memcmp(&wallNativeMax, &object.mesh.mesh.max, sizeof(wallNativeMax)) == 0);

    scale = 1.5f;
    mp6_widescreen_extend_wall_outer_sides(0, "meshstage_si13", 0.0f, 1.0f, 1);
    assert(grows == 1);
    assert_wall_native_prefix();
    assert_wall_edges(1.5f, 1.0f, 1);
    assert(object.mesh.mesh.max.z > wallNativeMax.z);

    scale = 1.0f;
    mp6_widescreen_extend_wall_outer_sides(0, "meshstage_si13", 0.0f, 1.0f, 1);
    assert_wall_native_prefix();
    assert_wall_edges(1.0f, 1.0f, 1);
    assert(memcmp(&wallNativeMin, &object.mesh.mesh.min, sizeof(wallNativeMin)) == 0);
    assert(memcmp(&wallNativeMax, &object.mesh.mesh.max, sizeof(wallNativeMax)) == 0);

    /* Static-margin orchestration is for one independent, non-linked copy:
     * freeze deformation through HU3D_ATTR_ENVELOPE_OFF, retain the global
     * format/deformation counts, degenerate only the copied native prefix,
     * and make a repeat call a no-op. */
    reset_wall_fixture();
    MP6WSStaticMargin margin = {
        "meshstage_si13", MP6_WS_MARGIN_WALL, 3, 0.0f, 1.0f, 1
    };
    models[0].attr = HU3D_ATTR_MOTION_MODEL;
    mp6_widescreen_static_margins(0, &margin, 1);
    assert(grows == 0 && face.count == WALL_FACES);
    assert((models[0].attr & HU3D_ATTR_ENVELOPE_OFF) == 0);
    reset_wall_fixture();
    mp6_widescreen_static_margins(0, &margin, 1);
    assert_static_margin_result();
    HSF_FACE savedStaticFaces[WALL_FACES + 2];
    memcpy(savedStaticFaces, face.data, sizeof(savedStaticFaces));
    s32 savedStaticFaceCount = face.count;
    int savedStaticAttrSets = attrSets;
    int savedStaticRebakes = rebakes;
    u32 savedStaticFlags = object.flags;
    mp6_widescreen_static_margins(0, &margin, 1);
    assert(face.count == savedStaticFaceCount);
    assert(memcmp(savedStaticFaces, face.data, sizeof(savedStaticFaces)) == 0);
    assert(attrSets == savedStaticAttrSets && rebakes == savedStaticRebakes);
    assert(object.flags == savedStaticFlags);

    /* Explicit oblique end-cap reflection: only the authored QUAD range is
     * copied, HSF emission winding is reversed, the seam is coincident, and
     * the complete copy stays native-density at every wide scale. */
    reset_reflect_fixture();
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 2, 2, 0, 1);
    assert(grows == 0 && face.count == REFLECT_FACES);
    assert(memcmp(reflectNativeVertices, vertex.data, sizeof(reflectNativeVertices)) == 0);
    assert(memcmp(reflectNativeSt, st.data, sizeof(reflectNativeSt)) == 0);
    assert(memcmp(reflectNativeFaces, face.data, sizeof(reflectNativeFaces)) == 0);

    /* The reflection is deliberately limited to flat XZ art. A sloped
     * selected corner must refuse before any buffers grow. */
    reset_reflect_fixture();
    ((HuVecF *)vertex.data)[2].y = 0.1f;
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 1 && grows == 0 && face.count == REFLECT_FACES);

    /* The real non-cenv stream is packed S8 (three bytes per normal), not a
     * HuVecF array.  A vertical packed normal must pass without changing the
     * native normal buffer or its source indices. */
    reset_reflect_fixture();
    normalS8Data[0] = (HSF_S8VEC){0, 64, 0};
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 0 && grows == 1);
    assert_reflected_faces(1.0f);

    /* Packed normals in the native assets may use the full signed-byte
     * magnitude (127), rather than the six-bit unit value (64). The guard is
     * direction-relative, so both vertical encodings are valid. */
    reset_reflect_fixture();
    normalS8Data[0] = (HSF_S8VEC){0, 127, 0};
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 0 && grows == 1);
    assert_reflected_faces(1.0f);

    /* A zero normal has no direction and must still refuse before growth. */
    reset_reflect_fixture();
    normalS8Data[0] = (HSF_S8VEC){0, 0, 0};
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 1 && grows == 0 && face.count == REFLECT_FACES);

    /* Unsupported tilted packed normals still refuse before growth. */
    reset_reflect_fixture();
    normalS8Data[0] = (HSF_S8VEC){8, 64, 0};
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 1 && grows == 0 && face.count == REFLECT_FACES);

    /* A cenv-labelled model uses the real F32 normal layout. */
    reset_reflect_fixture();
    hsf.cenvNum = 1;
    normalF32Data[0] = (HuVecF){0.0f, 1.0f, 0.0f};
    normal.data = normalF32Data;
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 0 && grows == 1);
    assert_reflected_faces(1.0f);

    /* Tilted and non-finite F32 normals remain unsupported. */
    reset_reflect_fixture();
    hsf.cenvNum = 1;
    normalF32Data[0] = (HuVecF){0.1f, 0.99f, 0.0f};
    normal.data = normalF32Data;
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 1 && grows == 0 && face.count == REFLECT_FACES);
    reset_reflect_fixture();
    hsf.cenvNum = 1;
    normalF32Data[0] = (HuVecF){NAN, 1.0f, 0.0f};
    normal.data = normalF32Data;
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 1 && grows == 0 && face.count == REFLECT_FACES);

    reset_reflect_fixture();
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(refuses == 0 && grows == 1);
    assert_reflected_faces(1.0f);
    assert(memcmp(&reflectNativeMin, &object.mesh.mesh.min, sizeof(reflectNativeMin)) == 0);
    assert(memcmp(&reflectNativeMax, &object.mesh.mesh.max, sizeof(reflectNativeMax)) == 0);
    scale = 1.75f;
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert(grows == 1);
    assert_reflected_faces(1.75f);
    scale = 1.0f;
    mp6_widescreen_extend_reflected_face_range(0, "meshstage_si13", 0, 1, 0, 1);
    assert_reflected_faces(1.0f);
    assert(memcmp(&reflectNativeMin, &object.mesh.mesh.min, sizeof(reflectNativeMin)) == 0);
    assert(memcmp(&reflectNativeMax, &object.mesh.mesh.max, sizeof(reflectNativeMax)) == 0);
    kill_fixture();
    assert(kills == 1);

    /* The unfiltered API keeps its existing whole-mesh outer-plane behavior. */
    reset_fixture();
    mp6_widescreen_extend_terrain_outer_sides(0, "meshstage_si13", 0.0f, 1.0f, 0);
    assert(refuses == 0 && grows == 1 && entry.edgeCount == 2);
    for (int i = 0; i < entry.edgeCount; ++i)
        assert(fabsf(fabsf(entry.edges[i].pos[0].x) - 20.0f) < 0.001f);

    /* Model teardown invalidates the registration; a new instance can grow. */
    kill_fixture();
    assert(kills == 2);
    reset_fixture();
    mp6_widescreen_extend_terrain_outer_sides_material(0, "meshstage_si13",
                                                       0.0f, 1.0f, 0, 3);
    assert(grows == 1 && entry.edgeCount == 2);
    kill_fixture();
    puts("widescreen filtered terrain/wall extrusion and static margins: PASS");
    return 0;
}
