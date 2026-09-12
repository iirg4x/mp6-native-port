/* Bounds-only validator for the original 32-bit, big-endian ANM sprite
 * container.  The native port cannot walk these bytes through ANIMDATA:
 * the packed pointer fields are four bytes while host pointers are eight.
 * Keep this header dependency-free so the exact runtime validator can also
 * be compiled under ASan/UBSan and exercised against the retail corpus. */
#ifndef MP6_ANIM_VALIDATE_INTERNAL_H
#define MP6_ANIM_VALIDATE_INTERNAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    MP6_ANIM_HEADER_SIZE = 20,
    MP6_ANIM_BANK_SIZE = 8,
    MP6_ANIM_FRAME_SIZE = 12,
    MP6_ANIM_PAT_SIZE = 16,
    MP6_ANIM_LAYER_SIZE = 32,
    MP6_ANIM_BMP_SIZE = 20
};

typedef struct MP6AnimValidated_s {
    int16_t bankNum;
    int16_t patNum;
    int16_t bmpNum;
    uint32_t bankOffset;
    uint32_t patOffset;
    uint32_t bmpOffset;
    uint32_t frameTotal;
    uint32_t layerTotal;
    size_t logicalSize;
} MP6AnimValidated;

static uint16_t mp6_anim_v_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t mp6_anim_v_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int16_t mp6_anim_v_s16(const uint8_t *p)
{
    return (int16_t)mp6_anim_v_u16(p);
}

static int mp6_anim_v_range(size_t capacity, uint64_t offset, uint64_t bytes)
{
    return offset <= (uint64_t)capacity
        && bytes <= (uint64_t)capacity - offset;
}

static int mp6_anim_v_fail(char *why, size_t whyCap, const char *what,
                           uint32_t a, uint32_t b)
{
    if (why != NULL && whyCap != 0) {
        (void)snprintf(why, whyCap, "%s (%u, %u)", what,
                       (unsigned)a, (unsigned)b);
    }
    return 0;
}

static void mp6_anim_v_max(size_t *logical, uint64_t end)
{
    if (end <= (uint64_t)SIZE_MAX && (size_t)end > *logical) {
        *logical = (size_t)end;
    }
}

static int mp6_anim_v_texture_layout(uint8_t format, uint8_t pixSize,
                                     uint16_t width, uint16_t height,
                                     uint64_t *tileBytes)
{
    uint32_t tileW;
    uint32_t tileH;
    uint32_t tileSize;

    switch (format) {
    case 0:  tileW = 4; tileH = 4; tileSize = 64; break;
    case 1:
    case 2:
    case 5:  tileW = 4; tileH = 4; tileSize = 32; break;
    case 3:
    case 6:
    case 7:
    case 9:  tileW = 8; tileH = 4; tileSize = 32; break;
    case 4:
    case 8:
    case 10: tileW = 8; tileH = 8; tileSize = 32; break;
    default:
        return 0;
    }
    /* pixSize is the authoring payload depth, not always the GX upload
     * format's bit depth. Three shipped RGB24 authoring records retain
     * format=3 and pixSize=24 (their data/palette offsets alias); the GX
     * consumer still reads only the bounded C8 tile prefix. */
    if ((pixSize != 4 && pixSize != 8 && pixSize != 16
            && pixSize != 24 && pixSize != 32)
            || width == 0 || height == 0) return 0;
    *tileBytes = (((uint64_t)width + tileW - 1u) / tileW)
               * (((uint64_t)height + tileH - 1u) / tileH) * tileSize;
    return *tileBytes != 0 && *tileBytes <= UINT32_MAX;
}

/* Validate every byte range and every nested index consumed by
 * HuSprAnimRead/HuSprCall/HuSprDisp. `capacity` must be either a verified
 * live allocation extent or an explicit compile-time size supplied by the
 * static-array caller. A zero capacity is always rejection, never an
 * invitation to trust packed offsets. */
