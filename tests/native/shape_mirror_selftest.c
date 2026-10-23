#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "game/hu3d.h"
#include "mp6_widescreen.h"
#include "shape_types.inc"

/* This fixture deliberately has several independent Z rows.  A single
 * vertical mirror plane would leave wedges when a row's animated endpoints
 * move; the production helper must choose the seam independently for every
 * vertex.  It is not a copy of the M675 asset: it is a compact, deterministic
 * row mesh which exercises the same ABI and failure paths. */
enum {
    ROWS = 5,
    COLS = 11,
    NATIVE_VERTICES = ROWS * COLS,
    NATIVE_FACES = (ROWS - 1) * (COLS - 1),
    SEA_FACES = MP6_WS_SHAPE_SEA_FACES,
    SEA_VERTICES = SEA_FACES * 4,
    EXPECTED_VERTICES = NATIVE_VERTICES * 3 + SEA_VERTICES,
    EXPECTED_ST = NATIVE_VERTICES + SEA_VERTICES,
    EXPECTED_FACES = NATIVE_FACES * 3 + SEA_FACES,
};

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
static HSF_DATA hsf;
static HSF_OBJECT object;
static HSF_BUFFER vertex, normal, st, face, shape;
static HSF_BUFFER *shapeList[1];
static HuVecF shapePoints[NATIVE_VERTICES];
static HuVecF nativeVertices[NATIVE_VERTICES];
static HuVec2f nativeSt[NATIVE_VERTICES];
static HSF_FACE nativeFaces[NATIVE_FACES];
static HuVecF nativeMin, nativeMax;
static HuVecF shapeNativePoints[NATIVE_VERTICES];
static void *nativeShapeData;
static s32 nativeShapeCount;
static MP6WSExtendEntry g_mp6WsExtend[1];
#define entry g_mp6WsExtend[0]
static s16 g_mp6WsExtendNum;
static s16 registryCount;
static float scale = 1.0f;
static int growCalls, refuseCalls, invalidateCalls;
static int failGrow;

float mp6_widescreen_scale_factor(void) { return scale; }
void GXInvalidateVtxCache(void) { ++invalidateCalls; }

/* The extracted registration code intentionally calls the real production
 * locator symbol.  This stub is narrow: it only supplies the object identity
 * and verifies that shape registration opts into the allowShape path. */
static HSF_OBJECT *mp6_ws_extend_locate_mesh_impl(HU3D_MODELID modelId,
                                                   const char *name,
                                                   BOOL needsST, int occurrence,
                                                   BOOL allowShape)
{
    assert(needsST && allowShape && occurrence == 0);
    if (modelId != 0 || name == NULL || strcmp(name, "shape_fixture") != 0 ||
        models[0].hsf == NULL) return NULL;
    return &object;
}

static BOOL mp6_ws_model_live(HU3D_MODELID modelId, const HSF_DATA *owner)
{
    return modelId >= 0 && modelId < HU3D_MODEL_MAX && owner != NULL &&
           Hu3DData[modelId].hsf == owner;
}

static MP6WSExtendEntry *mp6_ws_extend_find(HSF_OBJECT *candidate)
{
    return registryCount != 0 && g_mp6WsExtendNum != 0 && entry.obj == candidate &&
           mp6_ws_model_live(entry.modelId, entry.hsf) ? &entry : NULL;
}

static MP6WSExtendEntry *mp6_ws_extend_alloc(void)
{
    memset(&entry, 0, sizeof(entry));
    registryCount = 1;
    g_mp6WsExtendNum = 1;
    return &entry;
}

static void mp6_ws_extend_refuse(const char *name, const char *why)
{
    (void)name;
    (void)why;
    ++refuseCalls;
}

