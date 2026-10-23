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
static HSF_BUFFER vertex, normal, st, face;
static MP6WSExtendEntry entry;
static HuVecF normalData[32];
static HSF_FACE_INDEX stripData[2][5];
static float scale = 1.75f;
static int grows, refuse, fail, selectedOccurrence;
float mp6_widescreen_scale_factor(void) { return scale; }
void GXInvalidateVtxCache(void) {}
static HSF_OBJECT *mp6_ws_extend_locate(s16 id, const char *name) { return &object; }
static HSF_OBJECT *mp6_ws_extend_locate_mesh(s16 id, const char *name, BOOL needsST) { return &object; }
static HSF_OBJECT *mp6_ws_extend_locate_mesh_at(s16 id, const char *name, BOOL needsST, int occurrence)
{ return occurrence == selectedOccurrence ? &object : NULL; }
static MP6WSExtendEntry *mp6_ws_extend_find(HSF_OBJECT *o) { return entry.obj == o ? &entry : NULL; }
static MP6WSExtendEntry *mp6_ws_extend_alloc(void) { return &entry; }
static void mp6_ws_extend_refuse(const char *name, const char *why) { ++refuse; }
static BOOL mp6_ws_extend_grow(s16 id, HSF_OBJECT *o, s32 nv, s32 ns, s32 nf,
    void (*writer)(MP6WSExtendEntry *, HSF_FACE *, const HSF_FACE *), MP6WSExtendEntry *e)
{
    if (fail) return FALSE;
    HSF_FACE *next = calloc(face.count + nf, sizeof(HSF_FACE));
    memcpy(next, face.data, face.count * sizeof(HSF_FACE));
    writer(e, next + face.count, face.data);
    free(face.data); face.data = next; face.count += nf;
    vertex.data = realloc(vertex.data, (vertex.count + nv) * sizeof(HuVecF)); vertex.count += nv;
    st.data = realloc(st.data, (st.count + ns) * sizeof(HuVec2f)); st.count += ns;
    ++grows;
    return TRUE;
}
#include "edge_subject.inc"

static void reset(void)
{
    free(vertex.data); free(st.data); free(face.data);
    memset(&entry, 0, sizeof(entry)); memset(&object, 0, sizeof(object));
    memset(&normal, 0, sizeof(normal));
    normal.count = (s32)(sizeof(normalData) / sizeof(normalData[0]));
    normal.data = normalData;
    models[0].hsf = &hsf;
    object.mesh.vertex = &vertex; object.mesh.normal = &normal;
    object.mesh.st = &st; object.mesh.face = &face;
    vertex.count = st.count = 4; face.count = 1;
    vertex.data = calloc(4, sizeof(HuVecF)); st.data = calloc(4, sizeof(HuVec2f));
    face.data = calloc(1, sizeof(HSF_FACE));
    /* Native Z-order quad. UV density: one period per 2 world units. */
    HuVecF v[4] = {{-1,0,-1},{-1,0,1},{1,0,-1},{1,0,1}};
    HuVec2f uv[4] = {{0,0},{0,1},{1,0},{1,1}};
    memcpy(vertex.data, v, sizeof(v)); memcpy(st.data, uv, sizeof(uv));
    object.mesh.mesh.min = v[0]; object.mesh.mesh.max = v[3];
    HSF_FACE *f = face.data;
    f->typeSrc = HSF_FACE_QUAD; f->mat = 13;
    for (int i=0; i<4; ++i) {
        f->index[i].vertex = f->index[i].st = i;
        f->index[i].normal = 5; f->index[i].color = 8;
    }
    grows = refuse = fail = selectedOccurrence = 0;
}

static HSF_FACE_INDEX strip_index(int vertexIndex, int stIndex, int normalIndex, int colorIndex)
{
    HSF_FACE_INDEX index = {0};
    index.vertex = (s16)vertexIndex;
    index.st = (s16)stIndex;
    index.normal = (s16)normalIndex;
    index.color = (s16)colorIndex;
    return index;
}

/* Two cave-like rails with deliberately different top heights. The on-disc
 * tristrip starts [0,2,1], then continues in paired top/inner order. */
static void reset_strip(void)
{
    reset();
    free(vertex.data); free(st.data); free(face.data);
    vertex.count = st.count = 16;
    face.count = 2;
    vertex.data = calloc((size_t)vertex.count, sizeof(HuVecF));
    st.data = calloc((size_t)st.count, sizeof(HuVec2f));
    face.data = calloc((size_t)face.count, sizeof(HSF_FACE));
    normal.count = (s32)(sizeof(normalData) / sizeof(normalData[0]));
    normal.data = normalData;
    object.mesh.normal = &normal;
    memset(stripData, 0, sizeof(stripData));
    for (int rail = 0; rail < 2; ++rail) {
        const float topY = rail == 0 ? 550.0f : 500.0f;
        const float outerX = rail == 0 ? -8.0f : 8.0f;
        const float innerX = rail == 0 ? -7.0f : 7.0f;
        const int base = rail * 8;
        for (int i = 0; i < 4; ++i) {
            const float z = (float)i;
            const int top = base + i;
            const int inner = base + 4 + i;
            ((HuVecF *)vertex.data)[top] = (HuVecF){outerX + 0.25f * i, topY, z};
            ((HuVecF *)vertex.data)[inner] = (HuVecF){innerX + 0.25f * i, topY - 250.0f, z};
            ((HuVec2f *)st.data)[top] = (HuVec2f){0.1f + 0.2f * i + rail, 0.2f + 0.05f * i};
            ((HuVec2f *)st.data)[inner] = (HuVec2f){0.5f + 0.2f * i + rail, 0.8f + 0.05f * i};
        }
        HSF_FACE *f = (HSF_FACE *)face.data + rail;
        f->typeSrc = HSF_FACE_TRISTRIP;
        f->mat = (s16)(13 + rail);
        if (rail == 0) {
            /* First emitted pair is [top0, inner0], so top == 0. */
            f->index[0] = strip_index(base + 0, base + 0, 7, 9);
            f->index[1] = strip_index(base + 1, base + 1, 7, 9);
            f->index[2] = strip_index(base + 4, base + 4, 7, 9);
            stripData[rail][0] = strip_index(base + 5, base + 5, 7, 9);
            stripData[rail][1] = strip_index(base + 2, base + 2, 7, 9);
            stripData[rail][2] = strip_index(base + 6, base + 6, 7, 9);
            stripData[rail][3] = strip_index(base + 3, base + 3, 7, 9);
            stripData[rail][4] = strip_index(base + 7, base + 7, 7, 9);
        } else {
            /* First emitted pair is [inner0, top0], so top == 1. */
            f->index[0] = strip_index(base + 4, base + 4, 7, 9);
            f->index[1] = strip_index(base + 5, base + 5, 7, 9);
            f->index[2] = strip_index(base + 0, base + 0, 7, 9);
            stripData[rail][0] = strip_index(base + 1, base + 1, 7, 9);
            stripData[rail][1] = strip_index(base + 6, base + 6, 7, 9);
            stripData[rail][2] = strip_index(base + 2, base + 2, 7, 9);
            stripData[rail][3] = strip_index(base + 7, base + 7, 7, 9);
            stripData[rail][4] = strip_index(base + 3, base + 3, 7, 9);
        }
        f->strip.count = 5;
        f->strip.data = stripData[rail];
    }
    object.mesh.mesh.min = (HuVecF){-8.0f, 0.0f, 0.0f};
    object.mesh.mesh.max = (HuVecF){8.75f, 550.0f, 3.0f};
    grows = refuse = fail = 0;
}

