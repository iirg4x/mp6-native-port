/* MP6 native port -- native HSF (3D scene) deserializer.
 * See shim/include/mp6_hsf_native.h for the architecture summary and the
 * contract this module implements.
 *
 * ==========================================================================
 * FILE FORMAT DERIVATION
 * ==========================================================================
 * Every FHDR_ and F<TYPE>_ byte offset constant below is the ORIGINAL 32-bit MWCC/PPC
 * EABI layout of the matching HSF_* struct in game/hsfformat.h -- hand
 * derived under natural-alignment rules (this project's own configure.py
 * confirms `-enum int`, and there are no `#pragma pack`s or bitfields
 * anywhere in this format, so "natural alignment, C field order preserved,
 * struct size rounded up to its own max member alignment" fully determines
 * every offset) and cross-validated three independent ways:
 *   1. Two of the format's own field NAMES self-report their byte offset in
 *      hex (`HSF_MATERIAL.unk18`/`.unk20`/`.unk2C`, `HSF_ATTRIBUTE.unk8`/
 *      `.unk10`/`.unk18`/`.unk20`/`.unk24`/`.unk38`/`.unk6C`, `HSF_MESH.
 *      unk121`, `HSF_LIGHT.unk2C`) -- every single one lines up EXACTLY
 *      with this derivation once the enclosing struct's own base offset
 *      (0 for HSF_MATERIAL/HSF_ATTRIBUTE; +16 for HSF_MESH/HSF_LIGHT/
 *      HSF_CAMERA, since those unkNN names were assigned relative to the
 *      OUTER HSF_OBJECT, which has a 16-byte name/type/constData/flags
 *      header before the mesh/camera/light union begins) is added back in.
 *   2. The compiled disassembly (external_refs/repos/marioparty6/build/
 *      GP6E01/asm/game/hsfload.s) uses `mulli rX, rY, 0x144` for HSF_OBJECT
 *      array indexing and `mulli rX, rY, 0x84` for HSF_ATTRIBUTE -- both
 *      EXACTLY match this derivation's own computed sizeof (324, 132).
 *   3. Real, decoded bytes from the actual disc's data/title.bin (entries
 *      0x13/0x14/0x15 -- TITLE_HSF_PARTY/STAR/SIX, extracted+zlib-inflated
 *      per game/data.c's GetFileInfo/game/decode.c's HuDecodeZlib) were
 *      inspected field-by-field: every section's real `ofs` exactly equals
 *      the previous section's `ofs + num*sizeof(...)` per this derivation,
 *      and every field's real decoded value (a "HSFV037" magic, plausible
 *      floats/colors/flags/a real "I8title_gokou_e" bitmap name at the
 *      exact string-table offset this derivation predicts) confirms both
 *      the struct layouts AND the section/pool addressing scheme below.
 *
 * Struct-level notes worth keeping in mind while reading the code:
 *   - HSF_HEADER has 21 fixed {s32 ofs; s32 num;} section descriptors
 *     after an 8-byte magic (176 bytes total) -- no pointers, no
 *     endianness-vs-pointer-width ambiguity at all, the simplest part of
 *     the format.
 *   - Per-section arrays are NOT necessarily contiguous with each other in
 *     the file (a zero-count section's own `ofs` is meaningless leftover
 *     "current write cursor" and must never be dereferenced) -- but each
 *     section IS internally self-describing: every offset your own code
 *     needs (a buffer's data pool offset, a face group's per-entry offset,
 *     a symbol-pool index) is stored explicitly in the file, so this
 *     loader never needs to infer padding/alignment itself, only follow
 *     the offsets exactly as game/hsfload.c's own pointer arithmetic does.
 *   - The "symbol pool" (head.symbol section) is a flat array of raw u32
 *     indices -- HSF_MESH.child/.shape/.cluster fields (declared as
 *     pointer types, but really just small integers on disk) are each an
 *     INDEX into this pool, scaled by 4, not a byte offset; the pool
 *     itself then holds `childNum`/`shapeNum`/`clusterNum` further raw
 *     indices (into the object/vertex/cluster arrays respectively) at that
 *     position. See SymAt()/its 3 call sites.
 *   - HSF_OBJECT's own `mesh`/`camera`/`light` union is sized to fit the
 *     largest variant (HSF_MESH, 308 bytes) regardless of an object's
 *     actual `type` -- every object in the file occupies a full 324-byte
 *     slot (16-byte header + 308-byte union), confirmed by the disassembly
 *     stride above.
 *   - HSF_MESH.normal->data's ELEMENT format (packed HSF_S8VEC, 3 signed
 *     bytes/normal, vs. full HuVecF, 12-byte float triples) is decided by
 *     whether the WHOLE MODEL has any cenv (skinning) data at all, per
 *     game/hsfdraw.c's own vertex-format selection (confirmed by direct
 *     source review) -- NOT a fixed format. The file itself stores normals
 *     in whichever encoding matches that choice, so this deserializer reads
 *     each model's normals at the matching stride (float for a cenv model,
 *     packed bytes otherwise) rather than assuming one fixed layout -- see
 *     LoadNormalArrays().
 * ==========================================================================
 *
 * Every HSF section is deserialized for real; none are stub-tolerated:
 *   - `motion` (object/material/attribute keyframe animation -- camera
 *     moves, storybook page-turns, model motion in the opening) is built by
 *     LoadMotion() below; see that function's own header comment for the
 *     full struct-layout writeup and the automatic-activation mechanism
 *     (Hu3DModelCreate -> Hu3DMotionModelCreate -> Hu3DMotionExec ->
 *     mesh.curr) it plugs into.
 *   - cenv/cluster/shape/skeleton/part/matrix (skeletal-envelope skinning
 *     and vertex-morph deformation) are built by LoadCenv()/LoadClusters()/
 *     LoadShapes()/LoadSkeleton()/LoadParts()/LoadMatrix() below.
 *   - `mapAttr` is built by LoadMapAttr(). Worth recording since it
 *     contradicts a natural guess: mapAttr is NOT a rendering/material
 *     construct (game/hsfdraw.c never reads it) -- it's the game BOARD's
 *     own wall/floor collision grid, read only by game/mapspace.c. None of
 *     the models this port currently loads (opening cutscene + title
 *     screen) are board models, so mapAttrNum is 0 for all of them in
 *     practice; this is still real, faithful code for whichever model
 *     eventually loads an actual board HSF.
 * ==========================================================================
 */
#include "game/hsfformat.h"
#include "game/memory.h"
#include "game/EnvelopeExec.h"
#include "be.h"
#include "mp6_shim_log.h"
#include "mp6_hsf_native.h"
#include "mp6_gxarray_registry.h"
#include "mp6_boot.h" /* mp6_heap_block_data_size -- LoadBitmaps' copy bound */

#include <string.h>
#include <stdio.h>

/* ==========================================================================
 * Section header (HSF_HEADER, file-original layout: magic[8] + 21x
 * {s32 ofs; s32 num;} = 176 bytes total). Order matches HSF_HEADER's own
 * field declaration order in game/hsfformat.h exactly.
 * ========================================================================== */
enum {
    FSEC_SCENE = 0, FSEC_COLOR, FSEC_MATERIAL, FSEC_ATTRIBUTE, FSEC_VERTEX,
    FSEC_NORMAL, FSEC_ST, FSEC_FACE, FSEC_OBJECT, FSEC_BITMAP, FSEC_PALETTE,
    FSEC_MOTION, FSEC_CENV, FSEC_SKELETON, FSEC_PART, FSEC_CLUSTER,
    FSEC_SHAPE, FSEC_MAPATTR, FSEC_MATRIX, FSEC_SYMBOL, FSEC_STRING,
    FSEC_COUNT
};

typedef struct { u32 ofs; s32 num; } FSection;
typedef struct { u8 magic[8]; FSection sec[FSEC_COUNT]; } FHeader;

#define FHDR_SIZE 0xB0

/* HSF_SCENE (file) -- 16 bytes: GXFogType(4) + fogStart(4) + fogEnd(4) + GXColor(4) */
#define FSCENE_FOGTYPE   0
#define FSCENE_FOGSTART  4
#define FSCENE_FOGEND    8
#define FSCENE_FOGCOLOR  12
#define FSCENE_SIZE      16

/* HSF_MATERIAL (file) -- 60 (0x3C) bytes */
#define FMAT_NAME         0
#define FMAT_PASS         8
#define FMAT_VTXMODE      10
#define FMAT_LITCOLOR     11
#define FMAT_COLOR        14
#define FMAT_SHADOWCOLOR  17
#define FMAT_HILITESCALE  20
#define FMAT_UNK18        24
#define FMAT_INVALPHA     28
#define FMAT_UNK20        32
#define FMAT_REFALPHA     40
#define FMAT_UNK2C        44
#define FMAT_FLAGS        48
#define FMAT_ATTRNUM      52
#define FMAT_ATTR         56
#define FMAT_SIZE         60

/* HSF_ATTRIBUTE (file) -- 132 (0x84) bytes */
#define FATTR_NAME        0
#define FATTR_ANIMWORKP   4
#define FATTR_UNK8        8
#define FATTR_KCOLOR      12
#define FATTR_UNK10       16
#define FATTR_NBTTPLVL    20
#define FATTR_UNK18       24
#define FATTR_UNK20       32
#define FATTR_UNK24       36
#define FATTR_SCALE       40
#define FATTR_TRANS       48
#define FATTR_UNK38       56
#define FATTR_WRAPS       100
#define FATTR_WRAPT       104
#define FATTR_UNK6C       108
#define FATTR_MAXLOD      120
#define FATTR_FLAG        124
#define FATTR_BITMAP      128
#define FATTR_SIZE        132

/* HSF_BUFFER (file) -- 12 bytes: name(4) count(4) data(4). Used for
 * vertex/normal/st/color/face section headers alike. */
#define FBUF_NAME  0
#define FBUF_COUNT 4
#define FBUF_DATA  8
#define FBUF_SIZE  12

/* HSF_FACE_INDEX (file) -- 8 bytes: vertex/normal/color/st, all s16 */
#define FFACEIDX_SIZE 8

/* HSF_FACE (file) -- 48 (0x30) bytes */
#define FFACE_TYPE        0
#define FFACE_MAT         2
#define FFACE_STRIP_INDEX 4    /* 3x HSF_FACE_INDEX = 24 bytes, [4,28) */
#define FFACE_STRIP_COUNT 28
#define FFACE_STRIP_DATA  32
#define FFACE_INDEX4      4    /* alt view: 4x HSF_FACE_INDEX = 32 bytes, [4,36) */
#define FFACE_NBT         36
#define FFACE_SIZE        48

/* HSF_BITMAP (file) -- 32 (0x20) bytes */
#define FBMP_NAME     0
#define FBMP_MAXLOD   4
#define FBMP_DATAFMT  8
#define FBMP_PIXSIZE  9
#define FBMP_SIZEX    10
#define FBMP_SIZEY    12
#define FBMP_PALSIZE  14
#define FBMP_TINT     16
#define FBMP_PALDATA  20
#define FBMP_UNK      24
#define FBMP_DATA     28
#define FBMP_SIZE     32

/* HSF_PALETTE (file) -- 16 bytes */
#define FPAL_NAME     0
#define FPAL_UNK      4
#define FPAL_PALSIZE  8
#define FPAL_DATA     12
#define FPAL_SIZE     16

/* HSF_OBJECT (file) -- 324 (0x144) bytes: 16-byte header + 308-byte mesh/
 * camera/light union. Confirmed against the real compiled disassembly's
 * own `mulli rX, rY, 0x144` object-array-indexing immediate. */
#define FOBJ_NAME       0
#define FOBJ_TYPE       4
#define FOBJ_CONSTDATA  8
#define FOBJ_FLAGS      12
#define FOBJ_MESH_BASE  16
#define FOBJ_SIZE       324

/* HSF_MESH (file), offsets relative to FOBJ_MESH_BASE -- 308 (0x134) bytes */
#define FMESH_PARENT            0
#define FMESH_CHILDNUM          4
#define FMESH_CHILD             8
#define FMESH_BASE_XFORM        12   /* HSF_TRANSFORM: pos(12) rot(12) scale(12) = 36 bytes */
#define FMESH_CURR_XFORM        48
#define FMESH_MESH_MIN          84   /* union: {min,max,baseMorph,morphWeight[33]} vs replica */
#define FMESH_MESH_MAX          96
#define FMESH_MESH_BASEMORPH    108
#define FMESH_MESH_MORPHWEIGHT  112  /* 33 floats = 132 bytes, [112,244) */
#define FMESH_REPLICA           84   /* same union slot as MESH_MIN */
#define FMESH_FACE              244
#define FMESH_VERTEX            248
#define FMESH_NORMAL            252
#define FMESH_COLOR             256
#define FMESH_ST                260
#define FMESH_MATERIAL          264
#define FMESH_ATTRIBUTE         268
#define FMESH_WRITENUM          272
#define FMESH_UNK121            273
#define FMESH_SHAPETYPE         274
#define FMESH_MATPASS           275
#define FMESH_SHAPENUM          276
#define FMESH_SHAPE             280
#define FMESH_CLUSTERNUM        284
#define FMESH_CLUSTER           288
#define FMESH_CENVNUM           292
#define FMESH_CENV              296
#define FMESH_VTXTOP             300
#define FMESH_NORMTOP            304
#define FMESH_SIZE               308

/* HSF_CAMERA (file), relative to FOBJ_MESH_BASE -- 40 bytes */
#define FCAM_POS     0
#define FCAM_TARGET  12
#define FCAM_UPROT   24
#define FCAM_FOV     28
#define FCAM_NEAR    32
#define FCAM_FAR     36

/* HSF_LIGHT (file), relative to FOBJ_MESH_BASE -- 44 bytes */
#define FLIGHT_POS        0
#define FLIGHT_TARGET     12
#define FLIGHT_TYPE       24
#define FLIGHT_R          25
#define FLIGHT_G          26
#define FLIGHT_B          27
#define FLIGHT_UNK2C      28
#define FLIGHT_REFDIST    32
#define FLIGHT_REFBRIGHT  36
#define FLIGHT_CUTOFF     40

/* HSF_MOTION (file) -- 16 bytes: name(4) numTracks(4) track(4, UNUSED --
 * see LoadMotion()'s own header comment) maxTime(4). Only fileMotion[0] is
 * ever processed, regardless of head.motion.num -- matches game/hsfload.c's
 * own MotionLoad() exactly (every model in this game has at most one
 * embedded motion; any further entries would be exactly as dead on real
 * hardware as they are here). */