static BOOL mp6_ws_extend_grow(HU3D_MODELID modelId, HSF_OBJECT *owner,
                               s32 addVtx, s32 addSt, s32 addFaces,
                               void (*writer)(MP6WSExtendEntry *, HSF_FACE *,
                                              const HSF_FACE *),
                               MP6WSExtendEntry *e)
{
    HSF_FACE *newFaces;
    HuVecF *newVertices;
    HuVec2f *newSt;
    (void)modelId;
    assert(owner == &object && writer != NULL && e == &entry);
    if (failGrow) return FALSE;
    newFaces = calloc((size_t)(face.count + addFaces), sizeof(*newFaces));
    newVertices = calloc((size_t)(vertex.count + addVtx), sizeof(*newVertices));
    newSt = calloc((size_t)(st.count + addSt), sizeof(*newSt));
    assert(newFaces != NULL && newVertices != NULL && newSt != NULL);
    memcpy(newFaces, face.data, (size_t)face.count * sizeof(*newFaces));
    memcpy(newVertices, vertex.data, (size_t)vertex.count * sizeof(*newVertices));
    memcpy(newSt, st.data, (size_t)st.count * sizeof(*newSt));
    /* The writer must see the native face prefix before this fixture swaps
     * the live face buffer, exactly as the engine's grow helper does. */
    writer(e, newFaces + face.count, face.data);
    free(face.data);
    free(vertex.data);
    free(st.data);
    face.data = newFaces;
    vertex.data = newVertices;
    st.data = newSt;
    face.count += addFaces;
    vertex.count += addVtx;
    st.count += addSt;
    ++growCalls;
    return TRUE;
}

/* The dispatcher is extracted verbatim.  These branches are unreachable in
 * this fixture but keeping their symbols lets the test compile the real
 * switch instead of a shape-only rewrite. */
static void mp6_ws_extend_tiles_apply(MP6WSExtendEntry *e, float k) { (void)e; (void)k; }
static void mp6_ws_extend_mirror_apply(MP6WSExtendEntry *e, float k) { (void)e; (void)k; }
static void mp6_ws_extend_ring_apply(MP6WSExtendEntry *e, float k) { (void)e; (void)k; }
static void mp6_ws_extend_pillar_apply(MP6WSExtendEntry *e, float k) { (void)e; (void)k; }
static void mp6_ws_extend_results_apply(MP6WSExtendEntry *e, float k) { (void)e; (void)k; }
static void mp6_ws_extend_edge_apply(MP6WSExtendEntry *e, float k) { (void)e; (void)k; }
static void mp6_ws_extend_reflect_faces_apply(MP6WSExtendEntry *e, float k) { (void)e; (void)k; }
static void mp6_ws_extend_shape_apply(MP6WSExtendEntry *e, float k);
static void mp6_ws_extend_apply(MP6WSExtendEntry *e, float k);

#include "shape_subject.inc"