static void test_strip_rim(void)
{
    HuVecF nativeV[16], nativeMin, nativeMax;
    HuVec2f nativeUV[16];
    HSF_FACE nativeFaces[2];

    reset_strip();
    memcpy(nativeV, vertex.data, sizeof(nativeV));
    memcpy(nativeUV, st.data, sizeof(nativeUV));
    memcpy(nativeFaces, face.data, sizeof(nativeFaces));
    nativeMin = object.mesh.mesh.min;
    nativeMax = object.mesh.mesh.max;
    scale = 1.0f;
    mp6_widescreen_extend_strip_rim(0, "cave-rim", 500.0f, 20.0f);
    assert(grows == 1 && !refuse && entry.obj == &object);
    assert(entry.mode == MP6_WS_EXTEND_STRIP_RIM && entry.edgeCount == 6);
    assert(vertex.count == 28 && st.count == 28 && face.count == 8);
    assert(!memcmp(nativeV, vertex.data, sizeof(nativeV)));
    assert(!memcmp(nativeUV, st.data, sizeof(nativeUV)));
    assert(!memcmp(nativeFaces, face.data, sizeof(nativeFaces)));
    assert(!memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)));
    assert(!memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)));

    for (int i = 0; i < entry.edgeCount; ++i) {
        MP6WSEdge *edge = &entry.edges[i];
        HSF_FACE *appended = (HSF_FACE *)face.data + 2 + i;
        assert((appended->typeSrc & HSF_FACE_MASK) == HSF_FACE_QUAD);
        /* The seam is deliberately [current, next, new-next, new-current].
         * This checks both top-parity branches, not merely the vertex count. */
        assert(!memcmp(&appended->index[0], &edge->sourceIndex[1], sizeof(HSF_FACE_INDEX)));
        assert(!memcmp(&appended->index[2], &edge->sourceIndex[0], sizeof(HSF_FACE_INDEX)));
        assert(appended->index[3].vertex == 16 + i * 2);
        assert(appended->index[1].vertex == 16 + i * 2 + 1);
        assert(appended->index[3].st == 16 + i * 2);
        assert(appended->index[1].st == 16 + i * 2 + 1);
        assert(appended->index[3].normal == edge->sourceIndex[0].normal);
        assert(appended->index[1].normal == edge->sourceIndex[1].normal);
        assert(appended->index[3].color == edge->sourceIndex[0].color);
        assert(appended->index[1].color == edge->sourceIndex[1].color);
        for (int j = 0; j < 2; ++j) {
            HuVecF p = ((HuVecF *)vertex.data)[16 + i * 2 + j];
            HuVec2f uv = ((HuVec2f *)st.data)[16 + i * 2 + j];
            assert(!memcmp(&p, &edge->pos[j], sizeof(p)));
            assert(!memcmp(&uv, &edge->uv[j], sizeof(uv)));
        }
    }

    /* At wide aspect only the companion seam changes. The native rails and
     * original faces remain byte-identical, and UVs follow the same density. */
    scale = 1.75f;
    mp6_widescreen_extend_strip_rim(0, "cave-rim", 500.0f, 20.0f);
    assert(grows == 1 && vertex.count == 28 && st.count == 28 && face.count == 8);
    assert(!memcmp(nativeV, vertex.data, sizeof(nativeV)));
    assert(!memcmp(nativeUV, st.data, sizeof(nativeUV)));
    assert(!memcmp(nativeFaces, face.data, sizeof(nativeFaces)));
    for (int i = 0; i < entry.edgeCount; ++i) for (int j = 0; j < 2; ++j) {
        MP6WSEdge *edge = &entry.edges[i];
        HuVecF p = ((HuVecF *)vertex.data)[16 + i * 2 + j];
        HuVec2f uv = ((HuVec2f *)st.data)[16 + i * 2 + j];
        float distance = 15.0f;
        assert(fabsf(p.x - (edge->pos[j].x + distance * edge->posSlope[j].x)) < .0001f);
        assert(fabsf(p.y - (edge->pos[j].y + distance * edge->posSlope[j].y)) < .0001f);
        assert(fabsf(p.z - edge->pos[j].z) < .0001f);
        assert(fabsf(uv.x - (edge->uv[j].x + distance * edge->uvSlope[j].x)) < .0001f);
        assert(fabsf(uv.y - (edge->uv[j].y + distance * edge->uvSlope[j].y)) < .0001f);
    }
    /* The static STRIP_RIM fast path must not rewrite an already-applied
     * same-aspect seam. Sentinels make byte equality observable. */
    HuVecF sentinelV = {1234.0f, -567.0f, 89.0f};
    HuVec2f sentinelUV = {31.0f, -17.0f};
    ((HuVecF *)vertex.data)[16] = sentinelV;
    ((HuVec2f *)st.data)[16] = sentinelUV;
    mp6_widescreen_extend_strip_rim(0, "cave-rim", 500.0f, 20.0f);
    assert(grows == 1 && !memcmp(&sentinelV, (HuVecF *)vertex.data + 16, sizeof(sentinelV)));
    assert(!memcmp(&sentinelUV, (HuVec2f *)st.data + 16, sizeof(sentinelUV)));
    /* Duplicate registration remains idempotent after the sentinel check. */
    mp6_widescreen_extend_strip_rim(0, "cave-rim", 500.0f, 20.0f);
    assert(grows == 1 && !memcmp(&sentinelV, (HuVecF *)vertex.data + 16, sizeof(sentinelV)));
    assert(!memcmp(&sentinelUV, (HuVec2f *)st.data + 16, sizeof(sentinelUV)));
    /* A changed aspect must regenerate the sentinels from native snapshots. */
    scale = 1.0f;
    mp6_widescreen_extend_strip_rim(0, "cave-rim", 500.0f, 20.0f);
    assert(!memcmp((HuVecF *)vertex.data + 16, &entry.edges[0].pos[0], sizeof(HuVecF)));
    assert(!memcmp((HuVec2f *)st.data + 16, &entry.edges[0].uv[0], sizeof(HuVec2f)));
    for (int i = 0; i < entry.edgeCount; ++i) for (int j = 0; j < 2; ++j) {
        assert(!memcmp((HuVecF *)vertex.data + 16 + i * 2 + j,
                       &entry.edges[i].pos[j], sizeof(HuVecF)));
        assert(!memcmp((HuVec2f *)st.data + 16 + i * 2 + j,
                       &entry.edges[i].uv[j], sizeof(HuVec2f)));
    }
    assert(!memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)));
    assert(!memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)));

    /* Every refusal is transactional: no registry slot, growth, or partial
     * buffer append survives malformed strip input. */
    reset_strip();
    ((HSF_FACE *)face.data)->strip.count = 4; /* total emitted count is odd */
    mp6_widescreen_extend_strip_rim(0, "odd", 500.0f, 20.0f);
    assert(!grows && !entry.obj && vertex.count == 16 && st.count == 16 && face.count == 2);

    reset_strip();
    stripData[1][4].vertex = 99;
    mp6_widescreen_extend_strip_rim(0, "bad-index", 500.0f, 20.0f);
    assert(!grows && !entry.obj && vertex.count == 16 && face.count == 2);

    reset_strip();
    ((HSF_FACE *)face.data)->index[0].normal = normal.count;
    mp6_widescreen_extend_strip_rim(0, "bad-normal", 500.0f, 20.0f);
    assert(!grows && !entry.obj);

    reset_strip();
    ((HuVecF *)vertex.data)[12].z += 0.25f;
    mp6_widescreen_extend_strip_rim(0, "mismatched-z", 500.0f, 20.0f);
    assert(!grows && !entry.obj);

    reset_strip();
    ((HuVecF *)vertex.data)[10].z = -1.0f;
    ((HuVecF *)vertex.data)[14].z = -1.0f;
    mp6_widescreen_extend_strip_rim(0, "nonmonotonic-z", 500.0f, 20.0f);
    assert(!grows && !entry.obj);

    reset_strip();
    mp6_widescreen_extend_strip_rim(0, "too-high", 600.0f, 20.0f);
    assert(!grows && !entry.obj);
    reset_strip();
    mp6_widescreen_extend_strip_rim(0, "zero-reach", 500.0f, 0.0f);
    assert(!grows && !entry.obj);
    reset_strip();
    mp6_widescreen_extend_strip_rim(0, "nan-reach", 500.0f, NAN);
    assert(!grows && !entry.obj);
    reset_strip();
    mp6_widescreen_extend_strip_rim(0, "nan-height", NAN, 20.0f);
    assert(!grows && !entry.obj);
    reset_strip();
    ((HSF_FACE *)face.data)[0].strip.data = NULL;
    mp6_widescreen_extend_strip_rim(0, "null-strip", 500.0f, 20.0f);
    assert(!grows && !entry.obj);

    reset_strip();
    fail = 1;
    mp6_widescreen_extend_strip_rim(0, "allocation-failure", 500.0f, 20.0f);
    assert(!grows && !entry.obj && vertex.count == 16 && st.count == 16 && face.count == 2);

    reset_strip();
    ((HSF_FACE *)face.data)[1].strip.count = 4;
    mp6_widescreen_extend_strip_rim(0, "late-malformed", 500.0f, 20.0f);
    assert(!grows && !entry.obj && vertex.count == 16 && st.count == 16 && face.count == 2);
}

/* A wall silhouette can be non-planar in depth. The bounded wall-edge mode
 * still uses the common XY/adjacency planner, but companion vertices move
 * only in X. Authored Y/Z, STs, and native face attributes remain intact. */
