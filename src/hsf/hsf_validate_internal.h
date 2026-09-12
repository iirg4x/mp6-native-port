/* Bounds-only preflight for the decoded, still-big-endian HSF file image.
 *
 * This deliberately has no game/decomp dependencies so the exact validator
 * used by the runtime can also be fuzzed by a tiny host C self-test.  It does
 * not publish pointers or allocate: every byte range, signed count, nested
 * pool, and loader allocation cardinality is proved first. */
#ifndef MP6_HSF_VALIDATE_INTERNAL_H
#define MP6_HSF_VALIDATE_INTERNAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    MP6_HSF_SEC_SCENE = 0, MP6_HSF_SEC_COLOR, MP6_HSF_SEC_MATERIAL,
    MP6_HSF_SEC_ATTRIBUTE, MP6_HSF_SEC_VERTEX, MP6_HSF_SEC_NORMAL,
    MP6_HSF_SEC_ST, MP6_HSF_SEC_FACE, MP6_HSF_SEC_OBJECT,
    MP6_HSF_SEC_BITMAP, MP6_HSF_SEC_PALETTE, MP6_HSF_SEC_MOTION,
    MP6_HSF_SEC_CENV, MP6_HSF_SEC_SKELETON, MP6_HSF_SEC_PART,
    MP6_HSF_SEC_CLUSTER, MP6_HSF_SEC_SHAPE, MP6_HSF_SEC_MAPATTR,
    MP6_HSF_SEC_MATRIX, MP6_HSF_SEC_SYMBOL, MP6_HSF_SEC_STRING,
    MP6_HSF_SEC_COUNT
};

typedef struct MP6HsfValidated_s {
    size_t logicalSize;
    uint32_t stringOffset;
    uint32_t stringSize;
    /* Count of face CORNER indices whose value points outside the owning
     * mesh's (already extent-proved) vertex/normal/color/st buffer.  These
     * are authoring defects the retail loader never checks -- it simply
     * overreads into whatever follows the buffer.  They are reported here
     * instead of failing the preflight so the loader can CLAMP them (see
     * hsf_load_native.c's SanitizeMeshFaceCorners): rejecting the whole
     * model for one bad corner erased real retail content (the Boo
     * shopkeeper model, shipped 3x as capsuleshop[8]/m433[32]/m661[33],
     * whose 2-vertex "itemhook_R" marker mesh names vertex 2). */
    uint32_t oobFaceCorners;
} MP6HsfValidated;

typedef struct MP6HsfPreflightSection_s {
    uint32_t ofs;
    int32_t num;
} MP6HsfPreflightSection;

typedef struct MP6HsfPreflightCtx_s {
    const uint8_t *b;
    size_t n;
    MP6HsfPreflightSection sec[MP6_HSF_SEC_COUNT];
    uint64_t work;
    uint64_t workLimit;
    char *why;
    size_t whyCap;
    uint32_t oobFaceCorners;   /* see MP6HsfValidated.oobFaceCorners */
} MP6HsfPreflightCtx;

static uint16_t mp6_hsf_v_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t mp6_hsf_v_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t mp6_hsf_v_s32(const uint8_t *p)
{
    return (int32_t)mp6_hsf_v_u32(p);
}

static int16_t mp6_hsf_v_s16(const uint8_t *p)
{
    return (int16_t)mp6_hsf_v_u16(p);
}

static int mp6_hsf_v_fail(MP6HsfPreflightCtx *c, const char *what,
                          uint32_t a, uint32_t b)
{
    if (c->why != NULL && c->whyCap != 0) {
        (void)snprintf(c->why, c->whyCap, "%s (%u, %u)", what,
                       (unsigned)a, (unsigned)b);
    }
    return 0;
}

static int mp6_hsf_v_range(const MP6HsfPreflightCtx *c, uint64_t ofs,
                           uint64_t bytes)
{
    return ofs <= (uint64_t)c->n && bytes <= (uint64_t)c->n - ofs;
}

static int mp6_hsf_v_work(MP6HsfPreflightCtx *c, uint64_t count)
{
    if (count > c->workLimit - c->work) {
        return mp6_hsf_v_fail(c, "validation work budget exceeded",
                              (uint32_t)(count > UINT32_MAX ? UINT32_MAX : count), 0);
    }
    c->work += count;
    return 1;
}

static int mp6_hsf_v_nested_count(MP6HsfPreflightCtx *c, uint32_t count,
                                  uint32_t nativeStride, const char *what)
{
    if (nativeStride == 0 || count > (uint32_t)INT_MAX / nativeStride) {
        return mp6_hsf_v_fail(c, what, count, nativeStride);
    }
    return mp6_hsf_v_work(c, count);
}

static int mp6_hsf_v_string(MP6HsfPreflightCtx *c, uint32_t strOfs,
                            const char *what)
{
    const MP6HsfPreflightSection *s = &c->sec[MP6_HSF_SEC_STRING];
    const uint8_t *p;
    size_t remain;
    if (strOfs >= (uint32_t)s->num) {
        return mp6_hsf_v_fail(c, what, strOfs, (uint32_t)s->num);
    }
    p = c->b + s->ofs + strOfs;
    remain = (size_t)((uint32_t)s->num - strOfs);
    if (memchr(p, 0, remain) == NULL) {
        return mp6_hsf_v_fail(c, "unterminated HSF string", strOfs,
                              (uint32_t)remain);
    }
    return 1;
}

/* Several retail authoring records retain MWCC/debug fill in name slots that
 * no runtime consumer uses.  Preserve those two explicit sentinels as NULL;
 * any other non-sentinel value still has to name a terminated table string. */
static int mp6_hsf_v_optional_string(MP6HsfPreflightCtx *c, uint32_t strOfs,
                                     const char *what)
{
    if (strOfs == UINT32_MAX || strOfs == UINT32_C(0xCCCCCCCC)) return 1;
    return mp6_hsf_v_string(c, strOfs, what);
}

static int mp6_hsf_v_symbol_slice(MP6HsfPreflightCtx *c, uint32_t idx,
                                  uint32_t count, const char *what)
{
    uint32_t total = (uint32_t)c->sec[MP6_HSF_SEC_SYMBOL].num;
    if (idx > total || count > total - idx) {
        return mp6_hsf_v_fail(c, what, idx, count);
    }
    return 1;
}

static uint32_t mp6_hsf_v_sym(const MP6HsfPreflightCtx *c, uint32_t idx)
{
    return mp6_hsf_v_u32(c->b + c->sec[MP6_HSF_SEC_SYMBOL].ofs
                         + (size_t)idx * 4u);
}

static int mp6_hsf_v_bitmap_bytes(uint8_t fmt, uint8_t pix, uint16_t w,
                                  uint16_t h, uint64_t *naiveOut,
                                  uint64_t *tileOut)
{
    uint64_t naive = (uint64_t)w * (uint64_t)h * (uint64_t)pix / 8u;
    uint32_t tw = 0, th = 0, tb = 0;
    uint64_t tile;
    if (w == 0u || h == 0u) return 0;
    switch (fmt) {
    case 0:
        if (pix != 4u) return 0;
        tw = 8; th = 8; tb = 32;
        break;
    case 1:
    case 2:
        if (pix != 8u) return 0;
        tw = 8; th = 4; tb = 32;
        break;
    case 3:
    case 4:
    case 5:
        if (pix != 16u) return 0;
        tw = 4; th = 4; tb = 32;
        break;
    case 6:
        if (pix != 32u) return 0;
        tw = 4; th = 4; tb = 64;
        break;
    case 7:
        /* `pixSize` is retained authoring-source depth for CMPR, not the
         * encoded GX depth.  Retail uses canonical 4 plus RGB24/RGBA32. */
        if (pix != 4u && pix != 24u && pix != 32u) return 0;
        tw = 8; th = 8; tb = 32;
        break;
    case 9:
    case 10:
    case 11:
        if (pix == 4u)      { tw = 8; th = 8; tb = 32; }
        else if (pix == 8u) { tw = 8; th = 4; tb = 32; }
        else return 0;
        break;
    default:
        return 0;
    }
    tile = (((uint64_t)w + tw - 1u) / tw)
         * (((uint64_t)h + th - 1u) / th) * tb;
    *naiveOut = naive;
    *tileOut = tile;
    return tile <= (uint64_t)INT_MAX;
}

