"""Executable coverage for native HSF bitmap mip-chain validation."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]
SELFTEST = ROOT / "tests/native/hsf_bitmap_mips_selftest.c"
LOAD_SOURCE = ROOT / "src/hsf/hsf_load_native.c"
LOAD_PROBE_MARKER = "/* __MP6_HSF_BITMAP_MIPS_LOAD_PROBE__ */"


def function_body(source, name):
    match = re.search(
        r"(?m)^static\s+void\s+" + re.escape(name)
        + r"\s*\([^;{}]*\)\s*\{", source)
    if match is None:
        raise AssertionError(f"missing function definition: {name}")
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():index + 1]
    raise AssertionError(f"unterminated function: {name}")


LOAD_PROBE_STUBS = r'''/* Narrow seam for the actual hsf_load_native.c LoadBitmaps. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short s16;
typedef int s32;

enum {
    FSEC_ATTRIBUTE = MP6_HSF_SEC_ATTRIBUTE,
    FSEC_BITMAP = MP6_HSF_SEC_BITMAP,
    FSEC_STRING = MP6_HSF_SEC_STRING,
    FSEC_COUNT = MP6_HSF_SEC_COUNT,
    HEAP_MODEL = 4,
    HEAP_HEAP = 5,
};

#define FHDR_SIZE 176
#define FBMP_NAME 0
#define FBMP_MAXLOD 4
#define FBMP_DATAFMT 8
#define FBMP_PIXSIZE 9
#define FBMP_SIZEX 10
#define FBMP_SIZEY 12
#define FBMP_PALSIZE 14
#define FBMP_TINT 16
#define FBMP_PALDATA 20
#define FBMP_UNK 24
#define FBMP_DATA 28
#define FBMP_SIZE 32
#define FATTR_MAXLOD 120
#define FATTR_FLAG 124
#define FATTR_BITMAP 128
#define FATTR_SIZE 132
#define HSF_FLAG_MIPMAP 0x80u

typedef struct ProbeSection_s {
    u32 ofs;
    s32 num;
} ProbeSection;

typedef struct ProbeHeader_s {
    u8 magic[8];
    ProbeSection sec[FSEC_COUNT];
} ProbeHeader;

typedef struct ProbeColor_s {
    u8 r, g, b, a;
} ProbeColor;

typedef struct HSF_PALETTE_s {
    void *data;
} HSF_PALETTE;

typedef struct HSF_BITMAP_s {
    char *name;
    u32 maxLod;
    u8 dataFmt;
    u8 pixSize;
    s16 sizeX;
    s16 sizeY;
    s16 palSize;
    ProbeColor tint;
    void *palData;
    u32 unk;
    void *data;
} HSF_BITMAP;

typedef struct LoadCtx_s {
    const u8 *base;
    size_t fileSize;
    ProbeHeader h;
    HSF_PALETTE *palette;
    s32 paletteNum;
    HSF_BITMAP *bitmap;
    s32 bitmapNum;
    u32 mallocTag;
} LoadCtx;

static u16 be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16)
         | ((u32)p[2] << 8) | (u32)p[3];
}

static char *GetStr(const LoadCtx *ctx, u32 offset)
{
    (void)offset;
    return (char *)ctx->base;
}

static HSF_PALETTE *FindPaletteById(const LoadCtx *ctx, s32 id)
{
    (void)ctx;
    (void)id;
    return NULL;
}

enum { PROBE_CANARY_BYTES = 32, PROBE_MAX_ALLOCS = 16 };
typedef struct ProbeAlloc_s {
    unsigned char *raw;
    void *user;
    size_t size;
    s32 heap;
    int freed;
} ProbeAlloc;

static ProbeAlloc probeAllocs[PROBE_MAX_ALLOCS];
static size_t probeAllocCount;
static size_t probeFreeCount;
static s32 probeLastFreeHeap;
static size_t probeLastFreeSize;

static void probeCanaryFill(unsigned char *bytes, unsigned char value)
{
    memset(bytes, value, PROBE_CANARY_BYTES);
}

static void probeCanaryCheck(const ProbeAlloc *allocation)
{
    size_t i;
    for (i = 0; i < PROBE_CANARY_BYTES; ++i) {
        CHECK(allocation->raw[i] == 0xC3u);
        CHECK(allocation->raw[PROBE_CANARY_BYTES + allocation->size + i] == 0x5Au);
    }
}

static void *HuMemDirectMallocNum(s32 heap, s32 size, u32 tag)
{
    ProbeAlloc *allocation;
    unsigned char *raw;
    size_t total;
    (void)tag;
    CHECK(size >= 0);
    CHECK(probeAllocCount < PROBE_MAX_ALLOCS);
    total = PROBE_CANARY_BYTES + (size_t)size + PROBE_CANARY_BYTES;
    raw = (unsigned char *)malloc(total);
    CHECK(raw != NULL);
    probeCanaryFill(raw, 0xC3u);
    probeCanaryFill(raw + PROBE_CANARY_BYTES + (size_t)size, 0x5Au);
    allocation = &probeAllocs[probeAllocCount++];
    allocation->raw = raw;
    allocation->user = raw + PROBE_CANARY_BYTES;
    allocation->size = (size_t)size;
    allocation->heap = heap;
    allocation->freed = 0;
    return allocation->user;
}

static ProbeAlloc *probeFindAlloc(void *user)
{
    size_t i;
    for (i = 0; i < probeAllocCount; ++i) {
        if (probeAllocs[i].user == user) return &probeAllocs[i];
    }
    return NULL;
}

static void HuMemDirectFree(void *user)
{
    ProbeAlloc *allocation = probeFindAlloc(user);
    CHECK(allocation != NULL);
    CHECK(!allocation->freed);
    probeCanaryCheck(allocation);
    allocation->freed = 1;
    probeFreeCount += 1;
    probeLastFreeHeap = allocation->heap;
    probeLastFreeSize = allocation->size;
    free(allocation->raw);
}

static void probePutBitmap(u8 *record, u32 maxLod, u8 format, u8 pixelSize,
                           u16 width, u16 height, u32 dataOffset)
{
    put_u32(record + FBMP_NAME, 0);
    put_u32(record + FBMP_MAXLOD, maxLod);
    record[FBMP_DATAFMT] = format;
    record[FBMP_PIXSIZE] = pixelSize;
    put_u16(record + FBMP_SIZEX, width);
    put_u16(record + FBMP_SIZEY, height);
    put_u16(record + FBMP_PALSIZE, 0);
    put_s32(record + FBMP_PALDATA, -1);
    put_u32(record + FBMP_DATA, dataOffset);
}

static void probePutAttribute(u8 *record, u32 maxLod, u32 flags,
                              s32 bitmap)
{
    put_u32(record + 0, UINT32_MAX);
    put_u32(record + FATTR_MAXLOD, maxLod);
    put_u32(record + FATTR_FLAG, flags);
    put_s32(record + FATTR_BITMAP, bitmap);
}

static void probePattern(u8 *bytes, size_t size, u8 seed)
{
    size_t i;
    for (i = 0; i < size; ++i) bytes[i] = (u8)(seed + (u8)(i * 37u));
}

static void LoadBitmaps(LoadCtx *ctx);

static int run_loadbitmaps_probe(void)
{
    const uint64_t bitmap0Bytes = chain_bytes(6, 32, 128, 128, 1);
    const uint64_t bitmap1Bytes = chain_bytes(7, 4, 512, 512, 9);
    const size_t bitmapCount = 2;
    const size_t attributeCount = 4;
    const size_t attributeOfs = FHDR_SIZE;
    const size_t bitmapOfs = attributeOfs + attributeCount * FATTR_SIZE;
    const size_t poolOfs = bitmapOfs + bitmapCount * FBMP_SIZE;
    const size_t bitmap1Ofs = (size_t)bitmap0Bytes;
    const size_t fileSize = poolOfs + (size_t)bitmap0Bytes
                          + (size_t)bitmap1Bytes;
    uint8_t *file = (uint8_t *)calloc(fileSize, 1u);
    LoadCtx ctx;
    size_t i;
    int sawBitmap0 = 0;
    int sawBitmap1 = 0;

    CHECK(file != NULL);
    memset(&ctx, 0, sizeof(ctx));
    ctx.base = file;
    ctx.fileSize = fileSize;
    ctx.h.sec[FSEC_ATTRIBUTE].ofs = (u32)attributeOfs;
    ctx.h.sec[FSEC_ATTRIBUTE].num = (s32)attributeCount;
    ctx.h.sec[FSEC_BITMAP].ofs = (u32)bitmapOfs;
    ctx.h.sec[FSEC_BITMAP].num = (s32)bitmapCount;

    /* Header maxLod is a level count: bitmap #0 requests levels 0..1. */
    probePutBitmap(file + bitmapOfs + 0 * FBMP_SIZE, 2, 6, 32,
                   128, 128, 0);
    /* The flagged attributes raise bitmap #1 from header levels 0..1 to
     * inclusive level 9.  The unflagged level-15 request must be ignored. */
    probePutBitmap(file + bitmapOfs + 1 * FBMP_SIZE, 2, 7, 4,
                   512, 512, (u32)bitmap1Ofs);
    probePutAttribute(file + attributeOfs + 0 * FATTR_SIZE, 15, 0, 0);
    probePutAttribute(file + attributeOfs + 1 * FATTR_SIZE, 7,
                      HSF_FLAG_MIPMAP, 1);
    probePutAttribute(file + attributeOfs + 2 * FATTR_SIZE, 9,
                      HSF_FLAG_MIPMAP, 1);
    probePutAttribute(file + attributeOfs + 3 * FATTR_SIZE, 15, 0, 1);
    probePattern(file + poolOfs, (size_t)bitmap0Bytes, 0x11u);
    probePattern(file + poolOfs + bitmap1Ofs, (size_t)bitmap1Bytes, 0x72u);

    LoadBitmaps(&ctx);
    CHECK(ctx.bitmap != NULL);
    CHECK(ctx.bitmapNum == (s32)bitmapCount);
    CHECK(ctx.bitmap[0].maxLod == 2);
    CHECK(ctx.bitmap[1].maxLod == 2);
    CHECK(probeAllocCount == 2); /* HSF_BITMAP[], mipLevels[]: texels stay in the file */
    CHECK(probeFreeCount == 1);
    CHECK(probeLastFreeHeap == HEAP_HEAP);
    CHECK(probeLastFreeSize == bitmapCount * sizeof(u32));
    CHECK(memcmp(ctx.bitmap[0].data, file + poolOfs,
                 (size_t)bitmap0Bytes) == 0);
    CHECK(memcmp(ctx.bitmap[1].data, file + poolOfs + bitmap1Ofs,
                 (size_t)bitmap1Bytes) == 0);
    CHECK(((const u8 *)ctx.bitmap[0].data)[bitmap0Bytes - 1u]
          == file[poolOfs + bitmap0Bytes - 1u]);
    CHECK(((const u8 *)ctx.bitmap[1].data)[bitmap1Bytes - 1u]
          == file[poolOfs + bitmap1Ofs + bitmap1Bytes - 1u]);

    /* Texels are used in place, as on the GameCube: same bytes, same
     * neighbours, so recovered code that writes past one texture (m613's
     * 16-bit border clears over a 4-bit canvas) lands in the next one. */
    CHECK((const u8 *)ctx.bitmap[0].data == file + poolOfs);
    CHECK((const u8 *)ctx.bitmap[1].data == file + poolOfs + bitmap1Ofs);
    for (i = 0; i < probeAllocCount; ++i) {
        if (!probeAllocs[i].freed) probeCanaryCheck(&probeAllocs[i]);
    }
    sawBitmap0 = sawBitmap1 = 1;
    CHECK(sawBitmap0 && sawBitmap1);

    HuMemDirectFree(ctx.bitmap);
    CHECK(probeFreeCount == probeAllocCount);
    free(file);
    return 1;
}

'''


class HsfBitmapMips(unittest.TestCase):
    def test_real_chain_helper_and_preflight_fixtures(self):
        with tempfile.TemporaryDirectory(
                prefix="hsf-bitmap-mips-", dir=ROOT / "build") as directory:
            source_text = SELFTEST.read_text(encoding="utf-8")
            load_text = LOAD_SOURCE.read_text(encoding="utf-8")
            self.assertEqual(source_text.count(LOAD_PROBE_MARKER), 1)
            generated_source = Path(directory) / "hsf_bitmap_mips_probe.c"
            generated_source.write_text(
                source_text.replace(
                    LOAD_PROBE_MARKER,
                    LOAD_PROBE_STUBS + "\n" + function_body(load_text, "LoadBitmaps")
                ),
                encoding="utf-8",
            )
            executable = Path(directory) / "hsf_bitmap_mips_selftest.exe"
            compile_result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    "-std=c11",
                    "-O2",
                    "-UNDEBUG",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src/hsf"),
                    str(generated_source),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                "HSF bitmap mip self-test compile failed:\n"
                + compile_result.stderr,
            )

            run_result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(
                run_result.returncode,
                0,
                "HSF bitmap mip self-test failed:\n"
                + run_result.stdout
                + run_result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