static void test_wall_boundary_strip(void)
{
    reset();
    free(vertex.data); free(st.data); free(face.data);
    vertex.count = 3; st.count = 3; face.count = 1;
    vertex.data = calloc(3, sizeof(HuVecF));
    st.data = calloc(3, sizeof(HuVec2f));
    face.data = calloc(1, sizeof(HSF_FACE));
    HuVecF authored[3] = {
        {-4.00f, 0.0f, 10.0f},
        {-3.95f, 1.0f, 30.0f},
        {-3.98f, 2.0f, 110.0f},
    };
    HuVec2f authoredST[3] = {{.10f, .20f}, {.50f, .80f}, {.90f, .20f}};
    memcpy(vertex.data, authored, sizeof(authored));
    memcpy(st.data, authoredST, sizeof(authoredST));
    object.mesh.mesh.min = authored[0];
    object.mesh.mesh.max = authored[2];
    HSF_FACE *nativeFace = (HSF_FACE *)face.data;
    nativeFace->typeSrc = HSF_FACE_TRI;
    nativeFace->mat = 13;
    /* Native draw order is 0,2,1, so [A,Q,B] exposes A--B. */
    nativeFace->index[0] = strip_index(0, 0, 5, 8);
    nativeFace->index[1] = strip_index(1, 1, 5, 8);
    nativeFace->index[2] = strip_index(2, 2, 5, 8);

    HuVecF nativeMin = object.mesh.mesh.min, nativeMax = object.mesh.mesh.max;
    HSF_FACE nativeCopy = *nativeFace;
    scale = 1.0f;
    mp6_widescreen_extend_wall_edges(0, "nonplanar-wall", 0.0f, 2.0f, 1);
    assert(grows == 1 && !refuse && entry.obj == &object);
    assert(entry.mode == MP6_WS_EXTEND_WALL_EDGES && entry.edgeCount == 1);
    assert(vertex.count == 5 && st.count == 5 && face.count == 2);
    assert(!memcmp(authored, vertex.data, sizeof(authored)));
    assert(!memcmp(authoredST, st.data, sizeof(authoredST)));
    assert(!memcmp(&nativeCopy, face.data, sizeof(nativeCopy)));
    assert(!memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)));
    assert(!memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)));

    MP6WSEdge *edge = &entry.edges[0];
    HSF_FACE *appended = (HSF_FACE *)face.data + 1;
    assert((appended->typeSrc & HSF_FACE_MASK) == HSF_FACE_QUAD);
    assert(appended->mat == nativeCopy.mat);
    for (int i = 0; i < 4; ++i)
        assert(appended->index[i].normal == 5 && appended->index[i].color == 8);
    for (int j = 0; j < 2; ++j) {
        HuVecF p = ((HuVecF *)vertex.data)[3 + j];
        HuVec2f uv = ((HuVec2f *)st.data)[3 + j];
        assert(fabsf(p.x - edge->pos[j].x) < .0001f);
        assert(fabsf(p.y - edge->pos[j].y) < .0001f);
        assert(fabsf(p.z - edge->pos[j].z) < .0001f);
        assert(!memcmp(&uv, &edge->uv[j], sizeof(uv)));
    }

    /* The sloping authored edge intentionally spans a large Z range. At wide
     * scale only X moves; repeated registration must not append again. */
    scale = 1.75f;
    mp6_widescreen_extend_wall_edges(0, "nonplanar-wall", 0.0f, 2.0f, 1);
    assert(grows == 1 && vertex.count == 5 && st.count == 5 && face.count == 2);
    for (int j = 0; j < 2; ++j) {
        HuVecF p = ((HuVecF *)vertex.data)[3 + j];
        HuVec2f uv = ((HuVec2f *)st.data)[3 + j];
        float expectedX = edge->pos[j].x + (edge->pos[j].x - 0.0f) * 1.5f;
        assert(fabsf(p.x - expectedX) < .0001f);
        assert(fabsf(p.y - edge->pos[j].y) < .0001f);
        assert(fabsf(p.z - edge->pos[j].z) < .0001f);
        assert(!memcmp(&uv, &edge->uv[j], sizeof(uv)));
    }
    scale = 1.0f;
    mp6_widescreen_extend_wall_edges(0, "nonplanar-wall", 0.0f, 2.0f, 1);
    assert(grows == 1 && vertex.count == 5 && st.count == 5);
    for (int j = 0; j < 2; ++j)
        assert(!memcmp((HuVecF *)vertex.data + 3 + j, &edge->pos[j], sizeof(HuVecF)));
    assert(!memcmp(authored, vertex.data, sizeof(authored)));
    assert(!memcmp(authoredST, st.data, sizeof(authoredST)));
    assert(!memcmp(&nativeCopy, face.data, sizeof(nativeCopy)));
    assert(!memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)));
    assert(!memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)));

    reset();
    ((HSF_FACE *)face.data)->typeSrc = HSF_FACE_TRISTRIP;
    mp6_widescreen_extend_wall_edges(0, "unsupported", 0.0f, 2.0f, 1);
    assert(!grows && !entry.obj && vertex.count == 4 && face.count == 1);
}

static void reset_ring_fixture(void)
{
    reset();
    free(vertex.data); free(st.data); free(face.data);
    vertex.count = 17; st.count = 17; face.count = 16;
    vertex.data = calloc(17, sizeof(HuVecF));
    st.data = calloc(17, sizeof(HuVec2f));
    face.data = calloc(16, sizeof(HSF_FACE));
    ((HuVecF *)vertex.data)[0] = (HuVecF){0, 0, 0};
    ((HuVec2f *)st.data)[0] = (HuVec2f){.5f, .5f};
    for (int i = 0; i < 16; ++i) {
        float angle = (float)i * (6.283185307179586f / 16.0f);
        float x = cosf(angle), y = sinf(angle);
        ((HuVecF *)vertex.data)[i + 1] = (HuVecF){2.0f*x, 2.0f*y, 0.0f};
        ((HuVec2f *)st.data)[i + 1] = (HuVec2f){.5f + .2f*x, .5f + .2f*y};
        HSF_FACE *f = (HSF_FACE *)face.data + i;
        f->typeSrc = HSF_FACE_TRI;
        f->mat = 13;
        f->index[0] = strip_index(i + 1, i + 1, 5, -1);
        f->index[1] = strip_index((i + 1) % 16 + 1, (i + 1) % 16 + 1, 5, -1);
        f->index[2] = strip_index(0, 0, 5, -1);
    }
    object.mesh.mesh.min = (HuVecF){-2, -2, 0};
    object.mesh.mesh.max = (HuVecF){2, 2, 0};
}