static int32_t mp6_hsf_v_buffer_count(const MP6HsfPreflightCtx *c,
                                      unsigned secId, int32_t id)
{
    const MP6HsfPreflightSection *s = &c->sec[secId];
    if (id < 0 || id >= s->num) return -1;
    return mp6_hsf_v_s32(c->b + s->ofs + (size_t)id * 12u + 4u);
}

static float mp6_hsf_v_f32(const uint8_t *p)
{
    uint32_t bits = mp6_hsf_v_u32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int mp6_hsf_v_face_index(MP6HsfPreflightCtx *c, const uint8_t *p,
                                int32_t vertexCount, int32_t normalCount,
                                int32_t stCount, int32_t colorCount,
                                uint32_t owner, uint32_t corner)
{
    int32_t vertex = mp6_hsf_v_s16(p);
    int32_t normal = mp6_hsf_v_s16(p + 2);
    int32_t color = mp6_hsf_v_s16(p + 4);
    int32_t st = mp6_hsf_v_s16(p + 6);
    /* A face corner existing at all requires a non-empty vertex AND normal
     * buffer -- there is nothing to clamp such a corner TO, so that stays a
     * hard reject. */
    if (vertexCount <= 0 || normalCount <= 0) {
        return mp6_hsf_v_fail(c, "face index outside mesh buffer", owner, corner);
    }
    /* An out-of-range corner VALUE into a non-empty (extent-proved) buffer
     * is only COUNTED: the loader clamps it into range, rendering the
     * affected primitive degenerate -- the closest safe equivalent of the
     * retail loader's silent overread.  Rejecting the whole model here
     * erased the Boo shopkeeper (see MP6HsfValidated.oobFaceCorners). */
    if (vertex < 0 || vertex >= vertexCount
            || normal < 0 || normal >= normalCount
            || color < -1 || color >= colorCount
            || st < -1 || st >= stCount) {
        c->oobFaceCorners++;
    }
    return 1;
}

static int mp6_hsf_v_faces_for_mesh(MP6HsfPreflightCtx *c, int32_t faceId,
                                    int32_t vertexCount, int32_t normalCount,
                                    int32_t stCount, int32_t colorCount,
                                    int32_t materialCount, uint32_t owner)
{
    const MP6HsfPreflightSection *s = &c->sec[MP6_HSF_SEC_FACE];
    uint64_t pool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 12u;
    const uint8_t *last;
    int32_t lastCount;
    uint32_t lastOfs;
    uint64_t stripPool;
    const uint8_t *rec;
    int32_t count;
    uint32_t dataOfs;
    uint64_t group;
    int32_t j;
    if (faceId < 0 || faceId >= s->num || s->num <= 0) {
        return mp6_hsf_v_fail(c, "mesh has no valid face group", owner,
                              (uint32_t)faceId);
    }
    last = c->b + s->ofs + (size_t)(s->num - 1) * 12u;
    lastCount = mp6_hsf_v_s32(last + 4);
    lastOfs = mp6_hsf_v_u32(last + 8);
    stripPool = pool + lastOfs + (uint64_t)(uint32_t)lastCount * 48u;
    rec = c->b + s->ofs + (size_t)faceId * 12u;
    count = mp6_hsf_v_s32(rec + 4);
    dataOfs = mp6_hsf_v_u32(rec + 8);
    group = pool + dataOfs;
    for (j = 0; j < count; ++j) {
        const uint8_t *f = c->b + group + (size_t)j * 48u;
        uint32_t kind = mp6_hsf_v_u16(f) & 7u;
        int32_t material = mp6_hsf_v_s16(f + 2) & 0x0FFF;
        int baseCorners = kind == 2u ? 3 : 4;
        int k;
        if (material < 0 || material >= materialCount) {
            return mp6_hsf_v_fail(c, "face material outside section", owner,
                                  (uint32_t)j);
        }
        if (kind == 4u) baseCorners = 3;
        if (!mp6_hsf_v_work(c, (uint32_t)baseCorners)) return 0;
        for (k = 0; k < baseCorners; ++k) {
            if (!mp6_hsf_v_face_index(c, f + 4u + (size_t)k * 8u,
                                      vertexCount, normalCount, stCount,
                                      colorCount, owner, (uint32_t)k)) return 0;
        }
        if (kind == 4u) {
            int32_t stripCount = mp6_hsf_v_s32(f + 28);
            uint32_t stripIdx = mp6_hsf_v_u32(f + 32);
            const uint8_t *sp = c->b + stripPool + (uint64_t)stripIdx * 8u;
            if (!mp6_hsf_v_work(c, (uint32_t)stripCount)) return 0;
            for (k = 0; k < stripCount; ++k) {
                if (!mp6_hsf_v_face_index(c, sp + (size_t)k * 8u,
                                          vertexCount, normalCount, stCount,
                                          colorCount, owner,
                                          (uint32_t)(baseCorners + k))) return 0;
            }
        }
    }
    return 1;
}

static int mp6_hsf_v_cenv_for_mesh(MP6HsfPreflightCtx *c,
                                   uint32_t first, uint32_t count,
                                   uint32_t vertexCount, uint32_t normalCount,
                                   uint32_t matrixCount, uint32_t owner)
{
    const MP6HsfPreflightSection *s = &c->sec[MP6_HSF_SEC_CENV];
    uint64_t dataPool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 36u;
    uint64_t weightBytes = 0;
    uint64_t weightPool;
    uint32_t ci;
    for (ci = 0; ci < (uint32_t)s->num; ++ci) {
        const uint8_t *r = c->b + s->ofs + (size_t)ci * 36u;
        weightBytes += (uint64_t)mp6_hsf_v_u32(r + 16) * 12u;
        weightBytes += (uint64_t)mp6_hsf_v_u32(r + 20) * 16u;
        weightBytes += (uint64_t)mp6_hsf_v_u32(r + 24) * 16u;
    }
    weightPool = dataPool + weightBytes;
    for (ci = first; ci < first + count; ++ci) {
        const uint8_t *r = c->b + s->ofs + (size_t)ci * 36u;
        uint32_t sc = mp6_hsf_v_u32(r + 16);
        uint32_t dc = mp6_hsf_v_u32(r + 20);
        uint32_t mc = mp6_hsf_v_u32(r + 24);
        uint32_t so = mp6_hsf_v_u32(r + 4);
        uint32_t d_o = mp6_hsf_v_u32(r + 8);
        uint32_t mo = mp6_hsf_v_u32(r + 12);
        uint64_t copyEnd = (uint64_t)mp6_hsf_v_u32(r + 28)
                         + (uint64_t)mp6_hsf_v_u32(r + 32);
        uint32_t k;
        if (copyEnd > vertexCount) {
            return mp6_hsf_v_fail(c, "cenv copy span outside vertex buffer", owner, ci);
        }
        if (!mp6_hsf_v_work(c, (uint64_t)sc + dc + mc)) return 0;
        for (k = 0; k < sc; ++k) {
            const uint8_t *z = c->b + dataPool + so + (size_t)k * 12u;
            uint32_t target = mp6_hsf_v_u32(z);
            uint32_t pos = mp6_hsf_v_u16(z + 4);
            uint32_t posNum = mp6_hsf_v_u16(z + 6);
            uint32_t normal = mp6_hsf_v_u16(z + 8);
            uint32_t normalNum = mp6_hsf_v_u16(z + 10);
            if (target >= matrixCount || (uint64_t)pos + posNum > vertexCount
                    || (uint64_t)normal + normalNum > normalCount) {
                return mp6_hsf_v_fail(c, "cenv single reference outside mesh", owner, ci);
            }
        }
        for (k = 0; k < dc; ++k) {
            const uint8_t *z = c->b + dataPool + d_o + (size_t)k * 16u;
            uint32_t wn = mp6_hsf_v_u32(z + 8);
            uint32_t wo = mp6_hsf_v_u32(z + 12);
            uint32_t q;
            if (mp6_hsf_v_u32(z) >= matrixCount || mp6_hsf_v_u32(z + 4) >= matrixCount) {
                return mp6_hsf_v_fail(c, "cenv dual target outside matrix palette", owner, ci);
            }
            if (!mp6_hsf_v_work(c, wn)) return 0;
            for (q = 0; q < wn; ++q) {
                const uint8_t *w = c->b + weightPool + wo + (size_t)q * 12u;
                uint32_t pos = mp6_hsf_v_u16(w + 4);
                uint32_t posNum = mp6_hsf_v_u16(w + 6);
                uint32_t normal = mp6_hsf_v_u16(w + 8);
                uint32_t normalNum = mp6_hsf_v_u16(w + 10);
                if ((uint64_t)pos + posNum > vertexCount
                        || (uint64_t)normal + normalNum > normalCount) {
                    return mp6_hsf_v_fail(c, "cenv dual span outside mesh", owner, ci);
                }
            }
        }
        for (k = 0; k < mc; ++k) {
            const uint8_t *z = c->b + dataPool + mo + (size_t)k * 16u;
            uint32_t wn = mp6_hsf_v_u32(z);
            uint32_t wo = mp6_hsf_v_u32(z + 12);
            uint32_t q;
            if (mp6_hsf_v_u16(z + 4) >= vertexCount
                    || mp6_hsf_v_u16(z + 8) >= normalCount) {
                return mp6_hsf_v_fail(c, "cenv multi position outside mesh", owner, ci);
            }
            if (!mp6_hsf_v_work(c, wn)) return 0;
            for (q = 0; q < wn; ++q) {
                if (mp6_hsf_v_u32(c->b + weightPool + wo + (size_t)q * 8u)
                        >= matrixCount) {
                    return mp6_hsf_v_fail(c, "cenv multi target outside matrix palette", owner, ci);
                }
            }
        }
    }
    return 1;
}

/* Validate the exact byte graph consumed by hsf_load_native.c.  `capacity`
 * is the verified live allocation extent.  The HSF string table is the final
 * serialized section in every retail model and carries its exact byte count;
 * its checked end therefore narrows allocator padding to the logical file
 * length. */
static int mp6_hsf_preflight(const void *data, size_t capacity,
                             MP6HsfValidated *validated,
                             char *why, size_t whyCap)
{
    static const uint16_t stride[MP6_HSF_SEC_COUNT] = {
        16, 12, 60, 132, 12, 12, 12, 12, 324, 32, 16, 16,
        36, 40, 12, 160, 12, 24, 12, 4, 1
    };
    MP6HsfPreflightCtx c;
    uint64_t logical64;
    int i;

    memset(&c, 0, sizeof(c));
    c.b = (const uint8_t *)data;
    c.why = why;
    c.whyCap = whyCap;
    if (why != NULL && whyCap != 0) why[0] = '\0';
    if (validated != NULL) memset(validated, 0, sizeof(*validated));
    if (data == NULL || capacity < 176u) {
        c.n = capacity;
        return mp6_hsf_v_fail(&c, "HSF header outside verified allocation",
                              (uint32_t)capacity, 176);
    }
    if (memcmp(c.b, "HSF", 3) != 0) {
        c.n = capacity;
        return mp6_hsf_v_fail(&c, "invalid HSF magic", 0, 0);
    }
    for (i = 0; i < MP6_HSF_SEC_COUNT; ++i) {
        size_t o = 8u + (size_t)i * 8u;
        c.sec[i].ofs = mp6_hsf_v_u32(c.b + o);
        c.sec[i].num = mp6_hsf_v_s32(c.b + o + 4u);
        if (c.sec[i].num < 0) {
            c.n = capacity;
            return mp6_hsf_v_fail(&c, "negative HSF section count",
                                  (uint32_t)i, (uint32_t)c.sec[i].num);
        }
        if (i != MP6_HSF_SEC_SYMBOL && i != MP6_HSF_SEC_STRING
                && c.sec[i].num > INT16_MAX) {
            c.n = capacity;
            return mp6_hsf_v_fail(&c, "HSF section count exceeds s16 runtime field",
                                  (uint32_t)i, (uint32_t)c.sec[i].num);
        }
    }
    if (c.sec[MP6_HSF_SEC_STRING].num == 0) {
        /* Three retail entries are the canonical 176-byte empty HSF.  With
         * no other records there can be no string dereference; accept them
         * as an inert model without manufacturing a StringTable pointer. */
        for (i = 0; i < MP6_HSF_SEC_COUNT; ++i) {
            if (c.sec[i].num != 0) {
                c.n = capacity;
                return mp6_hsf_v_fail(&c,
                    "non-empty HSF has no string table", (uint32_t)i,
                    (uint32_t)c.sec[i].num);
            }
        }
        if (validated != NULL) validated->logicalSize = 176u;
        return 1;
    }
    logical64 = (uint64_t)c.sec[MP6_HSF_SEC_STRING].ofs
              + (uint64_t)(uint32_t)c.sec[MP6_HSF_SEC_STRING].num;
    if (logical64 < 176u || logical64 > (uint64_t)capacity
            || logical64 > (uint64_t)SIZE_MAX) {
        c.n = capacity;
        return mp6_hsf_v_fail(&c, "HSF logical length outside allocation",
                              c.sec[MP6_HSF_SEC_STRING].ofs,
                              (uint32_t)c.sec[MP6_HSF_SEC_STRING].num);
    }
    c.n = (size_t)logical64;
    c.workLimit = (uint64_t)c.n * 8u + 65536u;
    if (!mp6_hsf_v_range(&c, c.sec[MP6_HSF_SEC_STRING].ofs,
                         (uint32_t)c.sec[MP6_HSF_SEC_STRING].num)
            || c.b[c.n - 1u] != 0) {
        return mp6_hsf_v_fail(&c, "invalid HSF string table extent", 0, 0);
    }

    for (i = 0; i < MP6_HSF_SEC_COUNT; ++i) {
        uint64_t bytes;
        if (c.sec[i].num == 0) continue;
        bytes = (uint64_t)(uint32_t)c.sec[i].num * stride[i];
        if (!mp6_hsf_v_range(&c, c.sec[i].ofs, bytes)) {
            return mp6_hsf_v_fail(&c, "HSF section headers outside file",
                                  (uint32_t)i, (uint32_t)c.sec[i].num);
        }
        if (!mp6_hsf_v_work(&c, (uint32_t)c.sec[i].num)) return 0;
    }
    if (c.sec[MP6_HSF_SEC_MATRIX].num > 1) {
        return mp6_hsf_v_fail(&c, "matrix section has multiple headers",
                              (uint32_t)c.sec[MP6_HSF_SEC_MATRIX].num, 1);
    }

    /* Every section-owned name. */
#define MP6_HSF_CHECK_NAMES(secId, recStride, fieldOfs, optional) do {       \
        int32_t mp6NameI;                                                     \
        const MP6HsfPreflightSection *mp6NameS = &c.sec[(secId)];             \
        for (mp6NameI = 0; mp6NameI < mp6NameS->num; ++mp6NameI) {            \
            const uint8_t *mp6NameR = c.b + mp6NameS->ofs                     \
                + (size_t)mp6NameI * (recStride);                             \
            uint32_t mp6Name = mp6_hsf_v_u32(mp6NameR + (fieldOfs));         \
            if ((optional) ? !mp6_hsf_v_optional_string(                     \
                    &c, mp6Name, "HSF name outside string table")            \
                           : !mp6_hsf_v_string(                               \
                    &c, mp6Name, "HSF name outside string table")) return 0;  \
        }                                                                      \
    } while (0)
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_PALETTE, 16, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_BITMAP, 32, 0, 0);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_MATERIAL, 60, 0, 0);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_VERTEX, 12, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_NORMAL, 12, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_ST, 12, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_COLOR, 12, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_FACE, 12, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_OBJECT, 324, 0, 0);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_PART, 12, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_SHAPE, 12, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_CENV, 36, 0, 1);
    MP6_HSF_CHECK_NAMES(MP6_HSF_SEC_SKELETON, 40, 0, 0);