#define FMOT_NAME        0
#define FMOT_NUMTRACKS   4
#define FMOT_MAXTIME     12
#define FMOT_SIZE        16

/* HSF_TRACK (file) -- 16 bytes. Several fields are file-format UNIONS whose
 * real meaning depends on the track's own `type` (mirrors HSF_TRACK's own C
 * union declaration, game/hsfformat.h) -- see LoadMotion()'s per-type
 * switch for exactly which interpretation applies when. */
#define FTRACK_TYPE       0   /* u8 */
#define FTRACK_START      1   /* u8 */
#define FTRACK_TARGET     2   /* u16 (aka s16 "cluster") -- 2 bytes */
#define FTRACK_UNION2     4   /* 4 bytes: s32 "clusterWeight", OR s16 "attrIdx"(here)+u16 "channel"(+2) */
#define FTRACK_CURVETYPE  8   /* u16 */
#define FTRACK_NUMKEY     10  /* u16 (aka s16 "dataNum") */
#define FTRACK_DATA       12  /* 4 bytes: pool byte-offset (STEP/LINEAR/BEZIER/BITMAP), or the literal
                                * float value itself (CONST -- see ResolveTrackCurve's own comment) */
#define FTRACK_SIZE       16

/* Curve keyframe element strides -- file bytes only (native strides differ
 * for HSF_BITMAP_KEY, which carries a pointer; see ResolveTrackCurve). */
#define FCURVEKEY_STEP_SIZE   8   /* HSF_CONSTANT_KEY: float[2] {time,value} */
#define FCURVEKEY_LINEAR_SIZE 8   /* HSF_LINEAR_KEY: float[2] {time,value} */
#define FCURVEKEY_BEZIER_SIZE 16  /* HSF_BEZIER_KEY: float[4] {time,value,outTangent,inTangent} */
#define FCURVEKEY_BITMAP_SIZE 8   /* file: float time(4) + s32 bitmap-index(4) */

/* ==========================================================================
 * Deform/skinning section file layouts. All derived the same way as every
 * other section (game/hsfload.c's own in-place loaders + cross-checked
 * field-by-field), and all cross-validated by hand against the struct field
 * order in game/hsfformat.h under the same natural-alignment rules this
 * file's top-of-file note already established.
 * ========================================================================== */

/* HSF_PART (file) -- 12 bytes. name(4, string ofs) num(4) vertex(4, a u16-
 * ELEMENT index into the shared part-vertex pool that sits right after all
 * the part headers). Matches game/hsfload.c's PartLoad() exactly. */
#define FPART_NAME    0
#define FPART_NUM     4
#define FPART_VERTEX  8
#define FPART_SIZE    12

/* HSF_CLUSTER (file) -- 160 (0xA0) bytes. Confirmed against game/hsfload.c's
 * ClusterLoad() field-by-field. name[0]/name[1]/targetName are string ofs;
 * part is an index (SearchPartPtr); vertex is a SYMBOL-POOL index (like
 * HSF_MESH.child/.shape) whose pool entries are vertex-buffer indices. */
#define FCLU_NAME0       0
#define FCLU_NAME1       4
#define FCLU_TARGETNAME  8   /* string ofs on disk; ClusterAdjustObject() overwrites it in place
                              * with the resolved object index at model-create time -- see LoadCluster */
#define FCLU_PART        12
#define FCLU_INDEX       16  /* float, runtime-updated by cluster motion -- 0 on a fresh file */
#define FCLU_WEIGHT      20  /* float[32] = 128 bytes, [20,148); runtime-updated */
#define FCLU_ADJUSTED    148 /* u8, MUST start 0 (ClusterAdjustObject's own "already resolved" flag) */
#define FCLU_UNK95       149
#define FCLU_TYPE        150 /* u16 */
#define FCLU_VERTEXNUM   152 /* u32 */
#define FCLU_VERTEX      156 /* symbol-pool index */
#define FCLU_SIZE        160

/* HSF_SHAPE (file) -- 12 bytes. name(4) num16[2](2x u16 -> vertexNum in
 * num16[1]) vertex(4, symbol-pool index). Matches ShapeLoad(). */
#define FSHP_NAME    0
#define FSHP_NUM16   4   /* 2x u16 */
#define FSHP_VERTEX  8
#define FSHP_SIZE    12

/* HSF_MAPATTR (file) -- 24 bytes. NOT a rendering/material construct --
 * confirmed by direct grep across every game/*.c file: only game/mapspace.c
 * ever reads hsf->mapAttr (MapWall/MapWallCheck/MapPos, the game BOARD's
 * own wall/floor collision system: "does this X/Z position fall inside this
 * mapAttr's bounding box, and if so, walk its packed polygon list"),
 * game/hsfdraw.c never references it at all. Matches game/hsfload.c's
 * MapAttrLoad() exactly: minX/minZ/maxX/maxZ (float bounding box) + a
 * `data` field that -- like HSF_PART.vertex -- is a plain u16-ELEMENT index
 * (no symbol-pool indirection: MapAttrLoad's own `&data[(u32)mapAttrFile->
 * data]` casts the in-place field straight to an index) into a shared u16
 * pool sitting right after all the mapAttr headers; `dataLen` is a real,
 * consumed field (MapWallCheck's own `for(...; var_r30 < arg2->dataLen;)`
 * loop bound over the packed polygon list), unlike HSF_MATRIX's placeholder
 * palette bytes -- both are read here, not left as native-only bookkeeping. */
#define FMAPATTR_MINX     0
#define FMAPATTR_MINZ     4
#define FMAPATTR_MAXX     8
#define FMAPATTR_MAXZ     12
#define FMAPATTR_DATA     16  /* u16-element index into the shared pool, see this section's own comment */
#define FMAPATTR_DATALEN  20  /* u32, real/consumed -- see this section's own comment */
#define FMAPATTR_SIZE     24

/* HSF_SKELETON (file) -- 40 bytes: name(4, string ofs) + HSF_TRANSFORM(36:
 * pos(12) rot(12) scale(12)). Matches SkeletonLoad(). */
#define FSKEL_NAME       0
#define FSKEL_TRANSFORM  4
#define FSKEL_SIZE       40

/* HSF_CENV (file) -- 36 bytes. singleData/dualData/multiData are BYTE offsets
 * into the cenv data pool (which starts right after all the cenv headers);
 * counts are plain u32. name is a string ofs. Matches CenvLoad() pass 1. */
#define FCENV_NAME        0
#define FCENV_SINGLEDATA  4
#define FCENV_DUALDATA    8
#define FCENV_MULTIDATA   12
#define FCENV_SINGLECOUNT 16
#define FCENV_DUALCOUNT   20
#define FCENV_MULTICOUNT  24
#define FCENV_VTXCOUNT    28
#define FCENV_COPYCOUNT   32
#define FCENV_SIZE        36

/* HSF_CENV_SINGLE (file) -- 12 bytes: target(u32) pos(u16) posNum(u16)
 * normal(u16) normalNum(u16). No pointers -- pure in-place copy in CenvLoad. */
#define FCSINGLE_TARGET    0
#define FCSINGLE_POS       4
#define FCSINGLE_POSNUM    6
#define FCSINGLE_NORMAL    8
#define FCSINGLE_NORMALNUM 10
#define FCSINGLE_SIZE      12

/* HSF_CENV_DUAL (file) -- 16 bytes: target1(u32) target2(u32) weightNum(u32)
 * weight(4, BYTE offset into the weight pool that sits after all the single/
 * dual/multi arrays). Matches CenvLoad()'s dual resolution. */
#define FCDUAL_TARGET1   0
#define FCDUAL_TARGET2   4
#define FCDUAL_WEIGHTNUM 8
#define FCDUAL_WEIGHT    12
#define FCDUAL_SIZE      16

/* HSF_CENV_DUAL_WEIGHT (file) -- 12 bytes: weight(float) pos(u16) posNum(u16)
 * normal(u16) normalNum(u16). */
#define FCDW_WEIGHT    0
#define FCDW_POS       4
#define FCDW_POSNUM    6
#define FCDW_NORMAL    8
#define FCDW_NORMALNUM 10
#define FCDW_SIZE      12

/* HSF_CENV_MULTI (file) -- 16 bytes: weightNum(u32) pos(u16) posNum(u16)
 * normal(u16) normalNum(u16) weight(4, BYTE offset into weight pool). */
#define FCMULTI_WEIGHTNUM 0
#define FCMULTI_POS       4
#define FCMULTI_POSNUM    6
#define FCMULTI_NORMAL    8
#define FCMULTI_NORMALNUM 10
#define FCMULTI_WEIGHT    12
#define FCMULTI_SIZE      16

/* HSF_CENV_MULTI_WEIGHT (file) -- 8 bytes: target(u32) value(float). */
#define FCMW_TARGET 0
#define FCMW_VALUE  4
#define FCMW_SIZE   8

/* HSF_MATRIX (file) -- a single 12-byte header (base_idx(u32) count(u32)
 * data(4, ignored)) immediately followed by `count` many Mtx (48 bytes each,
 * float[3][4]). MatrixLoad() reads exactly one header and points .data right
 * past it; the Mtx bytes themselves are placeholder space InitEnvelope()
 * fills at runtime (SetMtx/SetRevMtx), never read from disk. */
#define FMTX_BASEIDX  0
#define FMTX_COUNT    4
#define FMTX_HDR_SIZE 12
#define FMTX_MTX_SIZE 48

/* ==========================================================================
 * Small byte-swapped read helpers
 * ========================================================================== */

static HuVecF ReadVecF(const u8 *p)
{
    HuVecF v;
    v.x = bef32(p + 0);
    v.y = bef32(p + 4);
    v.z = bef32(p + 8);
    return v;
}

static HuVec2f ReadVec2f(const u8 *p)
{
    HuVec2f v;
    v.x = bef32(p + 0);
    v.y = bef32(p + 4);
    return v;
}

static HSF_TRANSFORM ReadTransform(const u8 *p)
{
    HSF_TRANSFORM t;
    t.pos = ReadVecF(p + 0);
    t.rot = ReadVecF(p + 12);
    t.scale = ReadVecF(p + 24);
    return t;
}

static void ReadHeader(const u8 *base, FHeader *h)
{
    int i;
    memcpy(h->magic, base, 8);
    for (i = 0; i < FSEC_COUNT; i++) {
        u32 off = 8 + (u32)i * 8;
        h->sec[i].ofs = be32(base + off);
        h->sec[i].num = (s32)be32(base + off + 4);
    }
}

/* ==========================================================================
 * Load context -- carries the file base + parsed header + every
 * already-built native array, so later sections (object/attribute/bitmap)
 * can resolve cross-section index references (SearchXxxPtr equivalents)
 * against real, already-converted native pointers instead of raw file
 * offsets. Mirrors game/hsfload.c's own static globals (Model, vtxtop,
 * ClusterTop, ...) but as one explicit struct instead of file-scope
 * statics.
 * ========================================================================== */
typedef struct {
    const u8 *base;
    FHeader h;

    HSF_SCENE     *scene;      s32 sceneNum;
    HSF_PALETTE   *palette;    s32 paletteNum;
    HSF_BITMAP    *bitmap;     s32 bitmapNum;
    HSF_MATERIAL  *material;   s32 materialNum;
    HSF_ATTRIBUTE *attribute;  s32 attributeNum;
    HSF_BUFFER    *vertex;     s32 vertexNum;
    HSF_BUFFER    *normal;     s32 normalNum;
    HSF_BUFFER    *st;         s32 stNum;
    HSF_BUFFER    *color;      s32 colorNum;
    HSF_BUFFER    *face;       s32 faceNum;
    HSF_OBJECT    *object;     s32 objectNum;
    HSF_OBJECT    *root;

    /* motionNum mirrors game/hsfload.c's own MotionLoad() quirk exactly --
     * it's fileMotion[0].numTracks, NOT head.motion.num -- see
     * LoadMotion()'s own header comment. */
    HSF_MOTION    *motion;    s32 motionNum;

    /* Deform/skinning sections. */
    HSF_PART      *part;      s32 partNum;
    HSF_CLUSTER   *cluster;   s32 clusterNum;
    HSF_SHAPE     *shape;     s32 shapeNum;
    HSF_CENV      *cenv;      s32 cenvNum;
    HSF_SKELETON  *skeleton;  s32 skeletonNum;
    HSF_MATRIX    *matrix;    s32 matrixNum;

    /* The board-collision section -- see LoadMapAttr()'s own header
     * comment for why this is NOT rendering data. */
    HSF_MAPATTR   *mapAttr;   s32 mapAttrNum;

    BOOL normalIsFloat;   /* see LoadNormalArrays()'s own comment */

    /* Tag for every HEAP_MODEL sub-allocation the LoadXxx helpers below
     * make -- (u32)hsf, i.e. the SAME value game/hsfman.c's
     * Hu3DModelCreate independently computes as modelP->mallocNo right
     * after this loader returns (`modelP->mallocNo = (u32)modelP->hsf`).
     * Set ONCE in MP6_LoadHSFNative, right after allocating the outer hsf
     * struct (before any sub-allocation happens so the tag is already
     * known), and passed to HuMemDirectMallocNum -- an untagged
     * HuMemDirectMalloc sub-allocation defaults to -256 (game/memory.c's
     * HuMemMemoryAlloc), which Hu3DModelKill's own, unmodified decomp
     * bulk-free (`HuMemDirectFreeNum(HEAP_MODEL, modelP->mallocNo)`) would
     * never match, leaking it permanently. The OUTER hsf struct itself is
     * NOT tagged this way -- Hu3DModelKill frees it via a separate, plain
     * `HuMemDirectFree(modelP->hsf)` call (game/hsfman.c), matching how
     * this function's own two HSF_DATA allocations stay plain
     * HuMemDirectMalloc. */
    u32 mallocTag;
} LoadCtx;

/* Strings need no conversion at all (plain NUL-terminated bytes, no
 * endianness, no pointer-width issue) -- point straight into the original
 * file image, exactly like the original in-place loader's own GetString()
 * (`&StringTable[*strOfs]`). Safe forever: `data` is never freed by this
 * port either, matching the original's "the file buffer becomes part of
 * the live model" contract (see mp6_hsf_native.h). */
static char *GetStr(const LoadCtx *ctx, u32 strOfs)
{
    return (char *)(ctx->base + ctx->h.sec[FSEC_STRING].ofs + strOfs);
}

/* Motion-track name lookups use a 16-BIT string-table offset (the raw
 * on-disk HSF_TRACK.target/.cluster field, before resolution) -- distinct
 * from every other section's 32-bit string offset. Matches
 * game/hsfload.c's own GetMotionString(u16*)/
 * SetMotionName() exactly (game/hsfload.c's DicStringTable global, the
 * OTHER branch of MotionGetName(), is never assigned anything anywhere in
 * this codebase -- confirmed by grep -- so that branch is dead code on
 * real hardware too; not replicated here). */
static char *GetMotionStr(const LoadCtx *ctx, u16 strOfs)
{
    return (char *)(ctx->base + ctx->h.sec[FSEC_STRING].ofs + strOfs);
}

/* Symbol pool: a flat array of raw u32 (BE) indices -- see this file's own
 * top-of-file note on HSF_MESH.child/.shape/.cluster. */
static u32 SymAt(const LoadCtx *ctx, u32 idx)
{
    return be32(ctx->base + ctx->h.sec[FSEC_SYMBOL].ofs + idx * 4);
}

/* Forward-declared here because LoadObjects (which resolves each mesh's
 * cenv pointer) is defined above LoadCenv/FindCenvById. */
static HSF_CENV *FindCenvById(const LoadCtx *ctx, s32 id);

/* ==========================================================================
 * Palette (must load before Bitmap -- BitmapLoad resolves each bitmap's
 * palette by index).
 * ========================================================================== */
static void LoadPalettes(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_PALETTE].num;
    const u8 *secBase;
    HSF_PALETTE *out;
    s32 i;

    ctx->paletteNum = 0;
    ctx->palette = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_PALETTE].ofs;
    out = (HSF_PALETTE *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_PALETTE) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_PALETTE) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FPAL_SIZE;
        const u8 *poolBase = secBase + (u32)num * FPAL_SIZE;
        u32 nameOfs = be32(rec + FPAL_NAME);
        s32 palSize = (s32)be32(rec + FPAL_PALSIZE);
        u32 dataOfs = be32(rec + FPAL_DATA);
        u16 *data = NULL;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].palSize = (u32)palSize;
        if (palSize > 0) {
            /* Palette entries are GPU-side TLUT data, NOT CPU-parsed fields
             * -- the ONLY consumer is game/hsfdraw.c's LoadTexture ->
             * GXInitTlutObj -> GXLoadTlut, and Aurora ingests that buffer
             * exactly like texture pixel bytes: raw, big-endian,
             * GameCube-native (the same contract LoadBitmaps' own
             * pixel-copy comment below documents). Byte-swapping a TLUT
             * entry to host order inverts its alpha as the GPU decodes it
             * (the RGB5A3/RGB565 opacity bit lands in the wrong place), so
             * memcpy must preserve the file's big-endian entry stream
             * unchanged -- never be16()-swap this data. */
            data = (u16 *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(u16) * (u32)palSize), ctx->mallocTag);
            memcpy(data, poolBase + dataOfs, sizeof(u16) * (u32)palSize);
        }
        out[i].data = data;
    }
    ctx->palette = out;
    ctx->paletteNum = num;
}