static void test_ring_reflect_uv(void)
{
    HuVecF nativeV[17], nativeMin, nativeMax;
    HuVec2f nativeST[17];
    HSF_FACE nativeFaces[16];

    reset_ring_fixture();
    memcpy(nativeV, vertex.data, sizeof(nativeV));
    memcpy(nativeST, st.data, sizeof(nativeST));
    memcpy(nativeFaces, face.data, sizeof(nativeFaces));
    nativeMin = object.mesh.mesh.min;
    nativeMax = object.mesh.mesh.max;
    scale = 1.0f;
    mp6_ws_extend_ring_mode(0, "pattern-ring", TRUE);
    assert(grows == 1 && !refuse && entry.mode == MP6_WS_EXTEND_RING_REFLECT);
    assert(vertex.count == 33 && st.count == 33 && face.count == 32);
    assert(!memcmp(nativeV, vertex.data, sizeof(nativeV)));
    assert(!memcmp(nativeST, st.data, sizeof(nativeST)));
    assert(!memcmp(nativeFaces, face.data, sizeof(nativeFaces)));
    for (int i = 0; i < 16; ++i) {
        HuVecF p = ((HuVecF *)vertex.data)[17 + i];
        HuVec2f uv = ((HuVec2f *)st.data)[17 + i];
        assert(!memcmp(&p, &nativeV[i + 1], sizeof(p)));
        assert(!memcmp(&uv, &nativeST[i + 1], sizeof(uv)));
        HSF_FACE *f = (HSF_FACE *)face.data + 16 + i;
        for (int c = 0; c < 4; ++c) assert(f->index[c].normal == 5 && f->index[c].color == -1);
        assert(f->index[0].st >= 17 && f->index[0].st < 33);
        assert(f->index[2].st >= 17 && f->index[2].st < 33);
        assert(f->index[1].st < 17 && f->index[3].st < 17);
    }
    assert(!memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)));
    assert(!memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)));

    scale = 1.75f;
    mp6_ws_extend_ring_mode(0, "pattern-ring", TRUE);
    assert(grows == 1 && vertex.count == 33 && st.count == 33 && face.count == 32);
    assert(!memcmp(nativeV, vertex.data, sizeof(nativeV)));
    assert(!memcmp(nativeST, st.data, sizeof(nativeST)));
    assert(!memcmp(nativeFaces, face.data, sizeof(nativeFaces)));
    for (int i = 0; i < 16; ++i) {
        float angle = (float)i * (6.283185307179586f / 16.0f);
        float x = cosf(angle), y = sinf(angle);
        HuVecF p = ((HuVecF *)vertex.data)[17 + i];
        HuVec2f uv = ((HuVec2f *)st.data)[17 + i];
        HuVec2f rim = nativeST[i + 1], centre = nativeST[0];
        assert(fabsf(p.x - nativeV[i + 1].x * 1.75f) < .0001f);
        assert(fabsf(p.y - nativeV[i + 1].y * 1.75f) < .0001f && p.z == 0.0f);
        assert(fabsf(uv.x - (rim.x - (rim.x - centre.x) * .75f)) < .0001f);
        assert(fabsf(uv.y - (rim.y - (rim.y - centre.y) * .75f)) < .0001f);
        assert(fabsf(x*x + y*y - 1.0f) < .0001f);
    }
    scale = 1.0f;
    mp6_ws_extend_ring_mode(0, "pattern-ring", TRUE);
    assert(grows == 1 && vertex.count == 33 && st.count == 33);
    assert(!memcmp(nativeV, vertex.data, sizeof(nativeV)));
    assert(!memcmp(nativeST, st.data, sizeof(nativeST)));
    assert(!memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)));
    assert(!memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)));

    /* Real CP loops can reuse a rim vertex with a seam-specific ST index in
     * its neighboring chord. The reflected registrar must preserve both
     * source loops instead of choosing whichever face it visits first. */
    reset_ring_fixture();
    free(st.data);
    st.count = 33;
    st.data = calloc(33, sizeof(HuVec2f));
    assert(st.data != NULL);
    memcpy(st.data, nativeST, sizeof(nativeST));
    HSF_FACE seamFaces[16];
    memcpy(seamFaces, face.data, sizeof(seamFaces));
    for (int i = 0; i < 16; ++i) {
        float angle = (float)i * (6.283185307179586f / 16.0f);
        ((HuVec2f *)st.data)[17 + i] =
            (HuVec2f){.53f + .2f * cosf(angle), .47f + .2f * sinf(angle)};
        seamFaces[i].index[1].st = (s16)(17 + ((i + 1) % 16));
    }
    memcpy(face.data, seamFaces, sizeof(seamFaces));
    HuVec2f seamNativeST[33];
    memcpy(seamNativeST, st.data, sizeof(seamNativeST));
    scale = 1.0f;
    mp6_ws_extend_ring_mode(0, "seam-ring", TRUE);
    assert(grows == 1 && !refuse && entry.mode == MP6_WS_EXTEND_RING_REFLECT);
    assert(vertex.count == 33 && st.count == 65 && face.count == 32);
    assert(!memcmp(nativeV, vertex.data, sizeof(nativeV)));
    assert(!memcmp(seamNativeST, st.data, sizeof(seamNativeST)));
    assert(!memcmp(seamFaces, face.data, sizeof(seamFaces)));
    for (int i = 0; i < 16; ++i) {
        HSF_FACE *f = (HSF_FACE *)face.data + 16 + i;
        assert(f->index[0].st >= 33 && f->index[0].st < 65);
        assert(f->index[2].st >= 33 && f->index[2].st < 65);
    }
    scale = 1.5f;
    mp6_ws_extend_ring_mode(0, "seam-ring", TRUE);
    for (int i = 0; i < 16; ++i) {
        HSF_FACE *f = (HSF_FACE *)face.data + 16 + i;
        HuVec2f p = seamNativeST[seamFaces[i].index[0].st];
        HuVec2f q = seamNativeST[seamFaces[i].index[1].st];
        HuVec2f centre = seamNativeST[seamFaces[i].index[2].st];
        HuVec2f actualP = ((HuVec2f *)st.data)[f->index[2].st];
        HuVec2f actualQ = ((HuVec2f *)st.data)[f->index[0].st];
        assert(fabsf(actualP.x - (p.x - (p.x - centre.x) * .5f)) < .0001f);
        assert(fabsf(actualP.y - (p.y - (p.y - centre.y) * .5f)) < .0001f);
        assert(fabsf(actualQ.x - (q.x - (q.x - centre.x) * .5f)) < .0001f);
        assert(fabsf(actualQ.y - (q.y - (q.y - centre.y) * .5f)) < .0001f);
    }
    scale = 1.0f;
    mp6_ws_extend_ring_mode(0, "seam-ring", TRUE);
    assert(!memcmp(seamNativeST, st.data, sizeof(seamNativeST)));

    /* The legacy registration remains pinned and does not allocate STs. */
    reset_ring_fixture();
    scale = 1.75f;
    mp6_ws_extend_ring_mode(0, "legacy-ring", FALSE);
    assert(grows == 1 && !refuse && entry.mode == MP6_WS_EXTEND_RING);
    assert(vertex.count == 33 && st.count == 17 && face.count == 32);
    assert(!memcmp(nativeST, st.data, sizeof(nativeST)));
    for (int i = 0; i < 16; ++i) {
        HSF_FACE *f = (HSF_FACE *)face.data + 16 + i;
        assert(f->index[0].st < 17 && f->index[2].st < 17);
    }
}

static void reset_sky_column(void)
{
    reset();
    const HuVecF positions[6] = {
        {-2350,2350,300}, {-2350,-750,0}, {2350,2350,300},
        {2350,-750,0}, {2350,1325,0}, {-2350,1325,0},
    };
    const HuVec2f texcoords[8] = {
        {0,.03125f}, {0,1}, {2,1}, {2,.03125f},
        {2,.03125f}, {2,0}, {0,0}, {0,.03125f},
    };
    const int vertices[2][4] = {{5,1,4,3},{4,2,5,0}};
    const int texture[2][4] = {{0,1,3,2},{4,5,7,6}};
    vertex.count = 6; st.count = 8; face.count = 2;
    vertex.data = realloc(vertex.data, sizeof(positions));
    st.data = realloc(st.data, sizeof(texcoords));
    face.data = realloc(face.data, 2*sizeof(HSF_FACE));
    memcpy(vertex.data, positions, sizeof(positions));
    memcpy(st.data, texcoords, sizeof(texcoords));
    memset(face.data, 0, 2*sizeof(HSF_FACE));
    for (int f = 0; f < 2; ++f) {
        HSF_FACE *q = (HSF_FACE *)face.data + f;
        q->typeSrc = HSF_FACE_QUAD; q->mat = 13;
        for (int c = 0; c < 4; ++c)
            q->index[c] = strip_index(vertices[f][c], texture[f][c], f*4+c, -1);
    }
    object.mesh.mesh.min = (HuVecF){-2350,-750,0};
    object.mesh.mesh.max = (HuVecF){2350,2350,300};
}

static void test_sky_column(void)
{
    reset_sky_column();
    HuVecF nativeV[6]; HuVec2f nativeUV[8]; HSF_FACE nativeFaces[2];
    memcpy(nativeV, vertex.data, sizeof(nativeV));
    memcpy(nativeUV, st.data, sizeof(nativeUV));
    memcpy(nativeFaces, face.data, sizeof(nativeFaces));
    for (int tick = 0; tick < 20; ++tick) {
        const float aspects[4] = {1, 1.325f, 1.75f, 1};
        scale = aspects[tick%4];
        mp6_widescreen_extend_backdrop_frame(0, "grid83");
        assert(grows == 1 && entry.edgeCount == 6);
        assert(vertex.count == 18 && st.count == 20 && face.count == 8);
        assert(!memcmp(nativeV, vertex.data, sizeof(nativeV)));
        assert(!memcmp(nativeUV, st.data, sizeof(nativeUV)));
        assert(!memcmp(nativeFaces, face.data, sizeof(nativeFaces)));
        for (int i = 0; i < entry.edgeCount; ++i) {
            MP6WSEdge *e = &entry.edges[i];
            assert(!(e->pos[0].y == 1325 && e->pos[1].y == 1325));
            assert(e->face >= 0 && e->face < 2);
            HSF_FACE *f = (HSF_FACE *)face.data + 2+i;
            assert(f->mat == nativeFaces[e->face].mat);
            assert(!memcmp(&f->index[0], &nativeFaces[e->face].index[e->corner[1]], sizeof(HSF_FACE_INDEX)));
            assert(!memcmp(&f->index[2], &nativeFaces[e->face].index[e->corner[0]], sizeof(HSF_FACE_INDEX)));
            for (int j = 0; j < 2; ++j) {
                HuVecF p = ((HuVecF *)vertex.data)[6+2*i+j];
                assert(fabsf(p.x-e->pos[j].x*scale) < .001f);
                assert(fabsf(p.y-(800+(e->pos[j].y-800)*scale)) < .001f);
                assert(p.z == e->pos[j].z);
                for (int k = 0; k < entry.edgeCount; ++k) for (int n = 0; n < 2; ++n)
                    if (!memcmp(&e->pos[j], &entry.edges[k].pos[n], sizeof(HuVecF))) {
                        assert(!memcmp(&p, &((HuVecF *)vertex.data)[6+2*k+n], sizeof(p)));
                        assert(!memcmp(&((HuVec2f *)st.data)[8+2*i+j],
                                       &((HuVec2f *)st.data)[8+2*k+n], sizeof(HuVec2f)));
                    }
            }
        }
    }
    reset_sky_column(); ((HuVec2f *)st.data)[4].x = 3;
    mp6_widescreen_extend_backdrop_frame(0, "uv-seam"); assert(!grows);
    reset_sky_column(); ((HuVecF *)vertex.data)[0].z += 50;
    mp6_widescreen_extend_backdrop_frame(0, "twisted-row"); assert(!grows);
    reset_sky_column(); ((HSF_FACE *)face.data)[1] = ((HSF_FACE *)face.data)[0];
    mp6_widescreen_extend_backdrop_frame(0, "overlap"); assert(!grows);
    reset_sky_column(); fail = 1;
    mp6_widescreen_extend_backdrop_frame(0, "failed-column");
    assert(!grows && !entry.obj && vertex.count == 6 && face.count == 2);
}