static void reset_fixture(void)
{
    free(vertex.data);
    free(st.data);
    free(face.data);
    free(normal.data);
    memset(&hsf, 0, sizeof(hsf));
    memset(&object, 0, sizeof(object));
    memset(&vertex, 0, sizeof(vertex));
    memset(&normal, 0, sizeof(normal));
    memset(&st, 0, sizeof(st));
    memset(&face, 0, sizeof(face));
    memset(&shape, 0, sizeof(shape));
    memset(models, 0, sizeof(models));
    registryCount = 0;
    g_mp6WsExtendNum = 0;
    memset(&entry, 0, sizeof(entry));
    growCalls = refuseCalls = invalidateCalls = 0;
    failGrow = 0;
    scale = 1.0f;

    vertex.count = NATIVE_VERTICES;
    st.count = NATIVE_VERTICES;
    face.count = NATIVE_FACES;
    vertex.data = calloc(NATIVE_VERTICES, sizeof(HuVecF));
    st.data = calloc(NATIVE_VERTICES, sizeof(HuVec2f));
    face.data = calloc(NATIVE_FACES, sizeof(HSF_FACE));
    assert(vertex.data && st.data && face.data);

    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            int i = row * COLS + col;
            float x = -500.0f + 100.0f * col;
            float z = 100.0f * row;
            float y = 10.0f * row + (float)(col & 1);
            ((HuVecF *)vertex.data)[i] = (HuVecF){x, y, z};
            ((HuVec2f *)st.data)[i] = (HuVec2f){0.01f * (float)col,
                                               0.05f * (float)row};
            shapePoints[i] = (HuVecF){x + (col == 0 ? 5.0f : col == COLS - 1 ? -5.0f : 0.0f),
                                     y + 1.0f, z};
        }
    }
    for (int row = 0; row < ROWS - 1; ++row) {
        for (int col = 0; col < COLS - 1; ++col) {
            int f = row * (COLS - 1) + col;
            int a = row * COLS + col;
            int b = row * COLS + col + 1;
            int d = (row + 1) * COLS + col;
            int c = (row + 1) * COLS + col + 1;
            HSF_FACE *out = (HSF_FACE *)face.data + f;
            int count = f == 0 ? 3 : 4;
            int indices[4] = {a, d, b, c};
            memset(out, 0, sizeof(*out));
            out->typeSrc = (u16)(count == 3 ? HSF_FACE_TRI : HSF_FACE_QUAD);
            out->mat = (s16)(3 + (row & 1));
            out->nbt[0] = 0.0f;
            out->nbt[1] = 1.0f;
            out->nbt[2] = 0.0f;
            for (int corner = 0; corner < count; ++corner) {
                out->index[corner].vertex = (s16)indices[corner];
                out->index[corner].normal = (s16)(corner & 1);
                out->index[corner].color = (s16)(20 + corner);
                out->index[corner].st = (s16)indices[corner];
            }
        }
    }

    normal.count = 2;
    normal.data = calloc(2, sizeof(HSF_S8VEC));
    assert(normal.data != NULL);
    ((HSF_S8VEC *)normal.data)[0] = (HSF_S8VEC){0, 64, 0};
    ((HSF_S8VEC *)normal.data)[1] = (HSF_S8VEC){0, 0, 64};
    shape.name = "shape0";
    shape.count = NATIVE_VERTICES;
    shape.data = shapePoints;
    shapeList[0] = &shape;

    object.name = "shape_fixture";
    object.type = HSF_OBJ_MESH;
    object.mesh.vertex = &vertex;
    object.mesh.normal = &normal;
    object.mesh.st = &st;
    object.mesh.face = &face;
    object.mesh.shape = shapeList;
    object.mesh.shapeNum = 1;
    object.mesh.mesh.min = (HuVecF){-500.0f, 0.0f, 0.0f};
    object.mesh.mesh.max = (HuVecF){500.0f, 41.0f, 400.0f};
    nativeMin = object.mesh.mesh.min;
    nativeMax = object.mesh.mesh.max;
    nativeShapeData = shape.data;
    nativeShapeCount = shape.count;

    hsf.object = &object;
    hsf.objectNum = 1;
    hsf.materialNum = 8;
    models[0].hsf = &hsf;
    models[0].linkMdlId = HU3D_MODELID_NONE;
    memcpy(nativeVertices, vertex.data, sizeof(nativeVertices));
    memcpy(nativeSt, st.data, sizeof(nativeSt));
    memcpy(nativeFaces, face.data, sizeof(nativeFaces));
    memcpy(shapeNativePoints, shapePoints, sizeof(shapeNativePoints));
}

static void assert_native_prefix(void)
{
    assert(memcmp(nativeVertices, vertex.data, sizeof(nativeVertices)) == 0);
    assert(memcmp(nativeSt, st.data, sizeof(nativeSt)) == 0);
    assert(memcmp(nativeFaces, face.data, sizeof(nativeFaces)) == 0);
    assert(memcmp(shapeNativePoints, shapePoints, sizeof(shapeNativePoints)) == 0);
    assert(shape.data == nativeShapeData && shape.count == nativeShapeCount);
    assert(object.mesh.shape == shapeList && object.mesh.shapeNum == 1);
}

static int face_corner_count(const HSF_FACE *faceRecord)
{
    return (faceRecord->type & 7) == HSF_FACE_TRI ? 3 : 4;
}