static HSF_PALETTE *FindPaletteById(const LoadCtx *ctx, s32 id)
{
    if (id < 0 || id >= ctx->paletteNum) return NULL;
    return &ctx->palette[id];
}

/* ==========================================================================
 * Bitmap (needs Palette already loaded).
 * ========================================================================== */

/* Byte length of one bitmap's GX-tiled texel data. GX stores textures in
 * WHOLE tiles, so the correct length is ceil(w/tileW)*ceil(h/tileH)*
 * bytesPerTile -- NOT the naive w*h*bpp/8 this loader originally used,
 * which under-counts any bitmap whose width or height is not a tile
 * multiple. 27 bitmap instances on the real disc (13 unique bitmaps, e.g.
 * actman's 203x385 RGB565 "test09a", m605's 191x191 C8 "Thasira") are not
 * tile-aligned; with the naive count their last tile row was never copied,
 * and Aurora's own tile-aligned texture upload then read past the heap
 * allocation's end.
 *
 * Tile geometry per HSF dataFmt (game/hsfformat.h BMPFMT_* order; the CI_*
 * formats pick C4 vs C8 off `pixSize < 8`, mirroring game/hsfdraw.c's
 * LoadTexture switch). Mirrors tools/mp6scene/mp6_hsf.py's _GX_TILE, which
 * validated these against the whole disc (12,844 bitmaps, 0 failures). An
 * unknown format falls back to the naive count -- old behavior, never
 * larger. */
static u32 BitmapTileBytes(u8 dataFmt, u8 pixSize, u16 w, u16 h)
{
    u32 tw, th, tb;
    switch (dataFmt) {
    case 0:  tw = 8; th = 8; tb = 32; break;                 /* I4     */
    case 1:                                                  /* I8     */
    case 2:  tw = 8; th = 4; tb = 32; break;                 /* IA4    */
    case 3:                                                  /* IA8    */
    case 4:                                                  /* RGB565 */
    case 5:  tw = 4; th = 4; tb = 32; break;                 /* RGB5A3 */
    case 6:  tw = 4; th = 4; tb = 64; break;                 /* RGBA8  */
    case 7:  tw = 8; th = 8; tb = 32; break;                 /* CMPR: 4 DXT1 sub-blocks x 8B */
    case 9:                                                  /* CI_RGB565 */
    case 10:                                                 /* CI_RGB5A3 */
    case 11:                                                 /* CI_IA8    */
        if (pixSize < 8) { tw = 8; th = 8; tb = 32; }        /* -> C4 */
        else             { tw = 8; th = 4; tb = 32; }        /* -> C8 */
        break;
    default:
        return ((u32)w * (u32)h * (u32)pixSize) / 8;
    }
    return ((u32)(w + tw - 1) / tw) * ((u32)(h + th - 1) / th) * tb;
}

static void LoadBitmaps(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_BITMAP].num;
    const u8 *secBase;
    HSF_BITMAP *out;
    s32 i;

    ctx->bitmapNum = 0;
    ctx->bitmap = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_BITMAP].ofs;
    out = (HSF_BITMAP *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BITMAP) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_BITMAP) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FBMP_SIZE;
        const u8 *poolBase = secBase + (u32)num * FBMP_SIZE;
        u32 nameOfs = be32(rec + FBMP_NAME);
        s16 sizeX = (s16)be16(rec + FBMP_SIZEX);
        s16 sizeY = (s16)be16(rec + FBMP_SIZEY);
        u8 pixSize = rec[FBMP_PIXSIZE];
        s32 palId = (s32)be32(rec + FBMP_PALDATA);  /* raw slot re-purposed as a palette INDEX
                                                      * (-1 == none), matching SearchPalettePtr's
                                                      * own -1 sentinel convention. */
        u32 dataOfs = be32(rec + FBMP_DATA);
        HSF_PALETTE *pal;
        u32 pixelBytes;
        u32 naiveBytes;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].maxLod = be32(rec + FBMP_MAXLOD);
        out[i].dataFmt = rec[FBMP_DATAFMT];
        out[i].pixSize = pixSize;
        out[i].sizeX = sizeX;
        out[i].sizeY = sizeY;
        out[i].palSize = (s16)be16(rec + FBMP_PALSIZE);
        out[i].tint.r = rec[FBMP_TINT + 0];
        out[i].tint.g = rec[FBMP_TINT + 1];
        out[i].tint.b = rec[FBMP_TINT + 2];
        out[i].tint.a = rec[FBMP_TINT + 3];
        out[i].unk = be32(rec + FBMP_UNK);

        pal = FindPaletteById(ctx, palId);
        out[i].palData = pal ? pal->data : NULL;

        /* Pixel data: raw GX-native texel bytes -- Aurora consumes this
         * GameCube-native GPU data completely UNCHANGED (no byte-swap, no
         * relayout).
         *
         * Byte count = TILE-aligned (BitmapTileBytes above), not the naive
         * w*h*bpp/8 -- see that helper's comment for the 13 real bitmaps
         * the naive count under-copied.
         *
         * Copy bound: the source is an offset into the decoded FILE buffer,
         * whose length this loader is never told -- and 2 of those 13
         * bitmaps are physically TRUNCATED at EOF (the authoring tool wrote
         * the naive size: actman "test09a" is short 1,800 bytes, ishi
         * "testPic" 222). Real hardware just DMA'd whatever followed in
         * RAM; this port bounds the copy by the buffer's own allocation
         * extent (mp6_heap_block_data_size -- data.c allocates every file
         * buffer via HuMemDirectMalloc) and ZERO-pads the missing tail, the
         * same policy as tools/mp6scene/mp6_hsf.py's importer. If the bound
         * is unavailable (pointer not a verifiable block base), copy only
         * the naive byte count -- bytes guaranteed present in every real
         * file -- and zero the rest. */
        naiveBytes = ((u32)(u16)sizeX * (u32)(u16)sizeY * (u32)pixSize) / 8;
        pixelBytes = BitmapTileBytes(out[i].dataFmt, pixSize, (u16)sizeX, (u16)sizeY);
        if (pixelBytes > 0) {
            u8 *pixels = (u8 *)HuMemDirectMallocNum(HEAP_MODEL, (s32)pixelBytes, ctx->mallocTag);
            u32 avail = mp6_heap_block_data_size(ctx->base);
            u32 srcOfs = (u32)(poolBase - ctx->base) + dataOfs;
            u32 copyN;
            if (avail > 0) {
                copyN = (srcOfs >= avail) ? 0
                      : (pixelBytes <= avail - srcOfs) ? pixelBytes
                                                       : (avail - srcOfs);
            } else {
                copyN = (naiveBytes < pixelBytes) ? naiveBytes : pixelBytes;
            }
            if (copyN < pixelBytes) {
                memset(pixels + copyN, 0, pixelBytes - copyN);
            }
            if (copyN > 0) {
                memcpy(pixels, poolBase + dataOfs, copyN);
            }
            out[i].data = pixels;
        } else {
            out[i].data = NULL;
        }
    }
    ctx->bitmap = out;
    ctx->bitmapNum = num;
}

/* ==========================================================================
 * Material.
 * ========================================================================== */
static void LoadMaterials(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_MATERIAL].num;
    const u8 *secBase;
    HSF_MATERIAL *out;
    s32 i, j;

    ctx->materialNum = 0;
    ctx->material = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_MATERIAL].ofs;
    out = (HSF_MATERIAL *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_MATERIAL) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_MATERIAL) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FMAT_SIZE;
        u32 nameOfs = be32(rec + FMAT_NAME);
        u32 attrNum = be32(rec + FMAT_ATTRNUM);
        u32 attrPoolIdx = be32(rec + FMAT_ATTR);
        s32 *attrArr = NULL;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].pass = be16(rec + FMAT_PASS);
        out[i].vtxMode = rec[FMAT_VTXMODE];
        out[i].litColor[0] = rec[FMAT_LITCOLOR + 0];
        out[i].litColor[1] = rec[FMAT_LITCOLOR + 1];
        out[i].litColor[2] = rec[FMAT_LITCOLOR + 2];
        out[i].color[0] = rec[FMAT_COLOR + 0];
        out[i].color[1] = rec[FMAT_COLOR + 1];
        out[i].color[2] = rec[FMAT_COLOR + 2];
        out[i].shadowColor[0] = rec[FMAT_SHADOWCOLOR + 0];
        out[i].shadowColor[1] = rec[FMAT_SHADOWCOLOR + 1];
        out[i].shadowColor[2] = rec[FMAT_SHADOWCOLOR + 2];
        out[i].hiliteScale = bef32(rec + FMAT_HILITESCALE);
        out[i].unk18 = bef32(rec + FMAT_UNK18);
        out[i].invAlpha = bef32(rec + FMAT_INVALPHA);
        out[i].unk20[0] = bef32(rec + FMAT_UNK20 + 0);
        out[i].unk20[1] = bef32(rec + FMAT_UNK20 + 4);
        out[i].refAlpha = bef32(rec + FMAT_REFALPHA);
        out[i].unk2C = bef32(rec + FMAT_UNK2C);
        out[i].flags = be32(rec + FMAT_FLAGS);
        out[i].attrNum = attrNum;

        if (attrNum > 0) {
            attrArr = (s32 *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(s32) * attrNum), ctx->mallocTag);
            for (j = 0; j < (s32)attrNum; j++) {
                attrArr[j] = (s32)SymAt(ctx, attrPoolIdx + (u32)j);
            }
        }
        out[i].attr = attrArr;
    }
    ctx->material = out;
    ctx->materialNum = num;
}

/* ==========================================================================
 * Attribute (needs Bitmap already loaded).
 * ========================================================================== */
static void LoadAttributes(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_ATTRIBUTE].num;
    const u8 *secBase;
    HSF_ATTRIBUTE *out;
    s32 i;
    int k;

    ctx->attributeNum = 0;
    ctx->attribute = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_ATTRIBUTE].ofs;
    out = (HSF_ATTRIBUTE *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_ATTRIBUTE) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_ATTRIBUTE) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FATTR_SIZE;
        u32 nameOfs = be32(rec + FATTR_NAME);
        s32 bitmapId;
        HSF_BITMAP *bmp;

        /* AttributeLoad's own -1 sentinel check (game/hsfload.c) -- unlike
         * Material/Object names, which resolve unconditionally. */
        out[i].name = (nameOfs != 0xFFFFFFFFu) ? GetStr(ctx, nameOfs) : NULL;

        out[i].animWorkP = NULL;  /* runtime-only; always 0 on disk for a fresh model (verified
                                    * against real title HSF bytes -- see mp6_hsf_native.h). */
        for (k = 0; k < 4; k++) out[i].unk8[k] = rec[FATTR_UNK8 + k];
        out[i].kColor = bef32(rec + FATTR_KCOLOR);
        for (k = 0; k < 4; k++) out[i].unk10[k] = rec[FATTR_UNK10 + k];
        out[i].nbtTpLvl = bef32(rec + FATTR_NBTTPLVL);
        for (k = 0; k < 8; k++) out[i].unk18[k] = rec[FATTR_UNK18 + k];
        out[i].unk20 = bef32(rec + FATTR_UNK20);
        for (k = 0; k < 4; k++) out[i].unk24[k] = rec[FATTR_UNK24 + k];
        out[i].scale = ReadVec2f(rec + FATTR_SCALE);
        out[i].trans = ReadVec2f(rec + FATTR_TRANS);
        for (k = 0; k < 44; k++) out[i].unk38[k] = rec[FATTR_UNK38 + k];
        out[i].wrapS = be32(rec + FATTR_WRAPS);
        out[i].wrapT = be32(rec + FATTR_WRAPT);
        for (k = 0; k < 12; k++) out[i].unk6C[k] = rec[FATTR_UNK6C + k];
        out[i].maxLod = be32(rec + FATTR_MAXLOD);
        out[i].flag = be32(rec + FATTR_FLAG);

        bitmapId = (s32)be32(rec + FATTR_BITMAP);
        bmp = (bitmapId >= 0 && bitmapId < ctx->bitmapNum) ? &ctx->bitmap[bitmapId] : NULL;
        out[i].bitmap = bmp;
    }
    ctx->attribute = out;
    ctx->attributeNum = num;
}