#undef MP6_HSF_CHECK_NAMES

    /* Attribute names alone permit the format's -1 sentinel. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_ATTRIBUTE];
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 132u;
            uint32_t name = mp6_hsf_v_u32(r);
            int32_t bmp = mp6_hsf_v_s32(r + 128);
            if (name != UINT32_MAX
                    && !mp6_hsf_v_string(&c, name, "attribute name outside string table")) return 0;
            if (bmp < -1 || bmp >= c.sec[MP6_HSF_SEC_BITMAP].num) {
                return mp6_hsf_v_fail(&c, "attribute bitmap index outside section",
                                      (uint32_t)j, (uint32_t)bmp);
            }
        }
    }

    /* Palette record array followed by its raw u16 pool. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_PALETTE];
        uint64_t pool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 16u;
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 16u;
            int32_t count = mp6_hsf_v_s32(r + 8);
            uint32_t ofs = mp6_hsf_v_u32(r + 12);
            if (count < 0 || !mp6_hsf_v_nested_count(&c, (uint32_t)count, 2,
                                                     "palette allocation overflow")
                    || !mp6_hsf_v_range(&c, pool + ofs, (uint64_t)(uint32_t)count * 2u)) {
                return mp6_hsf_v_fail(&c, "palette data outside file",
                                      (uint32_t)j, (uint32_t)count);
            }
        }
    }

    /* Bitmap headers + pixel pool.  Retail has two authoring-tool files
     * whose final GX tile is physically truncated; the original naive
     * w*h*bpp bytes are present.  Require that safe prefix and allow only
     * the tile-alignment tail to be zero-padded by the loader. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_BITMAP];
        uint64_t pool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 32u;
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 32u;
            int16_t w = (int16_t)mp6_hsf_v_u16(r + 10);
            int16_t h = (int16_t)mp6_hsf_v_u16(r + 12);
            int16_t palSize = (int16_t)mp6_hsf_v_u16(r + 14);
            int32_t pal = mp6_hsf_v_s32(r + 20);
            uint32_t dataOfs = mp6_hsf_v_u32(r + 28);
            uint64_t naive = 0, tile = 0;
            int indexed = r[8] == 9u || r[8] == 10u || r[8] == 11u;
            if (w <= 0 || h <= 0 || palSize < 0 || pal < -1
                    || pal >= c.sec[MP6_HSF_SEC_PALETTE].num
                    || (indexed && (pal < 0 || palSize <= 0))
                    || (!indexed && (pal != -1 || palSize != 0))
                    || !mp6_hsf_v_bitmap_bytes(r[8], r[9], (uint16_t)w,
                                               (uint16_t)h, &naive, &tile)
                    || !mp6_hsf_v_range(&c, pool + dataOfs,
                                        naive < tile ? naive : tile)
                    || !mp6_hsf_v_nested_count(&c, (uint32_t)tile, 1,
                                               "bitmap allocation overflow")) {
                return mp6_hsf_v_fail(&c, "invalid bitmap dimensions/data",
                                      (uint32_t)j, (uint32_t)tile);
            }
            if (indexed) {
                const MP6HsfPreflightSection *ps =
                    &c.sec[MP6_HSF_SEC_PALETTE];
                const uint8_t *pr = c.b + ps->ofs + (size_t)pal * 16u;
                int32_t paletteCount = mp6_hsf_v_s32(pr + 8);
                uint32_t paletteOfs = mp6_hsf_v_u32(pr + 12);
                uint32_t required = (uint32_t)palSize;
                uint64_t palettePool = (uint64_t)ps->ofs
                                     + (uint64_t)(uint32_t)ps->num * 16u;
                if (paletteCount < palSize) {
                    return mp6_hsf_v_fail(&c,
                        "bitmap palette shorter than declared size",
                        (uint32_t)j, (uint32_t)paletteCount);
                }
                /* CI_IA8 carries two TLUT banks in the raw palette pool.
                 * LoadTexture selects the second at the next 16-entry
                 * boundary when HSF_TEXID_TL32 is set. */
                if (r[8] == 11u) {
                    required = ((required + 15u) & ~15u) + required;
                }
                if (!mp6_hsf_v_nested_count(&c, required, 2,
                                             "bitmap palette allocation overflow")
                        || !mp6_hsf_v_range(&c, palettePool + paletteOfs,
                                            (uint64_t)required * 2u)) {
                    return mp6_hsf_v_fail(&c,
                        "bitmap palette banks outside file",
                        (uint32_t)j, required);
                }
            }
        }
    }

    /* Materials reference attribute indices through the symbol pool. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_MATERIAL];
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 60u;
            uint32_t count = mp6_hsf_v_u32(r + 52);
            uint32_t idx = mp6_hsf_v_u32(r + 56);
            uint32_t k;
            if (!mp6_hsf_v_nested_count(&c, count, 4, "material attr allocation overflow")
                    || !mp6_hsf_v_symbol_slice(&c, idx, count,
                                               "material attr symbol slice outside pool")) return 0;
            for (k = 0; k < count; ++k) {
                if (mp6_hsf_v_sym(&c, idx + k)
                        >= (uint32_t)c.sec[MP6_HSF_SEC_ATTRIBUTE].num) {
                    return mp6_hsf_v_fail(&c, "material attribute index outside section", j, k);
                }
            }
        }
    }

    /* Vertex/normal/ST/color shared pools. */
    {
        static const uint8_t ids[4] = {
            MP6_HSF_SEC_VERTEX, MP6_HSF_SEC_NORMAL,
            MP6_HSF_SEC_ST, MP6_HSF_SEC_COLOR
        };
        int q;
        for (q = 0; q < 4; ++q) {
            const MP6HsfPreflightSection *s = &c.sec[ids[q]];
            uint32_t elem = ids[q] == MP6_HSF_SEC_VERTEX ? 12u
                          : ids[q] == MP6_HSF_SEC_NORMAL
                                ? (c.sec[MP6_HSF_SEC_CENV].num > 0 ? 12u : 3u)
                          : ids[q] == MP6_HSF_SEC_ST ? 8u : 4u;
            uint64_t pool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 12u;
            int32_t j;
            for (j = 0; j < s->num; ++j) {
                const uint8_t *r = c.b + s->ofs + (size_t)j * 12u;
                int32_t count = mp6_hsf_v_s32(r + 4);
                uint32_t ofs = mp6_hsf_v_u32(r + 8);
                uint32_t nativeElem = ids[q] == MP6_HSF_SEC_NORMAL
                                      && c.sec[MP6_HSF_SEC_CENV].num == 0 ? 3u : elem;
                if (count < 0
                        || !mp6_hsf_v_nested_count(&c, (uint32_t)count, nativeElem,
                                                   "HSF buffer allocation overflow")
                        || !mp6_hsf_v_range(&c, pool + ofs,
                                            (uint64_t)(uint32_t)count * elem)) {
                    return mp6_hsf_v_fail(&c, "HSF buffer data outside file",
                                          ids[q], (uint32_t)j);
                }
            }
        }
    }

    /* Face records and their section-global strip-index pool. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_FACE];
        if (s->num > 0) {
            uint64_t pool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 12u;
            const uint8_t *last = c.b + s->ofs + (size_t)(s->num - 1) * 12u;
            int32_t lastCount = mp6_hsf_v_s32(last + 4);
            uint32_t lastOfs = mp6_hsf_v_u32(last + 8);
            uint64_t stripPool;
            int32_t j;
            if (lastCount < 0 || !mp6_hsf_v_range(&c, pool + lastOfs,
                                                   (uint64_t)(uint32_t)lastCount * 48u)) {
                return mp6_hsf_v_fail(&c, "last face group outside file", 0,
                                      (uint32_t)lastCount);
            }
            stripPool = pool + lastOfs + (uint64_t)(uint32_t)lastCount * 48u;
            for (j = 0; j < s->num; ++j) {
                const uint8_t *r = c.b + s->ofs + (size_t)j * 12u;
                int32_t count = mp6_hsf_v_s32(r + 4);
                uint32_t ofs = mp6_hsf_v_u32(r + 8);
                uint64_t group = pool + ofs;
                int32_t k;
                if (count < 0
                        || !mp6_hsf_v_nested_count(&c, (uint32_t)count, 64,
                                                   "face allocation overflow")
                        || !mp6_hsf_v_range(&c, group,
                                            (uint64_t)(uint32_t)count * 48u)) {
                    return mp6_hsf_v_fail(&c, "face group outside file", j,
                                          (uint32_t)count);
                }
                for (k = 0; k < count; ++k) {
                    const uint8_t *f = c.b + group + (size_t)k * 48u;
                    if ((mp6_hsf_v_u16(f) & 7u) == 4u) {
                        int32_t stripCount = mp6_hsf_v_s32(f + 28);
                        uint32_t stripIdx = mp6_hsf_v_u32(f + 32);
                        if (stripCount < 0
                                || !mp6_hsf_v_nested_count(&c, (uint32_t)stripCount, 8,
                                                           "strip allocation overflow")
                                || !mp6_hsf_v_range(&c,
                                    stripPool + (uint64_t)stripIdx * 8u,
                                    (uint64_t)(uint32_t)stripCount * 8u)) {
                            return mp6_hsf_v_fail(&c, "face strip outside file", j, k);
                        }
                    }
                }
            }
        }
    }

    /* Object child lists and cross-section indices. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_OBJECT];
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 324u;
            const uint8_t *m = r + 16;
            uint32_t type = mp6_hsf_v_u32(r + 4);
            if (type != 7u && type != 8u) {
                uint32_t count = mp6_hsf_v_u32(m + 4);
                uint32_t idx = mp6_hsf_v_u32(m + 8);
                uint32_t k;
                if (!mp6_hsf_v_nested_count(&c, count, (uint32_t)sizeof(void *),
                                             "object child allocation overflow")
                        || !mp6_hsf_v_symbol_slice(&c, idx, count,
                                                   "object child symbol slice outside pool")) return 0;
                for (k = 0; k < count; ++k) {
                    uint32_t child = mp6_hsf_v_sym(&c, idx + k);
                    uint32_t q;
                    if (child >= (uint32_t)s->num) {
                        return mp6_hsf_v_fail(&c, "child object index outside section", j, k);
                    }
                    if (!mp6_hsf_v_work(&c, k)) return 0;
                    for (q = 0; q < k; ++q) {
                        if (mp6_hsf_v_sym(&c, idx + q) == child) {
                            return mp6_hsf_v_fail(&c, "duplicate child object reference", j, child);
                        }
                    }
                    {
                        const uint8_t *childRec = c.b + s->ofs + (size_t)child * 324u;
                        uint32_t childType = mp6_hsf_v_u32(childRec + 4);
                        if (childType != 7u && childType != 8u
                                && mp6_hsf_v_s32(childRec + 16) != j) {
                            return mp6_hsf_v_fail(&c,
                                "child list disagrees with raw parent", j, child);
                        }
                    }
                }
                if (type == 1u && mp6_hsf_v_u32(m + 84) >= (uint32_t)s->num) {
                    return mp6_hsf_v_fail(&c, "replica object index outside section", j, 0);
                }
                if (type == 2u) {
                    static const uint16_t refOfs[5] = { 248, 252, 260, 256, 244 };
                    static const uint8_t refSec[5] = {
                        MP6_HSF_SEC_VERTEX, MP6_HSF_SEC_NORMAL, MP6_HSF_SEC_ST,
                        MP6_HSF_SEC_COLOR, MP6_HSF_SEC_FACE
                    };
                    int q;
                    for (q = 0; q < 5; ++q) {
                        int32_t id = mp6_hsf_v_s32(m + refOfs[q]);
                        if (id < -1 || id >= c.sec[refSec[q]].num) {
                            return mp6_hsf_v_fail(&c, "mesh buffer index outside section", j, q);
                        }
                    }
                    if (c.sec[MP6_HSF_SEC_CENV].num > 0) {
                        uint32_t cn = mp6_hsf_v_u32(m + 292);
                        int32_t ci = mp6_hsf_v_s32(m + 296);
                        if ((cn == 0 && ci < -1)
                                || (cn > 0 && (ci < 0
                                    || (uint32_t)ci > (uint32_t)c.sec[MP6_HSF_SEC_CENV].num
                                    || cn > (uint32_t)c.sec[MP6_HSF_SEC_CENV].num - (uint32_t)ci))) {
                            return mp6_hsf_v_fail(&c, "mesh cenv slice outside section", j, cn);
                        }
                    }
                }
            }
        }
    }

    /* Prove that the recursively-consumed object hierarchy is one finite
     * tree.  Cameras/lights carry a different union layout and are leaves;
     * the two retail light-only assets legitimately use object[0] as the
     * loader's fallback root. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_OBJECT];
        int32_t graphObjects = 0;
        int32_t root = -1;
        int32_t roots = 0;
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 324u;
            uint32_t type = mp6_hsf_v_u32(r + 4);
            int32_t parent;
            if (type == 7u || type == 8u) continue;
            ++graphObjects;
            parent = mp6_hsf_v_s32(r + 16);
            if (parent == -1) { root = j; ++roots; }
            else if (parent < 0 || parent >= s->num) {
                return mp6_hsf_v_fail(&c, "object parent outside section", j,
                                      (uint32_t)parent);
            }
        }
        if (graphObjects != 0 && roots != 1) {
            return mp6_hsf_v_fail(&c, "object graph must have exactly one root",
                                  (uint32_t)graphObjects, (uint32_t)roots);
        }
        for (j = 0; j < s->num && graphObjects != 0; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 324u;
            int32_t cur = j;
            int32_t steps = 0;
            if (mp6_hsf_v_u32(r + 4) == 7u || mp6_hsf_v_u32(r + 4) == 8u) continue;
            while (cur != root) {
                const uint8_t *cr;
                int32_t parent;
                if (++steps > s->num || steps > 1024 || !mp6_hsf_v_work(&c, 1)) {
                    return mp6_hsf_v_fail(&c, "cycle in object hierarchy", j,
                                          (uint32_t)cur);
                }
                cr = c.b + s->ofs + (size_t)cur * 324u;
                parent = mp6_hsf_v_s32(cr + 16);
                if (parent < 0 || parent >= s->num) {
                    return mp6_hsf_v_fail(&c, "object is unreachable from root", j,
                                          (uint32_t)parent);
                }
                cr = c.b + s->ofs + (size_t)parent * 324u;
                if (mp6_hsf_v_u32(cr + 4) == 7u || mp6_hsf_v_u32(cr + 4) == 8u) {
                    return mp6_hsf_v_fail(&c, "camera/light used as object parent", j,
                                          (uint32_t)parent);
                }
                cur = parent;
            }
        }
    }

    /* Part u16 pools. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_PART];
        uint64_t pool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 12u;
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 12u;
            uint32_t count = mp6_hsf_v_u32(r + 4);
            uint32_t idx = mp6_hsf_v_u32(r + 8);
            if (!mp6_hsf_v_nested_count(&c, count, 2, "part allocation overflow")
                    || !mp6_hsf_v_range(&c, pool + (uint64_t)idx * 2u,
                                        (uint64_t)count * 2u)) {
                return mp6_hsf_v_fail(&c, "part vertex pool slice outside file", j, count);
            }
        }
    }

    /* Cluster names/part/vertex-symbol references. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_CLUSTER];
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 160u;
            int32_t part = mp6_hsf_v_s32(r + 12);
            uint32_t count = mp6_hsf_v_u32(r + 152);
            uint32_t idx = mp6_hsf_v_u32(r + 156);
            float initialIndex = mp6_hsf_v_f32(r + 16);
            uint32_t k;
            if (!mp6_hsf_v_string(&c, mp6_hsf_v_u32(r), "cluster name[0] outside strings")
                    || !mp6_hsf_v_string(&c, mp6_hsf_v_u32(r + 4), "cluster name[1] outside strings")
                    || !mp6_hsf_v_string(&c, mp6_hsf_v_u32(r + 8), "cluster target outside strings")) return 0;
            if (count > 32u || part < -1 || part >= c.sec[MP6_HSF_SEC_PART].num
                    || (count > 0u && part < 0)
                    || (count > 0u && !(initialIndex >= 0.0f
                                         && initialIndex < (float)count))
                    || !mp6_hsf_v_nested_count(&c, count, (uint32_t)sizeof(void *),
                                               "cluster vertex allocation overflow")
                    || !mp6_hsf_v_symbol_slice(&c, idx, count,
                                               "cluster vertex symbols outside pool")) {
                return mp6_hsf_v_fail(&c, "invalid cluster references", j, count);
            }
            for (k = 0; k < count; ++k) {
                uint32_t vertex = mp6_hsf_v_sym(&c, idx + k);
                if (vertex >= (uint32_t)c.sec[MP6_HSF_SEC_VERTEX].num) {
                    return mp6_hsf_v_fail(&c, "cluster vertex index outside section", j, k);
                }
                if (part >= 0) {
                    const uint8_t *partRec = c.b + c.sec[MP6_HSF_SEC_PART].ofs
                                           + (size_t)part * 12u;
                    uint32_t partCount = mp6_hsf_v_u32(partRec + 4);
                    if ((uint32_t)mp6_hsf_v_buffer_count(
                            &c, MP6_HSF_SEC_VERTEX, (int32_t)vertex) < partCount) {
                        return mp6_hsf_v_fail(&c,
                            "cluster source vertex buffer shorter than part", j, k);
                    }
                }
            }
            if (count > 0u && part >= 0) {
                const uint8_t *targetName = c.b + c.sec[MP6_HSF_SEC_STRING].ofs
                                          + mp6_hsf_v_u32(r + 8);
                const MP6HsfPreflightSection *os = &c.sec[MP6_HSF_SEC_OBJECT];
                int32_t targetObject = -1;
                int32_t q;
                if (!mp6_hsf_v_work(&c, (uint32_t)os->num)) return 0;
                for (q = 0; q < os->num; ++q) {
                    const uint8_t *obj = c.b + os->ofs + (size_t)q * 324u;
                    const char *objName = (const char *)(c.b
                        + c.sec[MP6_HSF_SEC_STRING].ofs + mp6_hsf_v_u32(obj));
                    if (strcmp(objName, (const char *)targetName) == 0) {
                        targetObject = q;
                        break;
                    }
                }
                if (targetObject >= 0) {
                    const uint8_t *obj = c.b + os->ofs
                                       + (size_t)targetObject * 324u;
                    int32_t targetVertex;
                    int32_t targetCount;
                    const uint8_t *partRec;
                    uint32_t partCount, partIdx;
                    uint64_t partPool;
                    if (mp6_hsf_v_u32(obj + 4) != 2u) {
                        return mp6_hsf_v_fail(&c,
                            "cluster target is not a mesh", j,
                            (uint32_t)targetObject);
                    }
                    targetVertex = mp6_hsf_v_s32(obj + 16u + 248u);
                    targetCount = mp6_hsf_v_buffer_count(
                        &c, MP6_HSF_SEC_VERTEX, targetVertex);
                    partRec = c.b + c.sec[MP6_HSF_SEC_PART].ofs
                            + (size_t)part * 12u;
                    partCount = mp6_hsf_v_u32(partRec + 4);
                    partIdx = mp6_hsf_v_u32(partRec + 8);
                    partPool = (uint64_t)c.sec[MP6_HSF_SEC_PART].ofs
                             + (uint64_t)(uint32_t)c.sec[MP6_HSF_SEC_PART].num * 12u;
                    if (!mp6_hsf_v_work(&c, partCount)) return 0;
                    for (k = 0; k < partCount; ++k) {
                        uint32_t vertexIndex = mp6_hsf_v_u16(
                            c.b + partPool + ((uint64_t)partIdx + k) * 2u);
                        if (targetCount < 0 || vertexIndex >= (uint32_t)targetCount) {
                            return mp6_hsf_v_fail(&c,
                                "cluster part index outside target mesh", j, k);
                        }
                    }
                }
            }
        }
    }

    /* Shape vertex-symbol references. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_SHAPE];
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 12u;
            uint32_t count = mp6_hsf_v_u16(r + 6);
            uint32_t idx = mp6_hsf_v_u32(r + 8);
            if (!mp6_hsf_v_nested_count(&c, count, (uint32_t)sizeof(void *),
                                         "shape vertex allocation overflow")
                    || !mp6_hsf_v_symbol_slice(&c, idx, count,
                                               "shape vertex symbols outside pool")) return 0;
            /* This is the file-level SHAPE SECTION's own vertex slice, which
             * has no runtime consumer: some retail shape records put a
             * different/one-based authoring ID here (e.g. [1..5] for five
             * vertex buffers, and 10 in a one-buffer motion-only asset), so
             * LoadShapes bounds each entry to NULL and nothing dereferences
             * them.  The slice that IS dereferenced is the PER-MESH one
             * (HSF_MESH.shapeNum/.shape, mesh record +276/+280, i.e. object
             * record +292/+296), which
             * game/ShapeExec.c's SetShapeMain walks; it is a genuine
             * vertex-buffer index list and hsf_load_native.c's BindMeshShape
             * bounds-checks it there rather than here, so that a file with an
             * unbindable shape mesh still loads and simply draws that mesh's
             * static pose. */
        }
    }

    /* Cenv's header pool and nested weight pool. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_CENV];
        uint64_t dataPool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 36u;
        uint64_t weightBytes = 0;
        uint64_t weightPool;
        int32_t j;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 36u;
            weightBytes += (uint64_t)mp6_hsf_v_u32(r + 16) * 12u;
            weightBytes += (uint64_t)mp6_hsf_v_u32(r + 20) * 16u;
            weightBytes += (uint64_t)mp6_hsf_v_u32(r + 24) * 16u;
            if (weightBytes > (uint64_t)c.n) {
                return mp6_hsf_v_fail(&c, "cenv aggregate pool overflow", j, 0);
            }
        }
        if (!mp6_hsf_v_range(&c, dataPool, weightBytes)) {
            return mp6_hsf_v_fail(&c, "cenv data pool outside file", 0, 0);
        }
        weightPool = dataPool + weightBytes;
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 36u;
            uint32_t sc = mp6_hsf_v_u32(r + 16);
            uint32_t dc = mp6_hsf_v_u32(r + 20);
            uint32_t mc = mp6_hsf_v_u32(r + 24);
            uint32_t so = mp6_hsf_v_u32(r + 4);
            uint32_t d_o = mp6_hsf_v_u32(r + 8);
            uint32_t mo = mp6_hsf_v_u32(r + 12);
            uint32_t k;
            if (!mp6_hsf_v_nested_count(&c, sc, 12, "cenv single allocation overflow")
                    || !mp6_hsf_v_nested_count(&c, dc, 32, "cenv dual allocation overflow")
                    || !mp6_hsf_v_nested_count(&c, mc, 32, "cenv multi allocation overflow")
                    || !mp6_hsf_v_range(&c, dataPool + so, (uint64_t)sc * 12u)
                    || !mp6_hsf_v_range(&c, dataPool + d_o, (uint64_t)dc * 16u)
                    || !mp6_hsf_v_range(&c, dataPool + mo, (uint64_t)mc * 16u)) {
                return mp6_hsf_v_fail(&c, "cenv record slice outside file", j, 0);
            }
            for (k = 0; k < dc; ++k) {
                const uint8_t *d = c.b + dataPool + d_o + (size_t)k * 16u;
                uint32_t wn = mp6_hsf_v_u32(d + 8);
                uint32_t wo = mp6_hsf_v_u32(d + 12);
                if (!mp6_hsf_v_nested_count(&c, wn, 12, "dual weight allocation overflow")
                        || !mp6_hsf_v_range(&c, weightPool + wo, (uint64_t)wn * 12u)) {
                    return mp6_hsf_v_fail(&c, "dual weight slice outside file", j, k);
                }
            }
            for (k = 0; k < mc; ++k) {
                const uint8_t *m = c.b + dataPool + mo + (size_t)k * 16u;
                uint32_t wn = mp6_hsf_v_u32(m);
                uint32_t wo = mp6_hsf_v_u32(m + 12);
                if (!mp6_hsf_v_nested_count(&c, wn, 16, "multi weight allocation overflow")
                        || !mp6_hsf_v_range(&c, weightPool + wo, (uint64_t)wn * 8u)) {
                    return mp6_hsf_v_fail(&c, "multi weight slice outside file", j, k);
                }
            }
        }
    }

    /* MapAttr u16 pools. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_MAPATTR];
        uint64_t pool = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 24u;
        int32_t mapVertexCount = mp6_hsf_v_buffer_count(
            &c, MP6_HSF_SEC_VERTEX, 0);
        int32_t j;
        if (s->num > 0 && mapVertexCount < 0) {
            return mp6_hsf_v_fail(&c, "mapAttr model has no vertex buffer", 0, 0);
        }
        for (j = 0; j < s->num; ++j) {
            const uint8_t *r = c.b + s->ofs + (size_t)j * 24u;
            uint32_t idx = mp6_hsf_v_u32(r + 16);
            uint32_t count = mp6_hsf_v_u32(r + 20);
            if (!mp6_hsf_v_nested_count(&c, count, 2, "mapAttr allocation overflow")
                    || !mp6_hsf_v_range(&c, pool + (uint64_t)idx * 2u,
                                        (uint64_t)count * 2u)) {
                return mp6_hsf_v_fail(&c, "mapAttr data outside file", j, count);
            }
            if (count > 0) {
                const uint8_t *dataP = c.b + pool + (uint64_t)idx * 2u;
                uint32_t cursor = 0;
                while (cursor < count) {
                    uint16_t tag = mp6_hsf_v_u16(dataP + (size_t)cursor * 2u);
                    uint32_t kind = tag & 0xFFu;
                    uint32_t step = (tag & 0x8000u) ? kind + 1u
                                  : (kind >= 1u && kind <= 4u) ? kind + 1u : 1u;
                    uint32_t q;
                    if (step > count - cursor) {
                        return mp6_hsf_v_fail(&c,
                            "mapAttr packed polygon crosses dataLen", j, cursor);
                    }
                    if (!mp6_hsf_v_work(&c, step)) return 0;
                    for (q = 1; q < step; ++q) {
                        if (mp6_hsf_v_u16(dataP + (size_t)(cursor + q) * 2u)
                                >= (uint32_t)mapVertexCount) {
                            return mp6_hsf_v_fail(&c,
                                "mapAttr vertex index outside buffer", j, cursor + q);
                        }
                    }
                    cursor += step;
                }
            }
        }
    }

    /* One matrix header; its file matrices are placeholders, but the runtime
     * allocation formula still must not wrap. */
    if (c.sec[MP6_HSF_SEC_MATRIX].num == 1) {
        const uint8_t *r = c.b + c.sec[MP6_HSF_SEC_MATRIX].ofs;
        uint64_t base = mp6_hsf_v_u32(r);
        uint64_t count = mp6_hsf_v_u32(r + 4);
        uint64_t meshCount = 0;
        uint64_t objectCount;
        uint64_t paletteLen;
        const MP6HsfPreflightSection *os = &c.sec[MP6_HSF_SEC_OBJECT];
        int32_t j;
        objectCount = (uint32_t)os->num;
        for (j = 0; j < os->num; ++j) {
            if (mp6_hsf_v_u32(c.b + os->ofs + (size_t)j * 324u + 4u) == 2u) ++meshCount;
        }
        paletteLen = meshCount;
#define MP6_HSF_MAX_PALETTE(expr) do { uint64_t mp6PaletteNeed = (expr); \
            if (mp6PaletteNeed > paletteLen) paletteLen = mp6PaletteNeed; } while (0)
        MP6_HSF_MAX_PALETTE(base + objectCount);
        MP6_HSF_MAX_PALETTE(base + count * meshCount + objectCount);
        MP6_HSF_MAX_PALETTE(base + count * (meshCount + 1u));