static void assert_shape_faces(void)
{
    HSF_FACE *faces = (HSF_FACE *)face.data;
    assert(entry.mode == MP6_WS_EXTEND_SHAPE_MIRROR);
    assert(entry.baseVtx == NATIVE_VERTICES);
    assert(entry.baseSt == NATIVE_VERTICES);
    assert(entry.shapeFaces == NATIVE_FACES);
    for (int side = 0; side < 2; ++side) {
        for (int f = 0; f < NATIVE_FACES; ++f) {
            const HSF_FACE *source = &nativeFaces[f];
            const HSF_FACE *tail = &faces[NATIVE_FACES + side * NATIVE_FACES + f];
            const int reverse[4] = {0, 2, 1, 3};
            int count = face_corner_count(source);
            assert(tail->type == source->type);
            assert(tail->typeSrc == source->typeSrc);
            assert(tail->mat == source->mat);
            assert(memcmp(tail->nbt, source->nbt, sizeof(tail->nbt)) == 0);
            for (int c = 0; c < count; ++c) {
                const HSF_FACE_INDEX *want = &source->index[reverse[c]];
                const HSF_FACE_INDEX *got = &tail->index[c];
                assert(got->vertex == want->vertex + NATIVE_VERTICES * (side + 1));
                assert(got->st == want->st);
                assert(got->normal == want->normal);
                assert(got->color == want->color);
            }
        }
    }
    for (int f = 0; f < SEA_FACES; ++f) {
        const HSF_FACE *tail = &faces[NATIVE_FACES * 3 + f];
        assert((tail->type & 7) == HSF_FACE_QUAD);
        assert(tail->mat == nativeFaces[1].mat);
        for (int c = 0; c < 4; ++c) {
            assert(tail->index[c].vertex == NATIVE_VERTICES * 3 + f * 4 + c);
            assert(tail->index[c].st == NATIVE_VERTICES + f * 4 + c);
            assert(tail->index[c].normal == nativeFaces[1].index[c].normal);
            assert(tail->index[c].color == nativeFaces[1].index[c].color);
        }
    }
}

static void assert_seam_mirror(float amount, int planeCollapsed)
{
    HuVecF *v = (HuVecF *)vertex.data;
    for (int row = 0; row < ROWS; ++row) {
        int left = row * COLS;
        int right = row * COLS + COLS - 1;
        for (int col = 0; col < COLS; ++col) {
            int i = row * COLS + col;
            HuVecF source = v[i];
            HuVecF leftMirror = v[NATIVE_VERTICES + i];
            HuVecF rightMirror = v[NATIVE_VERTICES * 2 + i];
            float leftX = v[left].x + amount * (v[left].x - source.x);
            float rightX = v[right].x + amount * (v[right].x - source.x);
            float leftY = v[left].y + amount * (source.y - v[left].y);
            float rightY = v[right].y + amount * (source.y - v[right].y);
            assert(fabsf(leftMirror.x - leftX) < .001f);
            assert(fabsf(rightMirror.x - rightX) < .001f);
            assert(fabsf(leftMirror.y - leftY) < .001f);
            assert(fabsf(rightMirror.y - rightY) < .001f);
            assert(leftMirror.z == source.z && rightMirror.z == source.z);
        }
    }
    for (int row = 0; row < MP6_WS_SHAPE_SEA_ROWS; ++row) {
        for (int col = 0; col < MP6_WS_SHAPE_SEA_COLS; ++col) {
            int tile = row * MP6_WS_SHAPE_SEA_COLS + col;
            int base = NATIVE_VERTICES * 3 + tile * 4;
            for (int c = 0; c < 4; ++c) {
                HuVecF p = v[base + c];
                if (planeCollapsed) {
                    assert(memcmp(&p, &v[0], sizeof(p)) == 0);
                } else {
                    float x = (col - MP6_WS_SHAPE_SEA_COLS / 2) * entry.shapeTileX +
                              ((c & 1) ? entry.shapeTileX : 0.0f);
                    float z = (row - 10) * entry.shapeTileZ +
                              ((c & 2) ? 0.0f : entry.shapeTileZ);
                    assert(fabsf(p.x - x) < .001f);
                    assert(fabsf(p.y - entry.shapePlaneY) < .001f);
                    assert(fabsf(p.z - z) < .001f);
                }
            }
        }
    }
}