/* ==========================================================================
 * Vertex arrays (HuVecF per element, real element-wise conversion --
 * matches VertexLoad's own HuCopyVecF call).
 * ========================================================================== */
static void LoadVertexArrays(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_VERTEX].num;
    const u8 *secBase;
    HSF_BUFFER *out;
    s32 i, j;

    ctx->vertexNum = 0;
    ctx->vertex = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_VERTEX].ofs;
    out = (HSF_BUFFER *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BUFFER) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_BUFFER) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FBUF_SIZE;
        const u8 *poolBase = secBase + (u32)num * FBUF_SIZE;
        u32 nameOfs = be32(rec + FBUF_NAME);
        s32 count = (s32)be32(rec + FBUF_COUNT);
        u32 dataOfs = be32(rec + FBUF_DATA);
        HuVecF *data = NULL;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].count = count;
        if (count > 0) {
            data = (HuVecF *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HuVecF) * (u32)count), ctx->mallocTag);
            for (j = 0; j < count; j++) {
                data[j] = ReadVecF(poolBase + dataOfs + (u32)j * 12);
            }
            /* Tell the GXSetArray bridge this buffer's real byte size --
             * see mp6_gxarray_registry.h. */
            mp6_gxarray_register(data, (uint32_t)(sizeof(HuVecF) * (u32)count));
        }
        out[i].data = data;
    }
    ctx->vertex = out;
    ctx->vertexNum = num;
}

/* ==========================================================================
 * Normal arrays. See this file's top-of-file note: element format (packed
 * HSF_S8VEC vs. float HuVecF) depends on whether the file has ANY cenv
 * (skinning) data. Read each model's normals at the stride that format
 * actually uses (float for a cenv model, packed bytes otherwise).
 * ========================================================================== */
static void LoadNormalArrays(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_NORMAL].num;
    const u8 *secBase;
    HSF_BUFFER *out;
    s32 i, j;
    u32 srcStride;

    ctx->normalNum = 0;
    ctx->normal = NULL;
    if (num <= 0) return;

    srcStride = ctx->normalIsFloat ? 12u : 3u;
    secBase = ctx->base + ctx->h.sec[FSEC_NORMAL].ofs;
    out = (HSF_BUFFER *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BUFFER) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_BUFFER) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FBUF_SIZE;
        const u8 *poolBase = secBase + (u32)num * FBUF_SIZE;
        u32 nameOfs = be32(rec + FBUF_NAME);
        s32 count = (s32)be32(rec + FBUF_COUNT);
        u32 dataOfs = be32(rec + FBUF_DATA);
        void *data = NULL;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].count = count;
        if (count > 0) {
            if (ctx->normalIsFloat) {
                /* A cenv (skinned) model's normals are stored as full float
                 * HuVecF on disk AND consumed as float -- game/hsfdraw.c
                 * submits GX_F32 when cenvNum != 0, and
                 * game/EnvelopeExec.c's SetEnvelop reads/writes them as
                 * Vec* (float). Keep them float here (no S8 quantization),
                 * exactly matching that consumption. */
                HuVecF *fdata = (HuVecF *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HuVecF) * (u32)count), ctx->mallocTag);
                for (j = 0; j < count; j++) {
                    fdata[j] = ReadVecF(poolBase + dataOfs + (u32)j * srcStride);
                }
                mp6_gxarray_register(fdata, (uint32_t)(sizeof(HuVecF) * (u32)count));
                data = fdata;
            } else {
                /* Non-cenv model: packed HSF_S8VEC (GX_S8), 3 signed bytes
                 * per normal -- see this file's own top-of-file note. */
                HSF_S8VEC *sdata = (HSF_S8VEC *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_S8VEC) * (u32)count), ctx->mallocTag);
                for (j = 0; j < count; j++) {
                    const u8 *src = poolBase + dataOfs + (u32)j * srcStride;
                    sdata[j].x = (s8)src[0];
                    sdata[j].y = (s8)src[1];
                    sdata[j].z = (s8)src[2];
                }
                mp6_gxarray_register(sdata, (uint32_t)(sizeof(HSF_S8VEC) * (u32)count));
                data = sdata;
            }
        }
        out[i].data = data;
    }
    ctx->normal = out;
    ctx->normalNum = num;
}

/* ==========================================================================
 * ST (texture coordinate) arrays -- HuVec2f, real conversion (matches
 * STLoad's own HuCopyVec2F call).
 * ========================================================================== */
static void LoadSTArrays(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_ST].num;
    const u8 *secBase;
    HSF_BUFFER *out;
    s32 i, j;

    ctx->stNum = 0;
    ctx->st = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_ST].ofs;
    out = (HSF_BUFFER *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BUFFER) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_BUFFER) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FBUF_SIZE;
        const u8 *poolBase = secBase + (u32)num * FBUF_SIZE;
        u32 nameOfs = be32(rec + FBUF_NAME);
        s32 count = (s32)be32(rec + FBUF_COUNT);
        u32 dataOfs = be32(rec + FBUF_DATA);
        HuVec2f *data = NULL;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].count = count;
        if (count > 0) {
            data = (HuVec2f *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HuVec2f) * (u32)count), ctx->mallocTag);
            for (j = 0; j < count; j++) {
                data[j] = ReadVec2f(poolBase + dataOfs + (u32)j * 8);
            }
            /* See LoadVertexArrays' own comment on mp6_gxarray_register. */
            mp6_gxarray_register(data, (uint32_t)(sizeof(HuVec2f) * (u32)count));
        }
        out[i].data = data;
    }
    ctx->st = out;
    ctx->stNum = num;
}

/* ==========================================================================
 * Color arrays -- raw GXColor-shaped bytes (4x u8: r,g,b,a), no swap
 * needed at all; matches ColorLoad's own (no-op passthrough) behavior.
 * ========================================================================== */
static void LoadColorArrays(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_COLOR].num;
    const u8 *secBase;
    HSF_BUFFER *out;
    s32 i;

    ctx->colorNum = 0;
    ctx->color = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_COLOR].ofs;
    out = (HSF_BUFFER *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BUFFER) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_BUFFER) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FBUF_SIZE;
        const u8 *poolBase = secBase + (u32)num * FBUF_SIZE;
        u32 nameOfs = be32(rec + FBUF_NAME);
        s32 count = (s32)be32(rec + FBUF_COUNT);
        u32 dataOfs = be32(rec + FBUF_DATA);
        u8 *data = NULL;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].count = count;
        if (count > 0) {
            data = (u8 *)HuMemDirectMallocNum(HEAP_MODEL, count * 4, ctx->mallocTag);
            memcpy(data, poolBase + dataOfs, (size_t)count * 4);
            /* See LoadVertexArrays' own comment on mp6_gxarray_register. */
            mp6_gxarray_register(data, (uint32_t)count * 4);
        }
        out[i].data = data;
    }
    ctx->color = out;
    ctx->colorNum = num;
}

/* ==========================================================================
 * Face groups. Each HSF_BUFFER-headed "group" owns `count` HSF_FACE
 * entries living in a POOL SHARED by every group in the section (computed
 * once, right after all the group headers); a TRISTRIP face's own strip
 * data lives in a further pool immediately after THAT group's own
 * `count` HSF_FACE entries -- matches game/hsfload.c's FaceLoad() exactly.
 * ========================================================================== */
static void LoadFaceGroups(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_FACE].num;
    const u8 *secBase;
    HSF_BUFFER *out;
    s32 i, j;

    ctx->faceNum = 0;
    ctx->face = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_FACE].ofs;
    out = (HSF_BUFFER *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BUFFER) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_BUFFER) * (u32)num);

    /* The TRISTRIP pool is GLOBAL to the whole face SECTION, not per-group.
     * game/hsfload.c's FaceLoad() computes
     * `strip = &((HSF_FACE *)newFace->data)[newFace->count]` INSIDE its
     * first per-group loop and never resets it, so the value its second
     * loop (the one that resolves every strip.data) actually uses is the
     * LAST iteration's: end of the LAST group's face records. Computing
     * this per-group instead (groupBase + count*FFACE_SIZE) only coincides
     * with the original for the final group -- every other group's
     * TRISTRIP strip indices would resolve into the NEXT group's face
     * records instead of the shared strip pool. */
    {
        const u8 *poolBaseAll = secBase + (u32)num * FBUF_SIZE;
        const u8 *lastRec = secBase + (u32)(num - 1) * FBUF_SIZE;
        u32 lastDataOfs = be32(lastRec + FBUF_DATA);
        s32 lastCount = (s32)be32(lastRec + FBUF_COUNT);
        const u8 *stripPoolBase = poolBaseAll + lastDataOfs + (u32)lastCount * FFACE_SIZE;

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FBUF_SIZE;
        const u8 *poolBase = poolBaseAll;
        u32 nameOfs = be32(rec + FBUF_NAME);
        s32 count = (s32)be32(rec + FBUF_COUNT);
        u32 dataOfs = be32(rec + FBUF_DATA);
        const u8 *groupBase = poolBase + dataOfs;
        HSF_FACE *data = NULL;

        out[i].name = GetStr(ctx, nameOfs);
        out[i].count = count;

        if (count > 0) {
            data = (HSF_FACE *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_FACE) * (u32)count), ctx->mallocTag);
            memset(data, 0, sizeof(HSF_FACE) * (u32)count);

            for (j = 0; j < count; j++) {
                const u8 *frec = groupBase + (u32)j * FFACE_SIZE;
                u16 typeSrc = be16(frec + FFACE_TYPE);
                int k;

                data[j].typeSrc = typeSrc;
                data[j].mat = (s16)be16(frec + FFACE_MAT);
                for (k = 0; k < 3; k++) {
                    data[j].nbt[k] = bef32(frec + FFACE_NBT + (u32)k * 4);
                }

                if ((typeSrc & HSF_FACE_MASK) == HSF_FACE_TRISTRIP) {
                    s32 stripCount = (s32)be32(frec + FFACE_STRIP_COUNT);
                    u32 stripRawIdx = be32(frec + FFACE_STRIP_DATA);
                    HSF_FACE_INDEX *stripData = NULL;
                    int m;

                    for (m = 0; m < 3; m++) {
                        const u8 *idxp = frec + FFACE_STRIP_INDEX + (u32)m * FFACEIDX_SIZE;
                        data[j].strip.index[m].vertex = (s16)be16(idxp + 0);
                        data[j].strip.index[m].normal = (s16)be16(idxp + 2);
                        data[j].strip.index[m].color  = (s16)be16(idxp + 4);
                        data[j].strip.index[m].st     = (s16)be16(idxp + 6);
                    }
                    data[j].strip.count = (u32)stripCount;
                    if (stripCount > 0) {
                        const u8 *stripSrc = stripPoolBase + stripRawIdx * FFACEIDX_SIZE;
                        stripData = (HSF_FACE_INDEX *)HuMemDirectMallocNum(
                            HEAP_MODEL, (s32)(sizeof(HSF_FACE_INDEX) * (u32)stripCount), ctx->mallocTag);
                        for (m = 0; m < stripCount; m++) {
                            const u8 *idxp = stripSrc + (u32)m * FFACEIDX_SIZE;
                            stripData[m].vertex = (s16)be16(idxp + 0);
                            stripData[m].normal = (s16)be16(idxp + 2);
                            stripData[m].color  = (s16)be16(idxp + 4);
                            stripData[m].st     = (s16)be16(idxp + 6);
                        }
                    }
                    data[j].strip.data = stripData;
                } else {
                    int m;
                    for (m = 0; m < 4; m++) {
                        const u8 *idxp = frec + FFACE_INDEX4 + (u32)m * FFACEIDX_SIZE;
                        data[j].index[m].vertex = (s16)be16(idxp + 0);
                        data[j].index[m].normal = (s16)be16(idxp + 2);
                        data[j].index[m].color  = (s16)be16(idxp + 4);
                        data[j].index[m].st     = (s16)be16(idxp + 6);
                    }
                }
            }
        }
        out[i].data = data;
        /* Parse-time face-index validation: logs the same out-of-range
         * vertex-index sweep the DL-build-time check runs (hsfdraw.c.patch),
         * but against the just-decoded source bytes here -- a bad-here/
         * good-there diff separates "source blob or parse offsets wrong"
         * from "heap stomp between parse and DL build". Env-gated by
         * MP6_DL_DUMP (see docs/DEBUGGING.md); no output otherwise. */
        if (getenv("MP6_DL_DUMP") != NULL && count > 0 && data != NULL) {
            s32 badV = 0, vRef = -1;
            s32 fj;
            int kk;
            for (fj = 0; fj < count; fj++) {
                HSF_FACE *f = &data[fj];
                s32 nIdx = ((f->typeSrc & 0x7) == 3) ? 4 : ((f->typeSrc & 0x7) == 2) ? 3 : 0;
                for (kk = 0; kk < nIdx; kk++) {
                    if (f->index[kk].vertex > vRef) vRef = f->index[kk].vertex;
                    if (f->index[kk].vertex < 0) badV++;
                }
                if ((f->typeSrc & 0x7) == 4) {
                    u32 m2;
                    for (m2 = 0; f->strip.data && m2 < f->strip.count; m2++) {
                        if (f->strip.data[m2].vertex > vRef) vRef = f->strip.data[m2].vertex;
                        if (f->strip.data[m2].vertex < 0) badV++;
                    }
                }
            }
            printf("[FACE-PARSE] group=%s count=%d maxVtxIdx=%d negV=%d\n",
                   out[i].name ? out[i].name : "(null)", count, (int)vRef, (int)badV);
            fflush(stdout);
        }
    }
    } /* close the section-global stripPoolBase scope */
    ctx->face = out;
    ctx->faceNum = num;
}