#undef MP6_HSF_MAX_PALETTE
        if (paletteLen + 64u > (uint64_t)INT_MAX / 48u) {
            return mp6_hsf_v_fail(&c, "matrix palette allocation overflow",
                                  (uint32_t)base, (uint32_t)count);
        }
    }

    /* Prove every pointer/index MakeDisplayList and EnvelopeProc consume for
     * each mesh.  These consumers dereference face/material/vertex/normal
     * pointers without NULL checks, and the envelope path also indexes matrix
     * and vertex pools using nested cenv fields. */
    {
        const MP6HsfPreflightSection *os = &c.sec[MP6_HSF_SEC_OBJECT];
        uint32_t matrixCount = 0;
        int32_t j;
        if (c.sec[MP6_HSF_SEC_CENV].num > 0) {
            if (c.sec[MP6_HSF_SEC_MATRIX].num != 1 || os->num <= 0) {
                return mp6_hsf_v_fail(&c, "cenv model lacks object/matrix data", 0, 0);
            }
            matrixCount = mp6_hsf_v_u32(c.b + c.sec[MP6_HSF_SEC_MATRIX].ofs + 4u);
            if (matrixCount != (uint32_t)os->num) {
                return mp6_hsf_v_fail(&c, "cenv matrix count differs from object count",
                                      matrixCount, (uint32_t)os->num);
            }
        }
        for (j = 0; j < os->num; ++j) {
            const uint8_t *r = c.b + os->ofs + (size_t)j * 324u;
            const uint8_t *m = r + 16;
            int32_t vertexId, normalId, stId, colorId, faceId;
            int32_t vertexCount, normalCount, stCount, colorCount;
            if (mp6_hsf_v_u32(r + 4) != 2u) continue;
            vertexId = mp6_hsf_v_s32(m + 248);
            normalId = mp6_hsf_v_s32(m + 252);
            stId = mp6_hsf_v_s32(m + 260);
            colorId = mp6_hsf_v_s32(m + 256);
            faceId = mp6_hsf_v_s32(m + 244);
            vertexCount = mp6_hsf_v_buffer_count(&c, MP6_HSF_SEC_VERTEX, vertexId);
            normalCount = mp6_hsf_v_buffer_count(&c, MP6_HSF_SEC_NORMAL, normalId);
            stCount = stId >= 0
                    ? mp6_hsf_v_buffer_count(&c, MP6_HSF_SEC_ST, stId) : 0;
            colorCount = colorId >= 0
                       ? mp6_hsf_v_buffer_count(&c, MP6_HSF_SEC_COLOR, colorId) : 0;
            if (vertexCount < 0 || normalCount < 0
                    || c.sec[MP6_HSF_SEC_MATERIAL].num <= 0
                    || !mp6_hsf_v_faces_for_mesh(&c, faceId, vertexCount,
                                                 normalCount, stCount, colorCount,
                                                 c.sec[MP6_HSF_SEC_MATERIAL].num,
                                                 (uint32_t)j)) {
                return mp6_hsf_v_fail(&c, "mesh lacks valid draw buffers",
                                      (uint32_t)j, 0);
            }
            if (c.sec[MP6_HSF_SEC_CENV].num > 0) {
                uint32_t cn = mp6_hsf_v_u32(m + 292);
                int32_t ci = mp6_hsf_v_s32(m + 296);
                if (cn > 0 && !mp6_hsf_v_cenv_for_mesh(
                        &c, (uint32_t)ci, cn, (uint32_t)vertexCount,
                        (uint32_t)normalCount, matrixCount, (uint32_t)j)) return 0;
            }
        }
    }

    /* Embedded motion: first motion record, its contiguous tracks, then the
     * curve pool.  Standalone transform tracks with no object section retain
     * their raw target and therefore do not dereference that name. */
    {
        const MP6HsfPreflightSection *s = &c.sec[MP6_HSF_SEC_MOTION];
        if (s->num > 0) {
            const uint8_t *mot = c.b + s->ofs;
            int32_t tracks = mp6_hsf_v_s32(mot + 4);
            uint64_t trackBase = (uint64_t)s->ofs + (uint64_t)(uint32_t)s->num * 16u;
            uint64_t pool;
            int32_t j;
            if (tracks < 0 || tracks > INT16_MAX
                    || !mp6_hsf_v_nested_count(&c, (uint32_t)tracks, 32,
                                               "motion track allocation overflow")
                    || !mp6_hsf_v_range(&c, trackBase, (uint64_t)(uint32_t)tracks * 16u)) {
                return mp6_hsf_v_fail(&c, "motion track array outside file", 0,
                                      (uint32_t)tracks);
            }
            pool = trackBase + (uint64_t)(uint32_t)tracks * 16u;
            if (!mp6_hsf_v_string(&c, mp6_hsf_v_u32(mot),
                                  "motion name outside string table")) return 0;
            for (j = 0; j < tracks; ++j) {
                const uint8_t *t = c.b + trackBase + (size_t)j * 16u;
                uint8_t type = t[0];
                uint16_t target = mp6_hsf_v_u16(t + 2);
                uint16_t curve = mp6_hsf_v_u16(t + 8);
                uint32_t keys = mp6_hsf_v_u16(t + 10);
                uint32_t ofs = mp6_hsf_v_u32(t + 12);
                uint32_t elem = curve == 2u ? 16u
                              : (curve == 0u || curve == 1u || curve == 3u) ? 8u : 0u;
                uint32_t nativeElem = curve == 2u ? 16u : curve == 3u ? 16u : 8u;
                if (elem != 0u
                        && (keys == 0u
                            || !mp6_hsf_v_nested_count(&c, keys, nativeElem,
                                                    "curve allocation overflow")
                            || !mp6_hsf_v_range(&c, pool + ofs,
                                                (uint64_t)keys * elem))) {
                    return mp6_hsf_v_fail(&c, "curve data outside file", j, keys);
                }
                if (curve == 3u) {
                    uint32_t k;
                    for (k = 0; k < keys; ++k) {
                        int32_t bitmap = mp6_hsf_v_s32(
                            c.b + pool + ofs + (size_t)k * 8u + 4u);
                        if (bitmap < -1 || bitmap >= c.sec[MP6_HSF_SEC_BITMAP].num) {
                            return mp6_hsf_v_fail(&c,
                                "bitmap curve index outside section", j, k);
                        }
                    }
                }
                if (type == 3u) {
                    int32_t morphWeight = mp6_hsf_v_s16(t + 6);
                    if (morphWeight < 0 || morphWeight >= 33) {
                        return mp6_hsf_v_fail(&c,
                            "morph track weight index outside array", j,
                            (uint32_t)morphWeight);
                    }
                } else if (type == 6u) {
                    int32_t clusterWeight = mp6_hsf_v_s32(t + 4);
                    if (clusterWeight < 0 || clusterWeight >= 32) {
                        return mp6_hsf_v_fail(&c,
                            "cluster weight track index outside array", j,
                            (uint32_t)clusterWeight);
                    }
                } else if (type == 9u && c.sec[MP6_HSF_SEC_OBJECT].num > 0) {
                    int32_t material = mp6_hsf_v_s16(t + 4);
                    if (material < 0 || material >= c.sec[MP6_HSF_SEC_MATERIAL].num) {
                        return mp6_hsf_v_fail(&c,
                            "embedded material track index outside section", j,
                            (uint32_t)material);
                    }
                } else if (type == 10u && target == UINT16_MAX) {
                    int32_t attribute = mp6_hsf_v_s16(t + 4);
                    if (attribute < -1 || (c.sec[MP6_HSF_SEC_OBJECT].num > 0
                            && attribute >= c.sec[MP6_HSF_SEC_ATTRIBUTE].num)) {
                        return mp6_hsf_v_fail(&c,
                            "attribute track index outside section", j,
                            (uint32_t)attribute);
                    }
                }
                if (((type == 2u || type == 3u)
                        && c.sec[MP6_HSF_SEC_OBJECT].num > 0)
                        || type == 5u || type == 6u
                        || (type == 10u && target != UINT16_MAX)) {
                    if (!mp6_hsf_v_string(&c, target,
                                          "motion target outside string table")) return 0;
                }
            }
        }
    }

    if (validated != NULL) {
        validated->logicalSize = c.n;
        validated->stringOffset = c.sec[MP6_HSF_SEC_STRING].ofs;
        validated->stringSize = (uint32_t)c.sec[MP6_HSF_SEC_STRING].num;
        validated->oobFaceCorners = c.oobFaceCorners;
    }
    return 1;
}

#endif /* MP6_HSF_VALIDATE_INTERNAL_H */
