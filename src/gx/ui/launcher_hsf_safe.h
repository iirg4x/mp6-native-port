/* Dependency-free validator for the launcher's runtime RGB5A3 HSF asset. */
#ifndef MP6_LAUNCHER_HSF_SAFE_H
#define MP6_LAUNCHER_HSF_SAFE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct Mp6LauncherRgb5a3View {
    const unsigned char *pixels;
    int width;
    int height;
    size_t tiledBytes;
} Mp6LauncherRgb5a3View;

static inline uint32_t mp6_launcher_hsf_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t mp6_launcher_hsf_be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline int mp6_launcher_hsf_span(size_t total, uint64_t offset, uint64_t size)
{
    return offset <= (uint64_t)total && size <= (uint64_t)total - offset;
}

static inline int mp6_launcher_hsf_find_rgb5a3_view(
    const unsigned char *blob, size_t len, const char *name,
    Mp6LauncherRgb5a3View *out)
{
    uint32_t bmpOfs, strOfs, bmpNum;
    uint64_t recordsBytes, pool;
    size_t wantedLen;
    uint32_t i;

    if (blob == NULL || name == NULL || out == NULL || len < 176 ||
        memcmp(blob, "HSFV", 4) != 0) return 0;
    memset(out, 0, sizeof(*out));
    bmpOfs = mp6_launcher_hsf_be32(blob + 8 + 9 * 8);
    bmpNum = mp6_launcher_hsf_be32(blob + 12 + 9 * 8);
    strOfs = mp6_launcher_hsf_be32(blob + 8 + 20 * 8);
    if (bmpNum == 0 || bmpNum > 4096) return 0;
    recordsBytes = (uint64_t)bmpNum * 32u;
    if (!mp6_launcher_hsf_span(len, bmpOfs, recordsBytes) || strOfs >= len) return 0;
    pool = (uint64_t)bmpOfs + recordsBytes;
    wantedLen = strlen(name);

    for (i = 0; i < bmpNum; i++) {
        uint64_t recOfs = (uint64_t)bmpOfs + (uint64_t)i * 32u;
        const unsigned char *rec = blob + (size_t)recOfs;
        uint32_t nameOfs = mp6_launcher_hsf_be32(rec);
        uint64_t namePos = (uint64_t)strOfs + nameOfs;
        const char *bmpName;
        const char *term;
        size_t actualLen;
        uint32_t dataOfs;
        uint32_t w, h;
        uint64_t tilesX, tilesY, tiledBytes, pixelOfs;

        if (!mp6_launcher_hsf_span(len, namePos, 1)) continue;
        bmpName = (const char *)(blob + (size_t)namePos);
        term = (const char *)memchr(bmpName, '\0', len - (size_t)namePos);
        if (term == NULL) continue;
        actualLen = (size_t)(term - bmpName);
        if (actualLen != wantedLen || memcmp(bmpName, name, wantedLen) != 0) continue;

        if (rec[8] != 5 /* HSF_BMPFMT_RGB5A3 */ || rec[9] != 16) return 0;
        w = mp6_launcher_hsf_be16(rec + 10);
        h = mp6_launcher_hsf_be16(rec + 12);
        if (w == 0 || h == 0 || w > 2048 || h > 2048) return 0;
        dataOfs = mp6_launcher_hsf_be32(rec + 28);
        tilesX = ((uint64_t)w + 3u) / 4u;
        tilesY = ((uint64_t)h + 3u) / 4u;
        tiledBytes = tilesX * tilesY * 32u;
        if (dataOfs > UINT64_MAX - pool) return 0;
        pixelOfs = pool + dataOfs;
        if (!mp6_launcher_hsf_span(len, pixelOfs, tiledBytes)) return 0;

        out->pixels = blob + (size_t)pixelOfs;
        out->width = (int)w;
        out->height = (int)h;
        out->tiledBytes = (size_t)tiledBytes;
        return 1;
    }
    return 0;
}

#endif /* MP6_LAUNCHER_HSF_SAFE_H */