/* ==========================================================================
 * Objects (the scene-graph hierarchy). Two-pass: (1) every object's own
 * scalar fields + type-specific sub-struct + its own child-index list,
 * unconditionally over the WHOLE flat array; (2) parent pointers, via a
 * flat "each container object stamps its own children" pass rather than
 * true top-down recursion (equivalent for a tree, avoids native recursion
 * depth). See this function's own inline comments for why pass 1 is
 * deliberately unconditional (safer than the original's reachability-gated
 * behavior, never less correct).
 * ========================================================================== */
static void LoadObjects(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_OBJECT].num;
    const u8 *secBase;
    HSF_OBJECT *out;
    s32 i, j;
    s32 rootIdx = -1;

    ctx->objectNum = 0;
    ctx->object = NULL;
    ctx->root = NULL;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_OBJECT].ofs;
    out = (HSF_OBJECT *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_OBJECT) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_OBJECT) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FOBJ_SIZE;
        const u8 *mrec = rec + FOBJ_MESH_BASE;
        u32 nameOfs = be32(rec + FOBJ_NAME);
        u32 type = be32(rec + FOBJ_TYPE);
        s32 rawParent = (s32)be32(mrec + FMESH_PARENT);

        out[i].name = GetStr(ctx, nameOfs);
        out[i].type = type;
        out[i].constData = NULL;  /* populated later, by MakeDisplayList -- see mp6_hsf_native.h */
        out[i].flags = be32(rec + FOBJ_FLAGS);

        if (rawParent == -1 && rootIdx == -1) {
            /* First (only, in every well-formed file) parent==-1 sentinel --
             * matches the original's own linear scan in ObjectLoad(). */
            rootIdx = i;
        }

        if (type == HSF_OBJ_CAMERA) {
            out[i].camera.pos = ReadVecF(mrec + FCAM_POS);
            out[i].camera.target = ReadVecF(mrec + FCAM_TARGET);
            out[i].camera.upRot = bef32(mrec + FCAM_UPROT);
            out[i].camera.fov = bef32(mrec + FCAM_FOV);
            out[i].camera.near = bef32(mrec + FCAM_NEAR);
            out[i].camera.far = bef32(mrec + FCAM_FAR);
            continue;
        }
        if (type == HSF_OBJ_LIGHT) {
            out[i].light.pos = ReadVecF(mrec + FLIGHT_POS);
            out[i].light.target = ReadVecF(mrec + FLIGHT_TARGET);
            out[i].light.type = mrec[FLIGHT_TYPE];
            out[i].light.r = mrec[FLIGHT_R];
            out[i].light.g = mrec[FLIGHT_G];
            out[i].light.b = mrec[FLIGHT_B];
            out[i].light.unk2C = bef32(mrec + FLIGHT_UNK2C);
            out[i].light.refDistance = bef32(mrec + FLIGHT_REFDIST);
            out[i].light.refBrightness = bef32(mrec + FLIGHT_REFBRIGHT);
            out[i].light.cutoff = bef32(mrec + FLIGHT_CUTOFF);
            continue;
        }

        /* MESH, NULL1, REPLICA, ROOT, JOINT, NULL2, NULL3, MAP -- all share
         * HSF_MESH's layout. Every one of them has parent/childNum/child +
         * base/curr transforms; only MESH populates min/max/morph and the
         * vertex/normal/st/color/face/material/attribute links, and only
         * REPLICA populates the union's alternate `replica` slot. */
        {
            u32 childNum = be32(mrec + FMESH_CHILDNUM);
            u32 childPoolIdx = be32(mrec + FMESH_CHILD);
            HSF_OBJECT **childArr = NULL;

            out[i].mesh.childNum = childNum;
            out[i].mesh.base = ReadTransform(mrec + FMESH_BASE_XFORM);
            /* curr: only dereferenced when a motion is actually attached to
             * this model instance (attachMotionF in game/hsfdraw.c) -- never
             * true for motionNum==0 (confirmed by direct source review).
             * Seeded from base as a defensively-sane value regardless. */
            out[i].mesh.curr = out[i].mesh.base;

            if (childNum > 0) {
                childArr = (HSF_OBJECT **)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_OBJECT *) * childNum), ctx->mallocTag);
                for (j = 0; j < (s32)childNum; j++) {
                    u32 childObjIdx = SymAt(ctx, childPoolIdx + (u32)j);
                    childArr[j] = (childObjIdx < (u32)num) ? &out[childObjIdx] : NULL;
                }
            }
            out[i].mesh.child = childArr;

            if (type == HSF_OBJ_REPLICA) {
                u32 replicaIdx = be32(mrec + FMESH_REPLICA);
                out[i].mesh.replica = (replicaIdx < (u32)num) ? &out[replicaIdx] : NULL;
            } else if (type == HSF_OBJ_MESH) {
                s32 vertexId, normalId, stId, colorId, faceId, attrFlag;

                out[i].mesh.mesh.min = ReadVecF(mrec + FMESH_MESH_MIN);
                out[i].mesh.mesh.max = ReadVecF(mrec + FMESH_MESH_MAX);
                out[i].mesh.mesh.baseMorph = bef32(mrec + FMESH_MESH_BASEMORPH);
                for (j = 0; j < 33; j++) {
                    out[i].mesh.mesh.morphWeight[j] = bef32(mrec + FMESH_MESH_MORPHWEIGHT + (u32)j * 4);
                }

                vertexId = (s32)be32(mrec + FMESH_VERTEX);
                normalId = (s32)be32(mrec + FMESH_NORMAL);
                stId = (s32)be32(mrec + FMESH_ST);
                colorId = (s32)be32(mrec + FMESH_COLOR);
                faceId = (s32)be32(mrec + FMESH_FACE);
                attrFlag = (s32)be32(mrec + FMESH_ATTRIBUTE);

                out[i].mesh.vertex = (vertexId >= 0 && vertexId < ctx->vertexNum) ? &ctx->vertex[vertexId] : NULL;
                out[i].mesh.normal = (normalId >= 0 && normalId < ctx->normalNum) ? &ctx->normal[normalId] : NULL;
                out[i].mesh.st     = (stId >= 0 && stId < ctx->stNum) ? &ctx->st[stId] : NULL;
                out[i].mesh.color  = (colorId >= 0 && colorId < ctx->colorNum) ? &ctx->color[colorId] : NULL;
                out[i].mesh.face   = (faceId >= 0 && faceId < ctx->faceNum) ? &ctx->face[faceId] : NULL;
                /* Whole-array pointers, not per-object indices -- matches the
                 * original's own unconditional `newObj->mesh.material =
                 * Model.material;` (per-face indexing happens via
                 * HSF_FACE.mat instead) and `(s32)data->attribute >= 0` sign
                 * check for whether this mesh uses attributes/textures at
                 * all. */
                out[i].mesh.material = ctx->material;
                out[i].mesh.attribute = (attrFlag >= 0) ? ctx->attribute : NULL;

                out[i].mesh.writeNum = mrec[FMESH_WRITENUM];
                out[i].mesh.unk121 = mrec[FMESH_UNK121];
                out[i].mesh.shapeType = mrec[FMESH_SHAPETYPE];
                out[i].mesh.matPass = mrec[FMESH_MATPASS];

                /* shape/cluster per-mesh fields stay 0/NULL: the cluster
                 * deform path (game/ClusterExec.c) works off the hsf-level
                 * cluster array + cluster->target, NOT obj->mesh.cluster
                 * (confirmed by direct source review), and no model in this
                 * content has shape>0. */
                out[i].mesh.shapeNum = 0;
                out[i].mesh.shape = NULL;
                out[i].mesh.clusterNum = 0;
                out[i].mesh.cluster = NULL;

                /* Per-mesh cenv (skinning). Only meaningful when the MODEL
                 * has any cenv at all (ctx->cenvNum > 0); a mesh in a model
                 * with no cenv data has no envelope binding and vtxtop/
                 * normtop stay NULL (see the else branch below).
                 * game/EnvelopeExec.c's SetEnvelop reads obj->mesh.vtxtop/
                 * normtop as the pristine BIND-POSE source and writes the
                 * skinned result into obj->mesh.vertex/normal->data (the live,
                 * drawn buffers) -- so vtxtop/normtop must be SEPARATE copies
                 * of the bind pose (== the freshly-loaded vertex/normal data,
                 * before any frame deforms the live buffers). Allocated once,
                 * here, HEAP_MODEL-owned. */
                if (ctx->cenvNum > 0) {
                    u32 meshCenvNum = be32(mrec + FMESH_CENVNUM);
                    s32 meshCenvIdx = (s32)be32(mrec + FMESH_CENV);
                    out[i].mesh.cenvNum = meshCenvNum;
                    out[i].mesh.cenv = FindCenvById(ctx, meshCenvIdx);
                    if (meshCenvNum > 0 && out[i].mesh.vertex && out[i].mesh.vertex->data &&
                        out[i].mesh.normal && out[i].mesh.normal->data) {
                        s32 vcount = out[i].mesh.vertex->count;
                        s32 ncount = out[i].mesh.normal->count;
                        HuVecF *vt = (HuVecF *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HuVecF) * (u32)vcount), ctx->mallocTag);
                        HuVecF *nt = (HuVecF *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HuVecF) * (u32)ncount), ctx->mallocTag);
                        /* normal->data is float HuVecF here because
                         * ctx->normalIsFloat is set whenever the model has
                         * cenv -- see LoadNormalArrays. */
                        memcpy(vt, out[i].mesh.vertex->data, sizeof(HuVecF) * (u32)vcount);
                        memcpy(nt, out[i].mesh.normal->data, sizeof(HuVecF) * (u32)ncount);
                        out[i].mesh.vtxtop = vt;
                        out[i].mesh.normtop = nt;
                    } else {
                        out[i].mesh.vtxtop = NULL;
                        out[i].mesh.normtop = NULL;
                    }
                } else {
                    out[i].mesh.cenvNum = 0;
                    out[i].mesh.cenv = NULL;
                    out[i].mesh.vtxtop = NULL;
                    out[i].mesh.normtop = NULL;
                }
            }
            /* NULL1/ROOT/JOINT/NULL2/NULL3/MAP: nothing further to do --
             * parent/childNum/child/base/curr (already set above) are
             * everything game/hsfdraw.c's objNull/objRoot/objJoint/objMap
             * read for these types. */
        }
    }

    /* Pass 2: parent pointers (see this function's own header comment). */
    for (i = 0; i < num; i++) {
        u32 type = out[i].type;
        if (type == HSF_OBJ_CAMERA || type == HSF_OBJ_LIGHT) continue;
        for (j = 0; j < (s32)out[i].mesh.childNum; j++) {
            HSF_OBJECT *child = out[i].mesh.child ? out[i].mesh.child[j] : NULL;
            if (child) child->mesh.parent = &out[i];
        }
    }

    if (rootIdx < 0) {
        MP6_LOG_ONCE("HSF", "LoadObjects-no-root-found");
        rootIdx = 0;  /* defensive fallback for a malformed file; see mp6_hsf_native.h */
    }

    ctx->object = out;
    ctx->objectNum = num;
    ctx->root = &out[rootIdx];
}

/* ==========================================================================
 * CLUSTER (vertex-morph deformation -- the opening storybook's page-curl /
 * book-opening) + its supporting PART section, plus SHAPE (a sibling
 * morph-target mechanism, loaded for completeness).
 *
 * Consumed, unmodified, by game/ClusterExec.c (ClusterMotionExec ->
 * SetClusterMain) and game/hsfman.c/hsfmotion.c (Hu3DMotionClusterSet, which
 * game/hsfman.c's Hu3DModelCreate calls automatically the instant
 * hsf->clusterNum != 0). The deform itself writes IN PLACE into the
 * model's own already-loaded vertex buffer (obj->mesh.vertex->data) every
 * frame; Aurora re-reads that same pointer's current bytes at end-of-frame
 * (see platform/gx/aurora_bridge.c's mp6_GXSetArray3), so an in-place morph
 * is picked up on screen with zero per-frame allocation -- SetClusterMain
 * allocates nothing, and this loader (like every other section here)
 * allocates once, at load, into HEAP_MODEL, owned+freed by the game's own
 * Hu3DModelKill.
 * ========================================================================== */

/* game/hsfload.c's PartLoad(): each part's `vertex` is a u16-ELEMENT index
 * into the shared part-vertex pool (right after all the part headers); each
 * part then owns `num` consecutive u16 vertex indices there. */
static void LoadParts(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_PART].num;
    const u8 *secBase;
    const u8 *poolBase;
    HSF_PART *out;
    s32 i, j;

    ctx->part = NULL;
    ctx->partNum = 0;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_PART].ofs;
    poolBase = secBase + (u32)num * FPART_SIZE;  /* u16 pool, right after all part headers */
    out = (HSF_PART *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_PART) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_PART) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FPART_SIZE;
        u32 partNum = be32(rec + FPART_NUM);
        u32 vtxElemOfs = be32(rec + FPART_VERTEX);  /* a u16-ELEMENT index, so *2 for the byte offset */
        u16 *vtx = NULL;

        out[i].name = GetStr(ctx, be32(rec + FPART_NAME));
        out[i].num = partNum;
        if (partNum > 0) {
            vtx = (u16 *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(u16) * partNum), ctx->mallocTag);
            for (j = 0; j < (s32)partNum; j++) {
                vtx[j] = be16(poolBase + (vtxElemOfs + (u32)j) * 2);
            }
        }
        out[i].vertex = vtx;
    }
    ctx->part = out;
    ctx->partNum = num;
}

static HSF_PART *FindPartById(const LoadCtx *ctx, s32 id)
{
    /* game/hsfload.c's SearchPartPtr(): -1 == none. */
    if (id < 0 || id >= ctx->partNum) return NULL;
    return &ctx->part[id];
}