static void test_tiled_sloped_wall(void)
{
    reset();
    HuVecF *v=vertex.data;
    v[1].z=1.03f; v[1].y=v[3].y=1;
    v[1].x+=.2f; v[3].x+=.2f;
    HuVecF nativeV[4]; HuVec2f nativeST[4]; HSF_FACE nativeF=*(HSF_FACE *)face.data;
    memcpy(nativeV,vertex.data,sizeof nativeV); memcpy(nativeST,st.data,sizeof nativeST);
    scale=1;
    mp6_widescreen_extend_tiled_sloped_rim(0,"wall",2,1.03f,0,.05f);
    assert(grows==1 && !refuse && entry.edgeTiled && entry.edgeSloped);
    assert(entry.edgeCount==8 && face.count==9 && vertex.count==20 && st.count==36);
    for(int cycle=0;cycle<100;++cycle) {
        scale=cycle%4==0?1:cycle%4==1?1.325f:cycle%4==2?1.75f:3.5f;
        mp6_widescreen_extend_tiled_sloped_rim(0,"wall",2,1.03f,0,.05f);
        assert(grows==1 && !memcmp(nativeV,vertex.data,sizeof nativeV));
        assert(!memcmp(nativeST,st.data,sizeof nativeST) && !memcmp(&nativeF,face.data,sizeof nativeF));
        float distance=(scale-1)*2.03f;
        for(int i=0;i<8;++i) {
            MP6WSEdge *e=&entry.edges[i]; HSF_FACE *f=(HSF_FACE *)face.data+1+i;
            assert(f->mat==13 && f->index[0].color==8 && f->index[0].normal==5);
            if(i) {
                assert(f->index[0].vertex==4+(i-1)*2+1);
                assert(f->index[2].vertex==4+(i-1)*2);
            }
            assert(f->index[0].st==4+i*4+1 && f->index[2].st==4+i*4);
            assert(f->index[1].st==4+i*4+3 && f->index[3].st==4+i*4+2);
            for(int j=0;j<2;++j) {
                HuVecF p=((HuVecF *)vertex.data)[4+i*2+j];
                HuVec2f inner=((HuVec2f *)st.data)[4+i*4+j];
                HuVec2f uv=((HuVec2f *)st.data)[4+i*4+2+j];
                float move=fminf(distance,e->tileOffset[j]+e->tileSpan[j]);
                assert(fabsf(p.z-(e->pos[j].z+move))<.00001f);
                assert(fabsf(p.y-(e->pos[j].y+move*e->posSlope[j].y))<.00001f);
                assert(uv.x>=0 && uv.x<=1 && uv.y>=0 && uv.y<=1);
                assert(!memcmp(&inner,&e->tileInnerUV[j],sizeof(inner)));
                if(i && distance>=e->tileOffset[j]) {
                    HuVec2f previous=((HuVec2f *)st.data)[4+(i-1)*4+2+j];
                    assert(fabsf(previous.y-inner.y)>.99f); /* atlas end/start block joint */
                }
            }
        }
    }
    reset();
    for(int i=0;i<4;++i) ((HuVecF *)vertex.data)[i].z *= -1;
    scale=1.75f;
    mp6_widescreen_extend_tiled_sloped_rim(0,"right-wall",2,-1,0,.05f);
    assert(grows==1 && entry.edgeReach<0 && ((HuVecF *)vertex.data)[4].z < -1);
    for(int i=4;i<st.count;++i) {
        HuVec2f uv=((HuVec2f *)st.data)[i];
        assert(uv.x>=0 && uv.x<=1 && uv.y>=0 && uv.y<=1);
    }
    /* Connected profile faces have unequal longitudinal spans. Their shared
     * vertex must stay welded at every atlas tile, not just at the source. */
    reset();
    vertex.data=realloc(vertex.data,6*sizeof(HuVecF)); vertex.count=6;
    st.data=realloc(st.data,6*sizeof(HuVec2f)); st.count=6;
    face.data=realloc(face.data,2*sizeof(HSF_FACE)); face.count=2;
    v=vertex.data; v[1].z=1.03f;
    v[4]=(HuVecF){2,1,-.9f}; v[5]=(HuVecF){2.2f,1,1.01f};
    ((HuVec2f *)st.data)[4]=(HuVec2f){0,.2f};
    ((HuVec2f *)st.data)[5]=(HuVec2f){1,.2f};
    HSF_FACE *pair=face.data; pair[1]=pair[0];
    const int connected[4]={2,3,4,5};
    for(int c=0;c<4;++c) pair[1].index[c].vertex=pair[1].index[c].st=connected[c];
    for(int cycle=0;cycle<100;++cycle) {
        scale=1+(cycle%40)*.1f;
        mp6_widescreen_extend_tiled_sloped_rim(0,"joined-wall",2,1.03f,3,.05f);
        assert(grows==1 && entry.edgeCount==16 && entry.edgeReach==3);
        for(int i=0;i<8;++i) {
            HuVecF *first=(HuVecF *)vertex.data+6+i*2;
            HuVecF *second=(HuVecF *)vertex.data+6+(8+i)*2+1;
            assert(!memcmp(first,second,sizeof(*first)));
        }
    }
    reset(); fail=1;
    mp6_widescreen_extend_tiled_sloped_rim(0,"wall",2,1,0,.05f);
    assert(!grows && !entry.obj);
    reset(); mp6_widescreen_extend_tiled_sloped_rim(0,"wall",2,1,0,2);
    assert(!grows);
}