static void mutate_native_shape(void)
{
    HuVecF *v = (HuVecF *)vertex.data;
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            int i = row * COLS + col;
            float span = 500.0f + row * 13.0f;
            v[i].x = -span + (2.0f * span * col) / (COLS - 1);
            v[i].y += 3.0f + (float)row;
        }
    }
}

static void test_registration_resize_and_shape_update(void)
{
    reset_fixture();
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == 1 && registryCount == 1 && invalidateCalls == 1);
    assert(vertex.count == EXPECTED_VERTICES);
    assert(st.count == EXPECTED_ST);
    assert(face.count == EXPECTED_FACES);
    assert_native_prefix();
    assert_shape_faces();
    assert_seam_mirror(0.0f, 1);
    assert(memcmp(&object.mesh.mesh.min, &nativeMin, sizeof(nativeMin)) == 0);
    assert(memcmp(&object.mesh.mesh.max, &nativeMax, sizeof(nativeMax)) == 0);

    scale = 1.325f;
    mp6_widescreen_shape_update(0);
    assert(growCalls == 1 && vertex.count == EXPECTED_VERTICES &&
           st.count == EXPECTED_ST && face.count == EXPECTED_FACES);
    assert_seam_mirror(1.0f, 0);
    assert_native_prefix();
    mutate_native_shape();
    HuVecF deformed[NATIVE_VERTICES];
    memcpy(deformed, vertex.data, sizeof(deformed));
    mp6_widescreen_shape_update(0);
    assert(memcmp(deformed, vertex.data, sizeof(deformed)) == 0);
    assert_seam_mirror(1.0f, 0);

    /* A second wide aspect and duplicate registration are idempotent. */
    scale = 1.75f;
    mp6_widescreen_shape_update(0);
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == 1 && vertex.count == EXPECTED_VERTICES &&
           st.count == EXPECTED_ST && face.count == EXPECTED_FACES);
    assert_seam_mirror(1.0f, 0);

    /* Returning to native collapses both mirrors and the backing sheet to a
     * single native point and restores the authored cull box byte-for-byte. */
    scale = 1.0f;
    mp6_widescreen_shape_update(0);
    assert_seam_mirror(0.0f, 1);
    assert(memcmp(&object.mesh.mesh.min, &nativeMin, sizeof(nativeMin)) == 0);
    assert(memcmp(&object.mesh.mesh.max, &nativeMax, sizeof(nativeMax)) == 0);
}

static void test_stale_owner_and_transaction_guards(void)
{
    HSF_DATA other;
    int oldGrow, oldRefuse;
    reset_fixture();
    failGrow = 1;
    oldGrow = growCalls;
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == oldGrow && vertex.count == NATIVE_VERTICES &&
           st.count == NATIVE_VERTICES && face.count == NATIVE_FACES);
    failGrow = 0;
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == 1);

    memcpy(nativeVertices, vertex.data, sizeof(nativeVertices));
    other = hsf;
    models[0].hsf = &other;
    scale = 1.75f;
    mp6_widescreen_shape_update(0);
    assert(memcmp(nativeVertices, vertex.data, sizeof(nativeVertices)) == 0);
    models[0].hsf = &hsf;

    reset_fixture();
    oldRefuse = refuseCalls;
    shapeList[0] = &vertex;
    object.mesh.shape = shapeList;
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == 0 && refuseCalls == oldRefuse + 1);

    reset_fixture();
    oldRefuse = refuseCalls;
    shapePoints[1].x = -700.0f;
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == 0 && refuseCalls == oldRefuse + 1);

    reset_fixture();
    vertex.count = 3;
    oldGrow = growCalls;
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == oldGrow && registryCount == 0);

    reset_fixture();
    models[0].attr = HU3D_ATTR_HOOKFUNC;
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == 0 && registryCount == 0);
    models[0].attr = 0;
    models[0].linkMdlId = 1;
    mp6_widescreen_extend_shape_mirror(0, "shape_fixture");
    assert(growCalls == 0 && registryCount == 0);
}

int main(void)
{
    test_registration_resize_and_shape_update();
    test_stale_owner_and_transaction_guards();
    return 0;
}