/* game/hsfload.c's ClusterLoad(). Note two deliberate in-place-faithful
 * choices, both matching the original exactly:
 *   - `targetName` is stored as the resolved STRING here, NOT the object
 *     index. game/hsfman.c's ClusterAdjustObject() (called from
 *     Hu3DMotionClusterSet at model-create time, once, guarded by the
 *     `adjusted` flag) overwrites this union field in place with the real
 *     object index later -- storing the string now is exactly what lets
 *     that already-compiled resolver work. `adjusted` therefore MUST start 0.
 *   - `vertex` is a SYMBOL-POOL index (like HSF_MESH.child/.shape): the pool
 *     holds `vertexNum` further vertex-buffer indices at that position. */
static void LoadClusters(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_CLUSTER].num;
    const u8 *secBase;
    HSF_CLUSTER *out;
    s32 i, j, k;

    ctx->cluster = NULL;
    ctx->clusterNum = 0;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_CLUSTER].ofs;
    out = (HSF_CLUSTER *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_CLUSTER) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_CLUSTER) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FCLU_SIZE;
        s32 partId = (s32)be32(rec + FCLU_PART);
        u32 vertexNum = be32(rec + FCLU_VERTEXNUM);
        u32 vertexSym = be32(rec + FCLU_VERTEX);
        HSF_BUFFER **vtxArr = NULL;

        out[i].name[0] = GetStr(ctx, be32(rec + FCLU_NAME0));
        out[i].name[1] = GetStr(ctx, be32(rec + FCLU_NAME1));
        out[i].targetName = GetStr(ctx, be32(rec + FCLU_TARGETNAME));  /* ClusterAdjustObject overwrites -> index */
        out[i].part = FindPartById(ctx, partId);
        out[i].index = bef32(rec + FCLU_INDEX);
        for (k = 0; k < 32; k++) {
            out[i].weight[k] = bef32(rec + FCLU_WEIGHT + (u32)k * 4);
        }
        out[i].adjusted = 0;  /* MUST be 0 -- see this function's own header comment */
        out[i].unk95 = rec[FCLU_UNK95];
        out[i].type = be16(rec + FCLU_TYPE);
        out[i].vertexNum = vertexNum;

        if (vertexNum > 0) {
            vtxArr = (HSF_BUFFER **)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BUFFER *) * vertexNum), ctx->mallocTag);
            for (j = 0; j < (s32)vertexNum; j++) {
                u32 vtxIdx = SymAt(ctx, vertexSym + (u32)j);
                vtxArr[j] = (vtxIdx < (u32)ctx->vertexNum) ? &ctx->vertex[vtxIdx] : NULL;
            }
        }
        out[i].vertex = vtxArr;
    }
    ctx->cluster = out;
    ctx->clusterNum = num;
}

/* game/hsfload.c's FindClusterName(): linear search on name[0]. Motion
 * CLUSTER/CLUSTER_WEIGHT tracks resolve their target string to a cluster
 * index this way (see LoadMotion). */
static s32 FindClusterIdxByName(const LoadCtx *ctx, const char *name)
{
    s32 i;
    for (i = 0; i < ctx->clusterNum; i++) {
        if (ctx->cluster[i].name[0] && strcmp(ctx->cluster[i].name[0], name) == 0) return i;
    }
    return -1;
}

/* game/hsfload.c's ShapeLoad(): num16[1] is the vertex-buffer count; `vertex`
 * is a symbol-pool index whose entries are vertex-buffer indices. */
static void LoadShapes(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_SHAPE].num;
    const u8 *secBase;
    HSF_SHAPE *out;
    s32 i, j;

    ctx->shape = NULL;
    ctx->shapeNum = 0;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_SHAPE].ofs;
    out = (HSF_SHAPE *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_SHAPE) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_SHAPE) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FSHP_SIZE;
        u16 count = be16(rec + FSHP_NUM16 + 2);  /* num16[1] */
        u32 vertexSym = be32(rec + FSHP_VERTEX);
        HSF_BUFFER **vtxArr = NULL;

        out[i].name = GetStr(ctx, be32(rec + FSHP_NAME));
        out[i].num16[0] = be16(rec + FSHP_NUM16 + 0);
        out[i].num16[1] = count;

        if (count > 0) {
            vtxArr = (HSF_BUFFER **)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_BUFFER *) * (u32)count), ctx->mallocTag);
            for (j = 0; j < count; j++) {
                u32 vtxIdx = SymAt(ctx, vertexSym + (u32)j);
                vtxArr[j] = (vtxIdx < (u32)ctx->vertexNum) ? &ctx->vertex[vtxIdx] : NULL;
            }
        }
        out[i].vertex = vtxArr;
    }
    ctx->shape = out;
    ctx->shapeNum = num;
}

/* ==========================================================================
 * CENV (skeletal-envelope skinning) + its supporting SKELETON (bind-pose
 * joint transforms) and MATRIX (the runtime bone-matrix palette) sections
 * -- the opening storybook's chain (chn1..3) and any other genuinely
 * skinned sub-object.
 *
 * Consumed, unmodified, by game/EnvelopeExec.c (InitEnvelope at load, then
 * EnvelopeProc every frame) -- like cluster, it deforms IN PLACE into the
 * model's own vertex/normal buffers, reading from a pristine bind-pose copy
 * (vtxtop/normtop) this loader allocates once. game/hsfman.c's Hu3DExec
 * gates EnvelopeProc on hsf->cenvNum, and game/hsfdraw.c switches the normal
 * vertex-array format to GX_F32 (float) when cenvNum != 0 -- so enabling
 * cenv REQUIRES float normals (LoadNormalArrays honors ctx->normalIsFloat,
 * set from realCenvNum, for exactly this). Everything allocates once, at
 * load, into HEAP_MODEL; the per-frame EnvelopeProc/SetEnvelop math
 * allocates nothing.
 * ========================================================================== */

/* game/hsfload.c's CenvLoad(): a two-region pool -- the single/dual/multi
 * arrays sit right after all the cenv headers (dataP); the dual/multi WEIGHT
 * sub-arrays sit right after ALL of those (weightP). Every header field that
 * points into either pool is a byte offset relative to that pool's base,
 * exactly as the original's in-place `+dataP`/`+weightP` arithmetic uses. */
static void LoadCenv(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_CENV].num;
    const u8 *secBase, *dataP, *weightP;
    HSF_CENV *out;
    s32 i, j, k;
    u32 weightCursor;

    ctx->cenv = NULL;
    ctx->cenvNum = 0;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_CENV].ofs;
    dataP = secBase + (u32)num * FCENV_SIZE;
    out = (HSF_CENV *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_CENV) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_CENV) * (u32)num);

    /* Pass 1: sum the single/dual/multi array sizes across ALL cenvs to
     * locate weightP, exactly as CenvLoad's own first loop does. */
    weightCursor = 0;
    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FCENV_SIZE;
        weightCursor += be32(rec + FCENV_SINGLECOUNT) * FCSINGLE_SIZE;
        weightCursor += be32(rec + FCENV_DUALCOUNT) * FCDUAL_SIZE;
        weightCursor += be32(rec + FCENV_MULTICOUNT) * FCMULTI_SIZE;
    }
    weightP = dataP + weightCursor;

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FCENV_SIZE;
        u32 singleOfs = be32(rec + FCENV_SINGLEDATA);
        u32 dualOfs = be32(rec + FCENV_DUALDATA);
        u32 multiOfs = be32(rec + FCENV_MULTIDATA);
        u32 sc = be32(rec + FCENV_SINGLECOUNT);
        u32 dc = be32(rec + FCENV_DUALCOUNT);
        u32 mc = be32(rec + FCENV_MULTICOUNT);

        out[i].name = GetStr(ctx, be32(rec + FCENV_NAME));
        out[i].singleCount = sc;
        out[i].dualCount = dc;
        out[i].multiCount = mc;
        out[i].vtxCount = be32(rec + FCENV_VTXCOUNT);
        out[i].copyCount = be32(rec + FCENV_COPYCOUNT);

        if (sc > 0) {
            HSF_CENV_SINGLE *s = (HSF_CENV_SINGLE *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_CENV_SINGLE) * sc), ctx->mallocTag);
            const u8 *sp = dataP + singleOfs;
            for (j = 0; j < (s32)sc; j++) {
                const u8 *r = sp + (u32)j * FCSINGLE_SIZE;
                s[j].target = be32(r + FCSINGLE_TARGET);
                s[j].pos = be16(r + FCSINGLE_POS);
                s[j].posNum = be16(r + FCSINGLE_POSNUM);
                s[j].normal = be16(r + FCSINGLE_NORMAL);
                s[j].normalNum = be16(r + FCSINGLE_NORMALNUM);
            }
            out[i].singleData = s;
        }
        if (dc > 0) {
            HSF_CENV_DUAL *d = (HSF_CENV_DUAL *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_CENV_DUAL) * dc), ctx->mallocTag);
            const u8 *dp = dataP + dualOfs;
            memset(d, 0, sizeof(HSF_CENV_DUAL) * dc);
            for (j = 0; j < (s32)dc; j++) {
                const u8 *r = dp + (u32)j * FCDUAL_SIZE;
                u32 wn = be32(r + FCDUAL_WEIGHTNUM);
                u32 wOfs = be32(r + FCDUAL_WEIGHT);
                d[j].target1 = be32(r + FCDUAL_TARGET1);
                d[j].target2 = be32(r + FCDUAL_TARGET2);
                d[j].weightNum = wn;
                if (wn > 0) {
                    HSF_CENV_DUAL_WEIGHT *w = (HSF_CENV_DUAL_WEIGHT *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_CENV_DUAL_WEIGHT) * wn), ctx->mallocTag);
                    const u8 *wp = weightP + wOfs;
                    for (k = 0; k < (s32)wn; k++) {
                        const u8 *wr = wp + (u32)k * FCDW_SIZE;
                        w[k].weight = bef32(wr + FCDW_WEIGHT);
                        w[k].pos = be16(wr + FCDW_POS);
                        w[k].posNum = be16(wr + FCDW_POSNUM);
                        w[k].normal = be16(wr + FCDW_NORMAL);
                        w[k].normalNum = be16(wr + FCDW_NORMALNUM);
                    }
                    d[j].weight = w;
                }
            }
            out[i].dualData = d;
        }
        if (mc > 0) {
            HSF_CENV_MULTI *m = (HSF_CENV_MULTI *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_CENV_MULTI) * mc), ctx->mallocTag);
            const u8 *mp = dataP + multiOfs;
            memset(m, 0, sizeof(HSF_CENV_MULTI) * mc);
            for (j = 0; j < (s32)mc; j++) {
                const u8 *r = mp + (u32)j * FCMULTI_SIZE;
                u32 wn = be32(r + FCMULTI_WEIGHTNUM);
                u32 wOfs = be32(r + FCMULTI_WEIGHT);
                m[j].weightNum = wn;
                m[j].pos = be16(r + FCMULTI_POS);
                m[j].posNum = be16(r + FCMULTI_POSNUM);
                m[j].normal = be16(r + FCMULTI_NORMAL);
                m[j].normalNum = be16(r + FCMULTI_NORMALNUM);
                if (wn > 0) {
                    HSF_CENV_MULTI_WEIGHT *w = (HSF_CENV_MULTI_WEIGHT *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_CENV_MULTI_WEIGHT) * wn), ctx->mallocTag);
                    const u8 *wp = weightP + wOfs;
                    for (k = 0; k < (s32)wn; k++) {
                        const u8 *wr = wp + (u32)k * FCMW_SIZE;
                        w[k].target = be32(wr + FCMW_TARGET);
                        w[k].value = bef32(wr + FCMW_VALUE);
                    }
                    m[j].weight = w;
                }
            }
            out[i].multiData = m;
        }
    }
    ctx->cenv = out;
    ctx->cenvNum = num;
}

static HSF_CENV *FindCenvById(const LoadCtx *ctx, s32 id)
{
    /* game/hsfload.c's SearchCenvPtr(): -1 == none. */
    if (id < 0 || id >= ctx->cenvNum) return NULL;
    return &ctx->cenv[id];
}

/* game/hsfload.c's SkeletonLoad(): name(string ofs) + a plain HSF_TRANSFORM. */
static void LoadSkeleton(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_SKELETON].num;
    const u8 *secBase;
    HSF_SKELETON *out;
    s32 i;

    ctx->skeleton = NULL;
    ctx->skeletonNum = 0;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_SKELETON].ofs;
    out = (HSF_SKELETON *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_SKELETON) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_SKELETON) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FSKEL_SIZE;
        out[i].name = GetStr(ctx, be32(rec + FSKEL_NAME));
        out[i].transform = ReadTransform(rec + FSKEL_TRANSFORM);
    }
    ctx->skeleton = out;
    ctx->skeletonNum = num;
}

/* game/hsfload.c's MatrixLoad() + game/EnvelopeExec.c's own indexing. The
 * file has one HSF_MATRIX header (base_idx + count) followed by the runtime
 * bone-matrix PALETTE -- but InitEnvelope() fills that palette itself at
 * load (SetMtx/SetRevMtx), never reading the file's placeholder Mtx bytes,
 * so this only needs to allocate a correctly-SIZED, zeroed palette. The
 * palette is indexed (SetRevMtx/SetEnvelop) up to roughly
 * base_idx + count + count*meshCount + count; this allocates that with
 * generous slack so a mis-estimate can never fault (over-allocation of a
 * few dozen KB of 48-byte matrices, all HEAP_MODEL-owned). */
static void LoadMatrix(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_MATRIX].num;
    const u8 *rec;
    u32 baseIdx, count, meshCount, paletteLen;
    HSF_MATRIX *out;
    Mtx *mtxData;
    s32 i;

    ctx->matrix = NULL;
    ctx->matrixNum = 0;
    if (num <= 0) return;

    rec = ctx->base + ctx->h.sec[FSEC_MATRIX].ofs;
    baseIdx = be32(rec + FMTX_BASEIDX);
    count = be32(rec + FMTX_COUNT);

    /* meshCount = number of HSF_OBJ_MESH objects -- the SetRevMtx/SetEnvelop
     * palette stride multiplier (see this function's own header comment). */
    meshCount = 0;
    for (i = 0; i < ctx->objectNum; i++) {
        if (ctx->object[i].type == HSF_OBJ_MESH) meshCount++;
    }
    paletteLen = baseIdx + count * (meshCount + 3) + 64;  /* generous, crash-proof over-allocation */

    out = (HSF_MATRIX *)HuMemDirectMallocNum(HEAP_MODEL, (s32)sizeof(HSF_MATRIX), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_MATRIX));
    mtxData = (Mtx *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(Mtx) * paletteLen), ctx->mallocTag);
    memset(mtxData, 0, sizeof(Mtx) * paletteLen);
    out->base_idx = baseIdx;
    out->count = count;
    out->data = mtxData;

    ctx->matrix = out;
    ctx->matrixNum = num;
}