int main(void)
{
    reset(); selectedOccurrence = 1;
    mp6_widescreen_extend_tiled_sloped_rim_at(0, "obj24", 0, 2, 1, 8, .1f);
    assert(grows == 0);
    mp6_widescreen_extend_tiled_sloped_rim_at(0, "obj24", 1, 2, 1, 8, .1f);
    assert(grows == 1 && entry.edgeCount == 8 && face.count == 9);
    scale = 1;
    mp6_widescreen_extend_tiled_sloped_rim_at(0, "obj24", 1, 2, 1, 8, .1f);
    assert(grows == 1);
    for (int i = 4; i < vertex.count; ++i)
        assert(((HuVecF *)vertex.data)[i].z == 1);
    test_sky_column();
    /* Sky frames must cover side AND lower-edge reveal without moving the
     * original quad. Corner positions are shared by adjacent border panels. */
    reset();
    HuVecF sky[4] = {{2,-6,7},{2,-4,7},{4,-6,7},{4,-4,7}};
    memcpy(vertex.data, sky, sizeof(sky));
    HuVec2f skyUV[4]; memcpy(skyUV, st.data, sizeof(skyUV));
    HSF_FACE skyFace = *(HSF_FACE *)face.data;
    object.mesh.mesh.min = sky[0]; object.mesh.mesh.max = sky[3];
    for (int tick = 0; tick < 100; ++tick) {
        const float ratios[4] = {1, 1.325f, 1.75f, 1};
        scale = ratios[tick%4];
        mp6_widescreen_extend_backdrop_frame(0, "sky");
        assert(grows == 1 && entry.edgeCount == 4 && vertex.count == 12 && face.count == 5);
        assert(!memcmp(sky, vertex.data, sizeof(sky)) && !memcmp(skyUV, st.data, sizeof(skyUV)));
        assert(!memcmp(&skyFace, face.data, sizeof(skyFace)));
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 2; ++j) {
            MP6WSEdge *edge = &entry.edges[i];
            HuVecF p = ((HuVecF *)vertex.data)[4+i*2+j];
            assert(fabsf(p.x-(3+(edge->pos[j].x-3)*scale)) < .0001f);
            assert(fabsf(p.y-(-5+(edge->pos[j].y+5)*scale)) < .0001f && p.z == 7);
            HuVec2f t = ((HuVec2f *)st.data)[4+i*2+j];
            assert(fabsf(t.x-(edge->uv[j].x+(.5f-edge->uv[j].x)*(scale-1))) < .0001f);
            assert(fabsf(t.y-(edge->uv[j].y+(.5f-edge->uv[j].y)*(scale-1))) < .0001f);
            if (j == 1) assert(!memcmp(&p, &((HuVecF *)vertex.data)[4+((i+1)%4)*2], sizeof(p)));
            HSF_FACE *f = face.data;
            assert(f[1+i].mat == skyFace.mat);
            assert(!memcmp(&f[1+i].index[0], &skyFace.index[edge->corner[1]], sizeof(HSF_FACE_INDEX)));
            assert(!memcmp(&f[1+i].index[2], &skyFace.index[edge->corner[0]], sizeof(HSF_FACE_INDEX)));
            assert(f[1+i].index[3].vertex == 4+i*2 && f[1+i].index[1].vertex == 5+i*2);
            assert(f[1+i].index[3].st == 4+i*2 && f[1+i].index[1].st == 5+i*2);
            for (int c = 0; c < 4; ++c) assert(f[1+i].index[c].normal == 5 && f[1+i].index[c].color == 8);
        }
        if (scale == 1) {
            assert(!memcmp(&object.mesh.mesh.min, &sky[0], sizeof(HuVecF)));
            assert(!memcmp(&object.mesh.mesh.max, &sky[3], sizeof(HuVecF)));
        }
    }
    reset(); memcpy(vertex.data, sky, sizeof(sky)); fail = 1;
    mp6_widescreen_extend_backdrop_frame(0, "failed"); assert(!grows && !entry.obj && vertex.count == 4);
    reset(); mp6_widescreen_extend_backdrop_frame(0, "not-xy"); assert(!grows);
    reset(); memcpy(vertex.data, sky, sizeof(sky)); ((HuVecF *)vertex.data)[3].x = 3;
    mp6_widescreen_extend_backdrop_frame(0, "not-rectangle"); assert(!grows);
    reset(); memcpy(vertex.data, sky, sizeof(sky)); ((HSF_FACE *)face.data)->index[0].vertex = 7;
    mp6_widescreen_extend_backdrop_frame(0, "bad-index"); assert(!grows);
    reset(); memcpy(vertex.data, sky, sizeof(sky)); ((HuVec2f *)st.data)[0].x = NAN;
    mp6_widescreen_extend_backdrop_frame(0, "bad-uv"); assert(!grows);

    /* Tilted-box rim: follow each sloping side edge, not a world-axis
     * translation. Native wall, floor-height edge and cap never move. */
    reset();
    HuVecF slopeV[4] = {{-1,0,0},{1,0,0},{-1.5f,2,-2},{1.5f,2,-2}};
    HuVec2f slopeUV[4] = {{.2f,1},{.8f,1},{0,.2f},{1,.2f}};
    memcpy(vertex.data, slopeV, sizeof(slopeV));
    memcpy(st.data, slopeUV, sizeof(slopeUV));
    object.mesh.mesh.min = (HuVecF){-2,-1,-3};
    object.mesh.mesh.max = (HuVecF){2,3,1}; /* authored culling margin */
    HuVecF nativeMin = object.mesh.mesh.min, nativeMax = object.mesh.mesh.max;
    face.data = realloc(face.data, 2*sizeof(HSF_FACE)); face.count = 2;
    HSF_FACE *sf = face.data;
    sf[1] = sf[0];
    sf[1].index[0] = sf[1].index[1] = sf[0].index[2];
    sf[1].index[2] = sf[1].index[3] = sf[0].index[3];
    HSF_FACE slopeFaces[2]; memcpy(slopeFaces, face.data, sizeof(slopeFaces));
    const float aspects[] = {1, 1.325f, 1.75f, 1};
    for (int tick=0; tick<100; ++tick) {
        scale = aspects[tick%4];
        mp6_widescreen_extend_sloped_rim(0, "box-wall", 1, 2);
        assert(grows == 1 && !refuse && entry.edgeSloped && entry.edgeCount == 1);
        assert(vertex.count == 6 && st.count == 6 && face.count == 3);
        assert(!memcmp(slopeV, vertex.data, sizeof(slopeV)));
        assert(!memcmp(slopeUV, st.data, sizeof(slopeUV)));
        assert(!memcmp(slopeFaces, face.data, sizeof(slopeFaces)));
        for (int j=0; j<2; ++j) {
            HuVecF p = ((HuVecF *)vertex.data)[4+j];
            HuVec2f uv = ((HuVec2f *)st.data)[4+j];
            int c = entry.edges[0].corner[j];
            float side = slopeV[c].x < 0 ? -1 : 1;
            assert(fabsf(p.x-side*(1+.5f*scale)) < .0001f);
            assert(fabsf(p.y-2*scale) < .0001f && fabsf(p.z+2*scale) < .0001f);
            assert(fabsf(uv.x-(slopeUV[c].x-side*.2f*(scale-1))) < .0001f);
            assert(fabsf(uv.y-(.2f+.8f*(scale-1))) < .0001f);
            if (scale == 1) assert(!memcmp(&p, &slopeV[c], sizeof(p)));
        }
        if (scale == 1) {
            assert(!memcmp(&nativeMin, &object.mesh.mesh.min, sizeof(nativeMin)));
            assert(!memcmp(&nativeMax, &object.mesh.mesh.max, sizeof(nativeMax)));
        }
    }
    reset(); fail = 1;
    mp6_widescreen_extend_sloped_rim(0, "failed", 0, 1);
    assert(!grows && !entry.obj && vertex.count == 4 && face.count == 1);
    reset(); mp6_widescreen_extend_sloped_rim(0, "interior", 0, 0);
    assert(!grows && refuse);
    reset(); mp6_widescreen_extend_sloped_rim(0, "invalid", 1, NAN);
    assert(!grows);

    /* A separate inner patch can have exposed edges but must not grow across
     * a river or another terrain object. Only the extreme X borders qualify. */
    for (int mirror=0;mirror<2;++mirror) {
        reset();
        vertex.data=realloc(vertex.data,8*sizeof(HuVecF)); vertex.count=8;
        st.data=realloc(st.data,8*sizeof(HuVec2f)); st.count=8;
        face.data=realloc(face.data,2*sizeof(HSF_FACE)); face.count=2;
        HSF_FACE *f=face.data; f[1]=f[0]; f[1].mat=19;
        for (int i=0;i<4;++i) {
            HuVecF p=((HuVecF *)vertex.data)[i]; p.x*=.5f; p.y=1;
            ((HuVecF *)vertex.data)[i+4]=p;
            ((HuVec2f *)st.data)[i+4]=((HuVec2f *)st.data)[i];
            f[1].index[i].vertex=f[1].index[i].st=i+4;
        }
        HuVecF native[8]; HuVec2f nativeUV[8]; HSF_FACE nativeFaces[2];
        memcpy(native,vertex.data,sizeof(native)); memcpy(nativeUV,st.data,sizeof(nativeUV));
        memcpy(nativeFaces,face.data,sizeof(nativeFaces));
        for (int tick=0;tick<100;++tick) {
            scale=tick%2?1:1.75f;
            mp6_widescreen_extend_terrain_outer_sides(0,"background",0,2,mirror);
            assert(grows==1 && entry.edgeCount==2 && vertex.count==12 && face.count==4);
            assert(!memcmp(native,vertex.data,sizeof(native)));
            assert(!memcmp(nativeUV,st.data,sizeof(nativeUV)));
            assert(!memcmp(nativeFaces,face.data,sizeof(nativeFaces)));
            for (int i=8;i<12;++i) {
                HuVecF p=((HuVecF *)vertex.data)[i]; HuVec2f t=((HuVec2f *)st.data)[i];
                float side=p.x<0?-1:1, expected=1+2*(scale-1);
                assert(fabsf(fabsf(p.x)-expected)<.001f && p.y==0);
                float sample=mirror?2*side-p.x:p.x;
                assert(fabsf(t.x-(sample+1)/2)<.001f && fabsf(t.y-(p.z+1)/2)<.001f);
            }
            for(int i=2;i<4;++i) assert(((HSF_FACE *)face.data)[i].mat==13);
        }
    }
    reset(); mp6_widescreen_extend_terrain_outer_sides(0,"bad",0,NAN,1); assert(!grows);
    reset(); mp6_widescreen_extend_terrain_outer_sides(0,"bad",0,0,1); assert(!grows);
    /* Side-only terrain continuation: a sloping fade ring must retain every
     * native face, height, depth, color/normal index, and texture density. */
    for (int triangles=0; triangles<2; ++triangles) for (int mirror=0; mirror<2; ++mirror) {
        reset(); scale=1.75f;
        HuVecF *v=vertex.data;
        for (int i=0;i<4;++i) { v[i].x+=5; v[i].y=(v[i].z+1)*.25f; }
        if (triangles) {
            HSF_FACE quad=*(HSF_FACE *)face.data;
            face.data=realloc(face.data,2*sizeof(HSF_FACE)); face.count=2;
            HSF_FACE *f=face.data;
            f[0]=f[1]=quad; f[0].typeSrc=f[1].typeSrc=HSF_FACE_TRI;
            f[0].index[0]=quad.index[0]; f[0].index[2]=quad.index[2]; f[0].index[1]=quad.index[1];
            f[1].index[0]=quad.index[2]; f[1].index[2]=quad.index[3]; f[1].index[1]=quad.index[1];
        }
        HuVecF native[4]; HuVec2f nativeUV[4]; HSF_FACE nativeFaces[2];
        memcpy(native,vertex.data,sizeof(native)); memcpy(nativeUV,st.data,sizeof(nativeUV));
        int nativeCount=face.count;
        memcpy(nativeFaces,face.data,nativeCount*sizeof(HSF_FACE));
        for (int tick=0;tick<100;++tick) {
            scale=tick%2?1:1.75f;
            mp6_widescreen_extend_terrain_sides(0,"terrain",5,mirror);
            assert(grows==1 && !refuse && entry.edgeCount==2 && vertex.count==8 && face.count==nativeCount+2);
            assert(!memcmp(native,vertex.data,sizeof(native)) && !memcmp(nativeUV,st.data,sizeof(nativeUV)));
            assert(!memcmp(nativeFaces,face.data,nativeCount*sizeof(HSF_FACE)));
            for(int i=0;i<2;++i) for(int j=0;j<2;++j) {
                MP6WSEdge *edge=&entry.edges[i];
                HuVecF p=((HuVecF *)vertex.data)[4+2*i+j], base=edge->pos[j];
                HuVec2f t=((HuVec2f *)st.data)[4+2*i+j];
                assert(fabsf(p.x-(5+(base.x-5)*scale))<.001f && p.y==base.y && p.z==base.z);
                float sample=mirror?2*base.x-p.x:p.x;
                assert(fabsf(t.x-(sample-4)/2)<.001f && fabsf(t.y-(p.z+1)/2)<.001f);
                HSF_FACE *f=face.data;
                assert(f[nativeCount+i].mat==13);
                for (int c=0;c<4;++c) assert(f[nativeCount+i].index[c].normal==5 && f[nativeCount+i].index[c].color==8);
            }
        }
    }
    reset(); ((HSF_FACE *)face.data)->typeSrc=HSF_FACE_TRISTRIP;
    mp6_widescreen_extend_terrain_sides(0,"unknown-tail",0,0); assert(!grows);
    reset(); ((HSF_FACE *)face.data)->index[2].vertex=9;
    mp6_widescreen_extend_terrain_sides(0,"invalid",0,0); assert(!grows);
    reset(); ((HuVec2f *)st.data)[1].y=NAN;
    mp6_widescreen_extend_terrain_sides(0,"invalid",0,0); assert(!grows);
    reset(); fail=1; mp6_widescreen_extend_terrain_sides(0,"allocation-failure",0,0);
    assert(!grows && !entry.obj && face.count==1 && vertex.count==4);
    /* Material-filtered ground beneath a tint: decorative geometry reaches
     * farther than the actual substrate, and must not affect its boundary. */
    for (int mirror=0; mirror<2; ++mirror) {
        reset(); scale=1.75f;
        vertex.data=realloc(vertex.data,8*sizeof(HuVecF)); vertex.count=8;
        st.data=realloc(st.data,8*sizeof(HuVec2f)); st.count=8;
        face.data=realloc(face.data,2*sizeof(HSF_FACE)); face.count=2;
        HSF_FACE *f=face.data; f[1]=f[0]; f[1].mat=19;
        f[0].mat=(s16)(0x8000|13); /* Face flags are not the material index. */
        for(int i=0;i<4;++i) {
            HuVecF p=((HuVecF *)vertex.data)[i]; p.x*=3; p.y=2;
            ((HuVecF *)vertex.data)[i+4]=p;
            ((HuVec2f *)st.data)[i+4]=((HuVec2f *)st.data)[i];
            f[1].index[i].vertex=f[1].index[i].st=i+4;
        }
        HuVecF native[8]; HuVec2f nativeUV[8]; HSF_FACE nativeFaces[2];
        memcpy(native,vertex.data,sizeof(native)); memcpy(nativeUV,st.data,sizeof(nativeUV));
        memcpy(nativeFaces,face.data,sizeof(nativeFaces));
        mp6_widescreen_extend_material_sides(0,"layered-ground",13,mirror);
        assert(grows==1 && !refuse && entry.edgeCount==2 && vertex.count==12 && face.count==4);
        for(int tick=0;tick<100;++tick) {
            scale=tick%2?1:1.75f;
            mp6_widescreen_extend_material_sides(0,"layered-ground",13,mirror);
            assert(grows==1 && !memcmp(native,vertex.data,sizeof(native)));
            assert(!memcmp(nativeUV,st.data,sizeof(nativeUV)) && !memcmp(nativeFaces,face.data,sizeof(nativeFaces)));
            for(int i=8;i<12;++i) {
                HuVecF p=((HuVecF *)vertex.data)[i]; HuVec2f uv=((HuVec2f *)st.data)[i];
                float side=p.x<0?-1:1, sample=mirror?side*2-p.x:p.x;
                assert(fabsf(fabsf(p.x)-scale)<.001f && p.y==0);
                assert(fabsf(uv.x-(sample+1)/2)<.001f);
            }
        }
        for(int i=2;i<4;++i) assert(((HSF_FACE *)face.data)[i].mat==(s16)(0x8000|13));
    }
    reset(); mp6_widescreen_extend_material_sides(0,"missing",12,1); assert(!grows);
    reset(); mp6_widescreen_extend_material_sides(0,"invalid",-1,1); assert(!grows);
    reset(); mp6_widescreen_extend_material_sides(0,"invalid",4096,1); assert(!grows);
    reset(); ((HSF_FACE *)face.data)->typeSrc=HSF_FACE_TRI;
    mp6_widescreen_extend_material_sides(0,"nonrect",13,1); assert(!grows);
    reset(); ((HSF_FACE *)face.data)->index[0].vertex=4;
    mp6_widescreen_extend_material_sides(0,"invalid",13,1); assert(!grows);
    reset(); ((HuVec2f *)st.data)[0].x=NAN;
    mp6_widescreen_extend_material_sides(0,"invalid",13,1); assert(!grows);
    reset(); fail=1; mp6_widescreen_extend_material_sides(0,"alloc-failure",13,1);
    assert(!grows && !entry.obj && face.count==1 && vertex.count==4);
    for (int mirror=0; mirror<2; ++mirror) {
        reset(); scale=1.75f;
        HuVecF nativeV[4]; HuVec2f nativeUV[4]; HSF_FACE nativeFace=*(HSF_FACE *)face.data;
        memcpy(nativeV,vertex.data,sizeof(nativeV));memcpy(nativeUV,st.data,sizeof(nativeUV));
        /* Nonzero centre must not shift the native plane. */
        for(int i=0;i<4;++i) { ((HuVecF *)vertex.data)[i].x+=7; nativeV[i].x+=7; }
        mp6_widescreen_extend_sides(0,"lava",mirror);
        assert(grows==1 && !refuse && entry.edgeCount==2 && vertex.count==8 && face.count==3);
        for(int tick=0;tick<100;++tick) {
            scale=tick%2?1:1.75f;mp6_widescreen_extend_sides(0,"lava",mirror);
            assert(grows==1 && !memcmp(vertex.data,nativeV,sizeof(nativeV)));
            assert(!memcmp(st.data,nativeUV,sizeof(nativeUV)) && !memcmp(face.data,&nativeFace,sizeof(nativeFace)));
            for(int i=4;i<8;++i) {
                HuVecF p=((HuVecF *)vertex.data)[i]; HuVec2f uv=((HuVec2f *)st.data)[i];
                assert(fabsf(fabsf(p.x-7)-scale)<.001f);
                float side=p.x<7?-1:1;
                float sample=mirror?side*2-(p.x-7):p.x-7;
                assert(fabsf(uv.x-(sample+1)/2)<.001f && fabsf(uv.y-(p.z+1)/2)<.001f);
            }
        }
    }
    for (int axis=0; axis<=2; axis+=2) for (int side=-1; side<=1; side+=2) {
        reset(); scale = 1.75f;
        HuVecF nativeV[4]; HuVec2f nativeUV[4]; HSF_FACE nativeFace = *(HSF_FACE *)face.data;
        memcpy(nativeV, vertex.data, sizeof(nativeV)); memcpy(nativeUV, st.data, sizeof(nativeUV));
        mp6_widescreen_extend_edge(0, "floor", axis, (float)side);
        assert(grows == 1 && refuse == 0 && vertex.count == 6 && st.count == 6 && face.count == 2);
        assert(memcmp(nativeV, vertex.data, sizeof(nativeV)) == 0);
        assert(memcmp(nativeUV, st.data, sizeof(nativeUV)) == 0);
        assert(memcmp(&nativeFace, face.data, sizeof(nativeFace)) == 0);
        for (int i=4; i<6; ++i) {
            HuVecF p = ((HuVecF *)vertex.data)[i];
            HuVec2f uv = ((HuVec2f *)st.data)[i];
            assert(fabsf(mp6_ws_edge_coord(p, axis) - side * 2.5f) < 0.001f);
            assert(fabsf(uv.x - (p.x+1)/2) < 0.001f && fabsf(uv.y - (p.z+1)/2) < 0.001f);
        }
        HSF_FACE *f = (HSF_FACE *)face.data + 1;
        assert(f->mat == 13);
        for (int i=0; i<4; ++i) assert(f->index[i].normal == 5 && f->index[i].color == 8);
        /* Same winding as the source, not a back-facing strip. */
        HuVecF *v = vertex.data, a=v[f->index[0].vertex], b=v[f->index[2].vertex], c=v[f->index[3].vertex];
        assert((b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z) < 0);
        scale = 1;
        mp6_widescreen_extend_edge(0, "floor", axis, (float)side);
        assert(grows == 1);
        for (int i=4; i<6; ++i) assert(mp6_ws_edge_coord(((HuVecF *)vertex.data)[i], axis) == side);
        assert(memcmp(&object.mesh.mesh.min, &nativeV[0], sizeof(HuVecF)) == 0);
        assert(memcmp(&object.mesh.mesh.max, &nativeV[3], sizeof(HuVecF)) == 0);
    }
    reset(); mp6_widescreen_extend_edge(0, "floor", 2, 0); assert(!grows && refuse);
    reset(); fail = 1; mp6_widescreen_extend_edge(0, "floor", 2, 1);
    assert(!grows && !entry.obj && vertex.count == 4);
    reset(); ((HuVecF *)vertex.data)[3].x += 0.5f;
    mp6_widescreen_extend_edge(0, "floor", 2, 1); assert(!grows && refuse);

    /* Narrow walls need frustum-sized reach, not their own tiny width.
     * A mixed mesh must leave all original pipe/decoration faces untouched. */
    for (int side=-1; side<=1; side+=2) {
        reset(); scale = 1.75f;
        face.data = realloc(face.data, 2 * sizeof(HSF_FACE)); face.count = 2;
        HSF_FACE *f = face.data;
        f[1] = f[0]; f[1].typeSrc = HSF_FACE_TRI; f[1].mat = 19;
        HuVecF native[4]; HSF_FACE nativeFaces[2];
        memcpy(native, vertex.data, sizeof(native)); memcpy(nativeFaces, face.data, sizeof(nativeFaces));
        mp6_widescreen_extend_edge_span(0, "wall-and-pipe", 0, side, 800);
        assert(grows == 1 && entry.edgeCount == 1 && face.count == 3);
        assert(memcmp(native, vertex.data, sizeof(native)) == 0);
        assert(memcmp(nativeFaces, face.data, sizeof(nativeFaces)) == 0);
        for (int i=4; i<6; ++i) {
            HuVecF p = ((HuVecF *)vertex.data)[i];
            assert(fabsf(p.x - side * 601) < .001f);
            assert(fabsf(((HuVec2f *)st.data)[i].x - (p.x+1)/2) < .001f);
        }
        scale = 1;
        mp6_widescreen_extend_edge_span(0, "wall-and-pipe", 0, side, 800);
        assert(grows == 1);
        for (int i=4; i<6; ++i) assert(((HuVecF *)vertex.data)[i].x == side);
    }
    reset(); mp6_widescreen_extend_edge_span(0, "invalid", 0, 1, -1); assert(!grows);
    reset(); mp6_widescreen_extend_edge_span(0, "invalid", 0, 1, NAN); assert(!grows);
    reset(); mp6_widescreen_extend_edge_span(0, "invalid", 0, INFINITY, 800); assert(!grows);
    reset(); ((HSF_FACE *)face.data)->index[0].vertex = -1;
    mp6_widescreen_extend_edge(0, "invalid-index", 0, 1); assert(!grows && refuse);

    /* A triangulated annular floor: expand only its outer contour, not the
     * inner gameplay hole or the shared diagonals. Test off-center too. */
    for (int offset = 0; offset < 2; ++offset) {
        reset(); scale = 1.75f;
        free(vertex.data); free(st.data); free(face.data);
        vertex.count = st.count = 8; face.count = 8;
        vertex.data = calloc(8, sizeof(HuVecF)); st.data = calloc(8, sizeof(HuVec2f));
        face.data = calloc(8, sizeof(HSF_FACE));
        HuVecF ring[8] = {{-2,3,-2},{2,3,-2},{2,3,2},{-2,3,2},
                         {-1,3,-1},{1,3,-1},{1,3,1},{-1,3,1}};
        HuVec2f nativeUV[8];
        HSF_FACE nativeFaces[8];
        float cx = 7 * offset, cz = -4 * offset;
        for (int i=0; i<8; ++i) {
            ring[i].x += cx; ring[i].z += cz;
            nativeUV[i] = (HuVec2f){ring[i].x/2, ring[i].z/2};
        }
        memcpy(vertex.data, ring, sizeof(ring)); memcpy(st.data, nativeUV, sizeof(nativeUV));
        for (int i=0; i<4; ++i) {
            int next = (i+1)%4;
            int corners[2][3] = {{i,next,next+4},{i,next+4,i+4}};
            for (int t=0; t<2; ++t) {
                HSF_FACE *f = (HSF_FACE *)face.data + i*2+t;
                f->typeSrc = HSF_FACE_TRI; f->mat = 13;
                for (int j=0; j<3; ++j) {
                    f->index[j].vertex = f->index[j].st = corners[t][j];
                    f->index[j].normal = 5; f->index[j].color = 8;
                }
            }
        }
        memcpy(nativeFaces, face.data, sizeof(nativeFaces));
        mp6_widescreen_extend_floor(0, "ring", 3, cx, cz);
        assert(grows == 1 && !refuse && entry.edgeCount == 4);
        assert(vertex.count == 16 && st.count == 16 && face.count == 12);
        assert(memcmp(vertex.data, ring, sizeof(ring)) == 0);
        assert(memcmp(st.data, nativeUV, sizeof(nativeUV)) == 0);
        assert(memcmp(face.data, nativeFaces, sizeof(nativeFaces)) == 0);
        for (int i=8; i<16; ++i) {
            HuVecF p = ((HuVecF *)vertex.data)[i];
            HuVec2f uv = ((HuVec2f *)st.data)[i];
            assert(fabsf(fabsf(p.x-cx) - 3.5f) < .001f && p.y == 3);
            assert(fabsf(fabsf(p.z-cz) - 3.5f) < .001f);
            MP6WSEdge *edge = &entry.edges[(i-8)/2];
            float bx=edge->pos[1].x-edge->pos[0].x, bz=edge->pos[1].z-edge->pos[0].z;
            float across=((p.x-edge->pos[0].x)*-bz+(p.z-edge->pos[0].z)*bx)/(bx*bx+bz*bz);
            float sampleX=p.x-2*across*-bz, sampleZ=p.z-2*across*bx;
            assert(fabsf(uv.x-sampleX/2) < .001f && fabsf(uv.y-sampleZ/2) < .001f);
        }
        for (int i=8; i<12; ++i) {
            HSF_FACE *f = (HSF_FACE *)face.data+i;
            assert(f->typeSrc == HSF_FACE_QUAD && f->mat == 13);
            HuVecF *v=vertex.data, a=v[f->index[0].vertex], b=v[f->index[2].vertex], c=v[f->index[3].vertex];
            /* Native TRI draw order is 0,2,1, unlike the QUAD fixture. */
            HSF_FACE *source=&nativeFaces[entry.edges[i-8].face];
            HuVecF sa=ring[source->index[0].vertex], sb=ring[source->index[2].vertex], sc=ring[source->index[1].vertex];
            float originalY=(sb.z-sa.z)*(sc.x-sa.x)-(sb.x-sa.x)*(sc.z-sa.z);
            float appendedY=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z);
            assert(originalY * appendedY > 0);
            for (int j=0; j<4; ++j) assert(f->index[j].normal == 5 && f->index[j].color == 8);
        }
        scale = 1;
        mp6_widescreen_extend_floor(0, "ring", 3, cx, cz);
        assert(grows == 1);
        for (int i=8; i<16; ++i) {
            HuVecF p = ((HuVecF *)vertex.data)[i];
            assert(fabsf(fabsf(p.x-cx)-2) < .001f && fabsf(fabsf(p.z-cz)-2) < .001f);
        }
    }
    reset(); mp6_widescreen_extend_floor(0, "planar-quad", 0, 0, 0);
    assert(grows == 1 && !refuse && vertex.count == 12 && entry.edgeCount == 4);
    reset(); ((HSF_FACE *)face.data)->typeSrc = HSF_FACE_TRI;
    ((HuVecF *)vertex.data)[0].y = 1;
    mp6_widescreen_extend_floor(0, "nonplanar", 0, 0, 0);
    assert(!grows && refuse && vertex.count == 4);
    reset(); ((HSF_FACE *)face.data)->typeSrc = HSF_FACE_TRI; fail = 1;
    mp6_widescreen_extend_floor(0, "allocation-failure", 0, -.5f, -.5f);
    assert(!grows && !entry.obj && vertex.count == 4);
    test_wall_boundary_strip();
    test_tiled_sloped_wall();
    test_ring_reflect_uv();
    test_strip_rim();
    free(vertex.data); free(st.data); free(face.data);
    return 0;
}