static int mp6_anim_preflight(const void *data, size_t capacity,
                              MP6AnimValidated *validated,
                              char *why, size_t whyCap)
{
    const uint8_t *raw = (const uint8_t *)data;
    MP6AnimValidated v;
    uint64_t recordsEnd;
    size_t logical = MP6_ANIM_HEADER_SIZE;
    int32_t i;

    memset(&v, 0, sizeof(v));
    if (validated != NULL) memset(validated, 0, sizeof(*validated));
    if (why != NULL && whyCap != 0) why[0] = '\0';
    if (raw == NULL || capacity < MP6_ANIM_HEADER_SIZE) {
        return mp6_anim_v_fail(why, whyCap,
                               "ANM header outside verified extent",
                               (uint32_t)(capacity > UINT32_MAX
                                   ? UINT32_MAX : capacity),
                               MP6_ANIM_HEADER_SIZE);
    }

    v.bankNum = mp6_anim_v_s16(raw + 0);
    v.patNum = mp6_anim_v_s16(raw + 2);
    v.bmpNum = mp6_anim_v_s16(raw + 4);
    v.bankOffset = mp6_anim_v_u32(raw + 8);
    v.patOffset = mp6_anim_v_u32(raw + 12);
    v.bmpOffset = mp6_anim_v_u32(raw + 16);

    /* Every drawable ANM needs all three top-level arrays.  This also
     * rejects the native ANIM_BMP_ALLOC high bit when it is presented as
     * if it were still-packed input. */
    if (v.bankNum <= 0 || v.patNum <= 0 || v.bmpNum <= 0) {
        return mp6_anim_v_fail(why, whyCap,
                               "ANM has invalid top-level count",
                               (uint32_t)(uint16_t)v.bankNum,
                               ((uint32_t)(uint16_t)v.patNum << 16)
                                   | (uint16_t)v.bmpNum);
    }
    if (!mp6_anim_v_range(capacity, v.bankOffset,
                          (uint64_t)(uint16_t)v.bankNum * MP6_ANIM_BANK_SIZE)
            || !mp6_anim_v_range(capacity, v.patOffset,
                          (uint64_t)(uint16_t)v.patNum * MP6_ANIM_PAT_SIZE)
            || !mp6_anim_v_range(capacity, v.bmpOffset,
                          (uint64_t)(uint16_t)v.bmpNum * MP6_ANIM_BMP_SIZE)) {
        return mp6_anim_v_fail(why, whyCap,
                               "ANM top-level record array outside extent",
                               v.bankOffset, v.patOffset);
    }
    recordsEnd = (uint64_t)v.bankOffset
               + (uint64_t)(uint16_t)v.bankNum * MP6_ANIM_BANK_SIZE;
    mp6_anim_v_max(&logical, recordsEnd);
    recordsEnd = (uint64_t)v.patOffset
               + (uint64_t)(uint16_t)v.patNum * MP6_ANIM_PAT_SIZE;
    mp6_anim_v_max(&logical, recordsEnd);
    recordsEnd = (uint64_t)v.bmpOffset
               + (uint64_t)(uint16_t)v.bmpNum * MP6_ANIM_BMP_SIZE;
    mp6_anim_v_max(&logical, recordsEnd);

    for (i = 0; i < v.bankNum; ++i) {
        const uint8_t *bank = raw + v.bankOffset
                            + (size_t)i * MP6_ANIM_BANK_SIZE;
        int32_t frameCount = mp6_anim_v_s16(bank);
        uint32_t frameOffset = mp6_anim_v_u32(bank + 4);
        int32_t j;
        if (frameCount <= 0
                || (uint32_t)frameCount > UINT32_MAX - v.frameTotal
                || !mp6_anim_v_range(capacity, frameOffset,
                         (uint64_t)(uint32_t)frameCount * MP6_ANIM_FRAME_SIZE)) {
            return mp6_anim_v_fail(why, whyCap,
                                   "ANM frame array outside extent",
                                   (uint32_t)i, (uint32_t)frameCount);
        }
        v.frameTotal += (uint32_t)frameCount;
        mp6_anim_v_max(&logical, (uint64_t)frameOffset
                     + (uint64_t)(uint32_t)frameCount * MP6_ANIM_FRAME_SIZE);
        for (j = 0; j < frameCount; ++j) {
            const uint8_t *frame = raw + frameOffset
                                 + (size_t)j * MP6_ANIM_FRAME_SIZE;
            int32_t pattern = mp6_anim_v_s16(frame);
            int32_t time = mp6_anim_v_s16(frame + 2);
            if (pattern < 0 || pattern >= v.patNum || time < -1) {
                return mp6_anim_v_fail(why, whyCap,
                                       "ANM frame has invalid pattern/time",
                                       (uint32_t)i, (uint32_t)j);
            }
        }
    }

    for (i = 0; i < v.patNum; ++i) {
        const uint8_t *pat = raw + v.patOffset
                           + (size_t)i * MP6_ANIM_PAT_SIZE;
        int32_t layerCount = mp6_anim_v_s16(pat);
        uint32_t layerOffset = mp6_anim_v_u32(pat + 12);
        int32_t j;
        if (layerCount < 0
                || (uint32_t)layerCount > UINT32_MAX - v.layerTotal
                || !mp6_anim_v_range(capacity, layerOffset,
                         (uint64_t)(uint32_t)layerCount * MP6_ANIM_LAYER_SIZE)) {
            return mp6_anim_v_fail(why, whyCap,
                                   "ANM layer array outside extent",
                                   (uint32_t)i, (uint32_t)layerCount);
        }
        v.layerTotal += (uint32_t)layerCount;
        mp6_anim_v_max(&logical, (uint64_t)layerOffset
                     + (uint64_t)(uint32_t)layerCount * MP6_ANIM_LAYER_SIZE);
        for (j = 0; j < layerCount; ++j) {
            const uint8_t *layer = raw + layerOffset
                                 + (size_t)j * MP6_ANIM_LAYER_SIZE;
            int32_t bitmap = mp6_anim_v_s16(layer + 2);
            if (bitmap < 0 || bitmap >= v.bmpNum) {
                return mp6_anim_v_fail(why, whyCap,
                                       "ANM layer bitmap index outside array",
                                       (uint32_t)i, (uint32_t)j);
            }
        }
    }

    for (i = 0; i < v.bmpNum; ++i) {
        const uint8_t *bmp = raw + v.bmpOffset
                           + (size_t)i * MP6_ANIM_BMP_SIZE;
        uint8_t pixSize = bmp[0];
        uint8_t format = bmp[1];
        int32_t paletteCount = mp6_anim_v_s16(bmp + 2);
        int32_t width = mp6_anim_v_s16(bmp + 4);
        int32_t height = mp6_anim_v_s16(bmp + 6);
        uint32_t dataSize = mp6_anim_v_u32(bmp + 8);
        uint32_t paletteOffset = mp6_anim_v_u32(bmp + 12);
        uint32_t dataOffset = mp6_anim_v_u32(bmp + 16);
        uint64_t tileBytes = 0;
        int indexed = format == 3 || format == 4;
        int direct24Compat = format == 3 && pixSize == 24
                          && paletteCount == 0
                          && paletteOffset == dataOffset;

        if (width <= 0 || height <= 0 || paletteCount < 0
                || !mp6_anim_v_texture_layout(format, pixSize,
                        (uint16_t)width, (uint16_t)height, &tileBytes)
                || dataSize < tileBytes
                || !mp6_anim_v_range(capacity, dataOffset, dataSize)
                || !mp6_anim_v_range(capacity, dataOffset, tileBytes)) {
            return mp6_anim_v_fail(why, whyCap,
                                   "ANM bitmap data/dimensions invalid",
                                   (uint32_t)i, dataSize);
        }
        if ((indexed && !direct24Compat
                    && (paletteCount <= 0 || paletteOffset == 0
                         || (format == 3 && paletteCount > 256)
                         || (format == 4 && paletteCount > 16)))
                || (!indexed && paletteCount != 0)
                || (paletteCount > 0
                    && !mp6_anim_v_range(capacity, paletteOffset,
                                         (uint64_t)(uint32_t)paletteCount * 2u))) {
            return mp6_anim_v_fail(why, whyCap,
                                   "ANM bitmap palette invalid",
                                   (uint32_t)i, (uint32_t)paletteCount);
        }
        mp6_anim_v_max(&logical, (uint64_t)dataOffset + dataSize);
        if (paletteCount > 0) {
            mp6_anim_v_max(&logical, (uint64_t)paletteOffset
                         + (uint64_t)(uint32_t)paletteCount * 2u);
        }
    }

    v.logicalSize = logical;
    if (validated != NULL) *validated = v;
    return 1;
}

#endif /* MP6_ANIM_VALIDATE_INTERNAL_H */