/* ==========================================================================
 * MAPATTR. Faithfully mirrors game/hsfload.c's own MapAttrLoad(), a plain
 * in-place "resolve one index field into a real pointer" loader, same
 * shape as PartLoad()/ShapeLoad(). See FMAPATTR_* above for the important
 * caveat: mapAttr is the game BOARD's own wall/floor collision grid
 * (game/mapspace.c), NOT a rendering/material construct -- game/hsfdraw.c
 * never reads hsf->mapAttr at all; its real consumer is game/mapspace.c's
 * MapWall/MapPos.
 * ========================================================================== */
static void LoadMapAttr(LoadCtx *ctx)
{
    s32 num = ctx->h.sec[FSEC_MAPATTR].num;
    const u8 *secBase;
    const u8 *poolBase;
    HSF_MAPATTR *out;
    s32 i;

    ctx->mapAttr = NULL;
    ctx->mapAttrNum = 0;
    if (num <= 0) return;

    secBase = ctx->base + ctx->h.sec[FSEC_MAPATTR].ofs;
    poolBase = secBase + (u32)num * FMAPATTR_SIZE;  /* u16 pool, right after all mapAttr headers */
    out = (HSF_MAPATTR *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_MAPATTR) * (u32)num), ctx->mallocTag);
    memset(out, 0, sizeof(HSF_MAPATTR) * (u32)num);

    for (i = 0; i < num; i++) {
        const u8 *rec = secBase + (u32)i * FMAPATTR_SIZE;
        u32 dataElemIdx = be32(rec + FMAPATTR_DATA);
        u32 dataLen = be32(rec + FMAPATTR_DATALEN);
        u16 *data = NULL;

        out[i].minX = bef32(rec + FMAPATTR_MINX);
        out[i].minZ = bef32(rec + FMAPATTR_MINZ);
        out[i].maxX = bef32(rec + FMAPATTR_MAXX);
        out[i].maxZ = bef32(rec + FMAPATTR_MAXZ);
        out[i].dataLen = dataLen;

        /* Plain u16-element index (like HSF_PART.vertex), NOT a symbol-pool
         * index -- MapAttrLoad's own `&data[(u32)mapAttrFile->data]` never
         * goes through SymAt/the symbol table. dataLen is a real, consumed
         * ELEMENT count (game/mapspace.c's MapWallCheck walks exactly this
         * many u16s), so this allocates+converts exactly that many. */
        if (dataLen > 0) {
            data = (u16 *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(u16) * dataLen), ctx->mallocTag);
            {
                u32 k;
                for (k = 0; k < dataLen; k++) {
                    data[k] = be16(poolBase + (dataElemIdx + k) * 2);
                }
            }
        }
        out[i].data = data;
    }
    ctx->mapAttr = out;
    ctx->mapAttrNum = num;
}

/* ==========================================================================
 * Motion: per-object/material/attribute keyframe animation embedded
 * directly in the same HSF file as the static geometry this deserializer
 * already loads -- the mechanism behind the opening cutscene's camera
 * moves, storybook page-turns, and model motion.
 *
 * Consumed by game/hsfmotion.c's Hu3DMotionExec() every frame, completely
 * UNMODIFIED (confirmed by direct source review): the instant
 * hsf->motionNum != 0, game/hsfman.c's Hu3DModelCreate() (also unmodified)
 * automatically wires this model's own embedded motion up as its motId
 * (Hu3DMotionModelCreate() just points a fresh HU3D_MOTION slot's .hsf
 * right back at THIS SAME model's own hsf -- no separate load, no extra
 * API call needed from this deserializer), and game/hsfdraw.c's objMesh()
 * (also unmodified) switches from reading mesh.base (the static bind pose)
 * to mesh.curr (the live value Hu3DMotionExec() writes into every frame)
 * the moment that model's motId is non-NONE -- see objCall()'s own
 * `attachMotionF = (modelP->motId != HU3D_MOTIONID_NONE)` line. Every
 * piece of this consumption path is unmodified decomp code; this
 * deserializer's only job is to feed it correctly-shaped data.
 *
 * Faithfully mirrors game/hsfload.c's own MotionLoad()/MotionLoadTransform()/
 * MotionLoadCluster()/MotionLoadClusterWeight()/MotionLoadMaterial()/
 * MotionLoadAttribute(), including one real quirk worth calling out
 * explicitly: ONLY fileMotion[0] is ever processed, regardless of
 * head.motion.num (every real model this project has inspected has exactly
 * one motion section entry; this matches what the original compiled game
 * does too, not an approximation this port introduces), and hsf->motionNum
 * ends up being fileMotion[0].numTracks -- NOT head.motion.num -- copying
 * the original's own field reuse verbatim. That name is misleading, but
 * every consumer (Hu3DModelCreate et al.) only ever tests it for
 * nonzero-ness as a "this model has a motion" boolean, so the exact value
 * is behaviorally inert either way; kept byte-faithful rather than
 * "corrected" to a real motion count nobody asks for.
 *
 * Cluster/cluster-weight tracks (the vertex-morph deformation behind e.g.
 * the storybook's own page-curl -- REL/bootDll/opening.c's own
 * Hu3DMotionClusterSpeedSet(OpeningMdlId[1], 0, 0.5f) call confirms the
 * book model genuinely has one) resolve against the real cluster array
 * LoadClusters() already built (see the HSF_TRACK_CLUSTER/_WEIGHT case in
 * LoadMotion() below) -- a miss yields -1, the same "not found" sentinel
 * game/hsfload.c's own FindClusterName() returns for a genuine miss. This
 * is load-bearing, not cosmetic: game/hsfmotion.c's Hu3DMotionExec()
 * dereferences `hsf->cluster[trackP->cluster]` completely unconditionally
 * for these two track types (gated only on a MOTOFF attribute flag this
 * port never sets, NOT on HU3D_ATTR_CLUSTER_ON) -- with a stale or
 * out-of-range index and no bounds check, that would be a genuine access
 * violation the very first frame this model's motion runs.
 * patches/decomp/src/game/hsfmotion.c.patch adds the matching bounds check
 * on the consumer side (the -1 sentinel alone isn't sufficient without
 * it); see that patch's own comment for why both sides of this fix are
 * needed.
 * ========================================================================== */

/* game/hsfload.c's own FindObjectName()/FindClusterName()/
 * FindAttributeName(): plain linear strcmp search. Object/attribute names
 * were already resolved into real strings earlier in this same
 * MP6_LoadHSFNative() call (LoadObjects()/LoadAttributes()), so these
 * search the SAME model's own arrays -- exactly matching the original's
 * `MotionOnly == FALSE` behavior (the ONLY case this deserializer's
 * automatic-embedded-motion scope ever needs; see this section's own
 * header comment). */
static s32 FindObjIdxByName(const LoadCtx *ctx, const char *name)
{
    s32 i;
    for (i = 0; i < ctx->objectNum; i++) {
        if (ctx->object[i].name && strcmp(ctx->object[i].name, name) == 0) return i;
    }
    return -1;
}

static s32 FindAttrIdxByName(const LoadCtx *ctx, const char *name)
{
    s32 i;
    for (i = 0; i < ctx->attributeNum; i++) {
        if (ctx->attribute[i].name && strcmp(ctx->attribute[i].name, name) == 0) return i;
    }
    return -1;
}

/* Resolves one track's curve-keyframe data (HSF_CURVE_STEP/_LINEAR/_BEZIER/
 * _BITMAP/_CONST, game/hsfformat.h) into a freshly allocated, fully
 * BE-converted native array -- exactly matching every MotionLoadXxx()
 * function in game/hsfload.c (they all share this identical switch body;
 * HSF_CURVE_BITMAP is only ever actually used by ATTRIBUTE tracks in
 * practice, but resolving it uniformly for every track type is a strict
 * superset of the original's own behavior, not a divergence, since no
 * other track type's real curveType is ever BITMAP). */
static void ResolveTrackCurve(LoadCtx *ctx, const u8 *trackRec, const u8 *poolBase, HSF_TRACK *outTrack)
{
    u16 curveType = be16(trackRec + FTRACK_CURVETYPE);
    u16 numKeyframes = be16(trackRec + FTRACK_NUMKEY);
    u32 rawDataOfs = be32(trackRec + FTRACK_DATA);
    s32 i;

    outTrack->curveType = curveType;
    outTrack->numKeyframes = numKeyframes;

    switch (curveType) {
    case HSF_CURVE_STEP:
    case HSF_CURVE_LINEAR:
        /* HSF_CONSTANT_KEY/HSF_LINEAR_KEY are both plain float[2] -- no
         * pointers inside, file and native strides match exactly (8
         * bytes/key); still need the BE->host float conversion though. */
        if (numKeyframes > 0) {
            float (*data)[2] = (float (*)[2])HuMemDirectMalloc(
                HEAP_MODEL, (s32)(sizeof(float) * 2 * (u32)numKeyframes));
            const u8 *src = poolBase + rawDataOfs;
            for (i = 0; i < numKeyframes; i++) {
                data[i][0] = bef32(src + (u32)i * FCURVEKEY_STEP_SIZE + 0);
                data[i][1] = bef32(src + (u32)i * FCURVEKEY_STEP_SIZE + 4);
            }
            outTrack->data = data;
        } else {
            outTrack->data = NULL;
        }
        break;

    case HSF_CURVE_BEZIER:
        if (numKeyframes > 0) {
            float (*data)[4] = (float (*)[4])HuMemDirectMalloc(
                HEAP_MODEL, (s32)(sizeof(float) * 4 * (u32)numKeyframes));
            const u8 *src = poolBase + rawDataOfs;
            for (i = 0; i < numKeyframes; i++) {
                data[i][0] = bef32(src + (u32)i * FCURVEKEY_BEZIER_SIZE + 0);
                data[i][1] = bef32(src + (u32)i * FCURVEKEY_BEZIER_SIZE + 4);
                data[i][2] = bef32(src + (u32)i * FCURVEKEY_BEZIER_SIZE + 8);
                data[i][3] = bef32(src + (u32)i * FCURVEKEY_BEZIER_SIZE + 12);
            }
            outTrack->data = data;
        } else {
            outTrack->data = NULL;
        }
        break;

    case HSF_CURVE_BITMAP:
        /* Native HSF_BITMAP_KEY carries a real HSF_BITMAP* (pointer-widened
         * on this 64-bit port, same story as everywhere else in this file)
         * -- the file's own 4-byte "data" slot per key is a raw BITMAP
         * INDEX, resolved against ctx->bitmap exactly like AttributeLoad's
         * own bitmap resolution (already loaded earlier in this same
         * MP6_LoadHSFNative() call). */
        if (numKeyframes > 0) {
            HSF_BITMAP_KEY *data = (HSF_BITMAP_KEY *)HuMemDirectMalloc(
                HEAP_MODEL, (s32)(sizeof(HSF_BITMAP_KEY) * (u32)numKeyframes));
            const u8 *src = poolBase + rawDataOfs;
            for (i = 0; i < numKeyframes; i++) {
                const u8 *krec = src + (u32)i * FCURVEKEY_BITMAP_SIZE;
                s32 bmpId = (s32)be32(krec + 4);
                data[i].time = bef32(krec + 0);
                data[i].data = (bmpId >= 0 && bmpId < ctx->bitmapNum) ? &ctx->bitmap[bmpId] : NULL;
            }
            outTrack->data = data;
        } else {
            outTrack->data = NULL;
        }
        break;

    case HSF_CURVE_CONST:
        /* No pool at all -- game/hsfload.c's own MotionLoadXxx() switch
         * does literally nothing for this case, leaving the in-place
         * struct's raw file bytes as the value (the file already stores
         * the literal float directly in this same 4-byte slot). Mirrored
         * here by decoding those same raw bytes as a float directly
         * (rather than a byte-for-byte struct copy, which would wrongly
         * reinterpret them as a pointer on this 64-bit, pointer-widened
         * port -- see this file's own top-of-file note). */
        outTrack->value = bef32(trackRec + FTRACK_DATA);
        break;

    default:
        /* Unrecognized curve type -- leave data/value zeroed (already
         * memset by LoadMotion()) rather than guess; game/hsfmotion.c's own
         * GetCurve()/GetClusterCurve() switches fall through to `return 0;`
         * for any type they don't recognize either, so this stays
         * consistent with real consumer behavior. */
        break;
    }
}

static void LoadMotion(LoadCtx *ctx)
{
    s32 motionSecNum = ctx->h.sec[FSEC_MOTION].num;
    const u8 *secBase;
    const u8 *motRec;
    const u8 *trackArrayBase;
    const u8 *poolBase;
    s32 numTracks;
    HSF_MOTION *motion;
    HSF_TRACK *tracks;
    s32 i;

    ctx->motion = NULL;
    ctx->motionNum = 0;
    if (motionSecNum <= 0) return;

    if (motionSecNum > 1) {
        /* Never observed in any real file this project has inspected --
         * game/hsfload.c's own MotionLoad() only ever processes
         * fileMotion[0] regardless, so this is purely an FYI, not a bug:
         * any motion[1..] entries would be exactly as dead on real
         * hardware as they are here. */
        MP6_LOG_ONCE("HSF", "LoadMotion-multi-motion-section");
    }

    secBase = ctx->base + ctx->h.sec[FSEC_MOTION].ofs;
    motRec = secBase;  /* fileMotion[0] -- see this section's own header comment */
    numTracks = (s32)be32(motRec + FMOT_NUMTRACKS);

    motion = (HSF_MOTION *)HuMemDirectMallocNum(HEAP_MODEL, sizeof(HSF_MOTION), ctx->mallocTag);
    memset(motion, 0, sizeof(HSF_MOTION));
    motion->name = GetStr(ctx, be32(motRec + FMOT_NAME));
    motion->maxTime = bef32(motRec + FMOT_MAXTIME);

    if (numTracks <= 0) {
        ctx->motion = motion;
        ctx->motionNum = 0;  /* mirrors fileMotion->numTracks exactly, see header comment */
        return;
    }

    /* Track array sits right after ALL motionSecNum many HSF_MOTION file
     * records (matches `trackStart = &fileMotion[head.motion.num];` --
     * never read from any "ofs" field, an implicit-placement special case
     * this format's motion section uses, unlike every other section); the
     * shared curve-data pool sits right after that one motion's own
     * numTracks-many HSF_TRACK records (matches `trackData =
     * &trackStart[fileMotion->numTracks];`). */
    trackArrayBase = secBase + (u32)motionSecNum * FMOT_SIZE;
    poolBase = trackArrayBase + (u32)numTracks * FTRACK_SIZE;

    tracks = (HSF_TRACK *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_TRACK) * (u32)numTracks), ctx->mallocTag);
    memset(tracks, 0, sizeof(HSF_TRACK) * (u32)numTracks);

    for (i = 0; i < numTracks; i++) {
        const u8 *trec = trackArrayBase + (u32)i * FTRACK_SIZE;
        u8 type = trec[FTRACK_TYPE];
        u16 rawTarget = be16(trec + FTRACK_TARGET);
        HSF_TRACK *out = &tracks[i];

        out->type = type;
        out->start = trec[FTRACK_START];

        switch (type) {
        case HSF_TRACK_TRANSFORM:
        case HSF_TRACK_MORPH:
            /* game/hsfload.c's MotionLoadTransform(): only resolved against
             * this model's own object names when it HAS any -- mirrors the
             * original's own `if(objtop)` gate exactly (see this section's
             * own header comment for why leaving the raw string-offset
             * bytes alone in the else-branch, rather than writing -1, is
             * the faithful choice here). */
            if (ctx->objectNum > 0) {
                char *name = GetMotionStr(ctx, rawTarget);
                out->target = (u16)FindObjIdxByName(ctx, name);
            } else {
                out->target = rawTarget;
            }
            /* Same bytes as HSF_TRACK_MORPH's own `morphWeight` -- see
             * this section's own header comment; writing through `channel`
             * is byte-identical to writing through `morphWeight`. */
            out->channel = be16(trec + FTRACK_UNION2 + 2);
            break;

        case HSF_TRACK_CLUSTER:
        case HSF_TRACK_CLUSTER_WEIGHT:
            /* Resolved against the cluster array LoadClusters() built
             * (called before LoadMotion, see MP6_LoadHSFNative's own call
             * order) -- game/hsfload.c's MotionLoadCluster()/
             * MotionLoadClusterWeight() resolve the track's target STRING
             * to a cluster index via FindClusterName exactly this way. A
             * miss still yields -1 (a real "not found" value the original
             * returns too); every consumer that indexes
             * hsf->cluster[track->cluster] is bounds-guarded against it
             * (patches/decomp/src/game/hsfmotion.c.patch for Hu3DMotionExec,
             * patches/decomp/src/game/ClusterExec.c.patch for
             * ClusterMotionExec) -- see those patches' own comments. */
            {
                char *cname = GetMotionStr(ctx, rawTarget);
                out->cluster = (s16)FindClusterIdxByName(ctx, cname);
            }
            /* clusterWeight (the weight-array index a CLUSTER_WEIGHT track
             * needs) is a plain raw index already on disk, no lookup at
             * all -- copied through unconditionally for CLUSTER tracks too
             * since nothing ever reads it there (matches the original's
             * own in-place "only touch what MotionLoadCluster() actually
             * assigns" behavior). */
            out->clusterWeight = (s32)be32(trec + FTRACK_UNION2);
            break;

        case HSF_TRACK_MATERIAL:
            /* game/hsfload.c's MotionLoadMaterial(): attrIdx is already a
             * real material-array index on disk, no name lookup at all. */
            out->attrIdx = (s16)be16(trec + FTRACK_UNION2);
            out->channel = be16(trec + FTRACK_UNION2 + 2);
            break;

        case HSF_TRACK_ATTRIBUTE:
        {
            /* game/hsfload.c's MotionLoadAttribute(): the raw target/
             * cluster field is checked for -1 FIRST (a real file
             * convention, not a bug); only then is it treated as a string
             * offset and searched against this model's own attribute
             * names -- otherwise the raw on-disk attrIdx bytes are left
             * untouched, matching the original's own in-place behavior. */
            s16 rawCluster = (s16)rawTarget;
            if (rawCluster != -1) {
                char *name = GetMotionStr(ctx, rawTarget);
                out->attrIdx = (s16)FindAttrIdxByName(ctx, name);
            } else {
                out->attrIdx = (s16)be16(trec + FTRACK_UNION2);
            }
            out->cluster = rawCluster;
            out->channel = be16(trec + FTRACK_UNION2 + 2);
            break;
        }

        default:
            /* Unknown track type -- leave target/cluster/attrIdx/channel
             * zeroed (already memset) rather than guess; every consumer
             * switch (Hu3DMotionExec() et al.) already ignores
             * unrecognized types by simply not matching any case. */
            break;
        }

        ResolveTrackCurve(ctx, trec, poolBase, out);
    }

    motion->numTracks = numTracks;
    motion->track = tracks;
    ctx->motion = motion;
    ctx->motionNum = numTracks;  /* mirrors fileMotion->numTracks, see this section's own header comment */
}

/* ==========================================================================
 * Top-level entry point.
 * ========================================================================== */
HSF_DATA *MP6_LoadHSFNative(void *dataPtr)
{
    const u8 *base = (const u8 *)dataPtr;
    LoadCtx ctx;
    HSF_DATA *hsf;
    s32 realCenvNum, realClusterNum, realShapeNum, realSkeletonNum;
    s32 realMotionNum, realMapAttrNum, realPartNum, realMatrixNum;
    s32 sceneNum;

    MP6_LOG_ONCE("HSF", "LoadHSF-native");

    memset(&ctx, 0, sizeof(ctx));

    if (!base) {
        /* Should never happen (callers always pass a freshly decoded
         * buffer) -- defensively return a safe, inert, non-NULL model
         * rather than crash. */
        hsf = (HSF_DATA *)HuMemDirectMallocNum(HEAP_MODEL, sizeof(HSF_DATA), ctx.mallocTag);
        memset(hsf, 0, sizeof(HSF_DATA));
        return hsf;
    }

    ctx.base = base;
    ReadHeader(base, &ctx.h);

    if (memcmp(ctx.h.magic, "HSF", 3) != 0) {
        fprintf(stderr,
                "[WARN] MP6_LoadHSFNative: magic \"%.8s\" doesn't start with \"HSF\" -- "
                "parsing anyway (best-effort); rendering may be wrong for this model\n",
                ctx.h.magic);
    }

    realCenvNum = ctx.h.sec[FSEC_CENV].num;
    ctx.normalIsFloat = (realCenvNum > 0);

    /* Allocate the outer struct FIRST, so its own address is known BEFORE
     * any sub-allocation happens -- every HuMemDirectMallocNum call inside
     * the LoadXxx helpers below tags its allocation with ctx.mallocTag, so
     * Hu3DModelKill's existing bulk free can find and reclaim it later.
     * See LoadCtx's own mallocTag comment above for the full story. */
    hsf = (HSF_DATA *)HuMemDirectMallocNum(HEAP_MODEL, sizeof(HSF_DATA), ctx.mallocTag);
    memset(hsf, 0, sizeof(HSF_DATA));
    ctx.mallocTag = (u32)hsf;

    LoadPalettes(&ctx);
    LoadBitmaps(&ctx);
    LoadMaterials(&ctx);
    LoadAttributes(&ctx);
    LoadVertexArrays(&ctx);
    LoadNormalArrays(&ctx);
    LoadSTArrays(&ctx);
    LoadColorArrays(&ctx);
    LoadFaceGroups(&ctx);
    LoadCenv(&ctx);    /* before LoadObjects, so each mesh can resolve its cenv pointer */
    LoadObjects(&ctx);
    /* Part before cluster (cluster references parts); both before motion
     * (motion CLUSTER tracks resolve cluster names). Same dependency order
     * game/hsfload.c's own LoadHSF() uses. */
    LoadParts(&ctx);
    LoadClusters(&ctx);
    LoadShapes(&ctx);
    LoadSkeleton(&ctx);  /* needed by InitEnvelope's bind-pose matching (called at end) */
    LoadMotion(&ctx);  /* needs ctx->object/ctx->attribute/ctx->cluster loaded above */
    LoadMatrix(&ctx);  /* needs ctx->object (mesh count) for the palette-size estimate */
    LoadMapAttr(&ctx);  /* independent of every other section */

    /* Scene (fog) -- cheap, always safe, no stub-tolerance needed. */
    sceneNum = ctx.h.sec[FSEC_SCENE].num;
    ctx.sceneNum = 0;
    ctx.scene = NULL;
    if (sceneNum > 0) {
        const u8 *rec = base + ctx.h.sec[FSEC_SCENE].ofs;
        HSF_SCENE *scene = (HSF_SCENE *)HuMemDirectMallocNum(HEAP_MODEL, (s32)(sizeof(HSF_SCENE) * (u32)sceneNum), ctx.mallocTag);
        s32 si;
        for (si = 0; si < sceneNum; si++) {
            const u8 *srec = rec + (u32)si * FSCENE_SIZE;
            scene[si].fogType = (GXFogType)be32(srec + FSCENE_FOGTYPE);
            scene[si].fogStart = bef32(srec + FSCENE_FOGSTART);
            scene[si].fogEnd = bef32(srec + FSCENE_FOGEND);
            scene[si].fogColor.r = srec[FSCENE_FOGCOLOR + 0];
            scene[si].fogColor.g = srec[FSCENE_FOGCOLOR + 1];
            scene[si].fogColor.b = srec[FSCENE_FOGCOLOR + 2];
            scene[si].fogColor.a = srec[FSCENE_FOGCOLOR + 3];
        }
        ctx.scene = scene;
        ctx.sceneNum = sceneNum;
    }

    /* Reports the real on-file deform-section counts for visibility (see
     * LoadMapAttr()'s own header comment for why mapAttr is not a
     * rendering concern). */
    realClusterNum = ctx.h.sec[FSEC_CLUSTER].num;
    realShapeNum = ctx.h.sec[FSEC_SHAPE].num;
    realSkeletonNum = ctx.h.sec[FSEC_SKELETON].num;
    realMotionNum = ctx.h.sec[FSEC_MOTION].num;
    realMapAttrNum = ctx.h.sec[FSEC_MAPATTR].num;
    realPartNum = ctx.h.sec[FSEC_PART].num;
    realMatrixNum = ctx.h.sec[FSEC_MATRIX].num;
    if (realCenvNum > 0 || realClusterNum > 0 || realSkeletonNum > 0 || realMatrixNum > 0 || realMapAttrNum > 0) {
        printf("[HSF] deform sections (magic=%.8s): cenv=%d cluster=%d skeleton=%d matrix=%d part=%d "
               "shape=%d motion=%d mapAttr=%d ALL real\n",
               ctx.h.magic, realCenvNum, realClusterNum, realSkeletonNum, realMatrixNum,
               realPartNum, realShapeNum, realMotionNum, realMapAttrNum);
        fflush(stdout);
    }

    /* hsf was already allocated + zeroed near the top of this function
     * (see the mallocTag allocation comment there) -- just populate it now
     * that every LoadXxx helper above has filled in the matching ctx.*
     * fields. */
    memcpy(hsf->magic, ctx.h.magic, 8);
    hsf->scene = ctx.scene;           hsf->sceneNum = (s16)ctx.sceneNum;
    hsf->attribute = ctx.attribute;   hsf->attributeNum = (s16)ctx.attributeNum;
    hsf->material = ctx.material;     hsf->materialNum = (s16)ctx.materialNum;
    hsf->vertex = ctx.vertex;         hsf->vertexNum = (s16)ctx.vertexNum;
    hsf->normal = ctx.normal;         hsf->normalNum = (s16)ctx.normalNum;
    hsf->st = ctx.st;                 hsf->stNum = (s16)ctx.stNum;
    hsf->color = ctx.color;           hsf->colorNum = (s16)ctx.colorNum;
    hsf->face = ctx.face;             hsf->faceNum = (s16)ctx.faceNum;
    hsf->bitmap = ctx.bitmap;         hsf->bitmapNum = (s16)ctx.bitmapNum;
    hsf->palette = ctx.palette;       hsf->paletteNum = (s16)ctx.paletteNum;
    hsf->root = ctx.root;
    hsf->object = ctx.object;         hsf->objectNum = (s16)ctx.objectNum;
    hsf->cenv = ctx.cenv;             hsf->cenvNum = (s16)ctx.cenvNum;         /* see LoadCenv() */
    hsf->skeleton = ctx.skeleton;     hsf->skeletonNum = (s16)ctx.skeletonNum; /* see LoadSkeleton() */
    hsf->cluster = ctx.cluster;       hsf->clusterNum = (s16)ctx.clusterNum;  /* see LoadClusters() */
    hsf->part = ctx.part;             hsf->partNum = (s16)ctx.partNum;        /* see LoadParts() */
    hsf->shape = ctx.shape;           hsf->shapeNum = (s16)ctx.shapeNum;      /* see LoadShapes() */
    hsf->motion = ctx.motion;         hsf->motionNum = (s16)ctx.motionNum;  /* see LoadMotion() */
    hsf->mapAttr = ctx.mapAttr;       hsf->mapAttrNum = (s16)ctx.mapAttrNum;  /* see LoadMapAttr() */
    hsf->matrix = ctx.matrix;         hsf->matrixNum = (s16)ctx.matrixNum;    /* see LoadMatrix() */

    InitEnvelope(hsf);  /* does real skeletal-envelope setup when hsf->cenvNum != 0 */

    printf("[HSF] loaded magic=%.8s scene=%d material=%d attribute=%d vertex=%d normal=%d "
           "st=%d color=%d face=%d object=%d bitmap=%d palette=%d cluster=%d part=%d shape=%d root=%p\n",
           ctx.h.magic, ctx.sceneNum, ctx.materialNum, ctx.attributeNum, ctx.vertexNum,
           ctx.normalNum, ctx.stNum, ctx.colorNum, ctx.faceNum, ctx.objectNum, ctx.bitmapNum,
           ctx.paletteNum, ctx.clusterNum, ctx.partNum, ctx.shapeNum, (void *)ctx.root);
    fflush(stdout);

    return hsf;
}
