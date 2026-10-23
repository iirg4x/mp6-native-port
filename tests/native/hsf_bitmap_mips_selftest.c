#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hsf_validate_internal.h"

enum {
    HSF_HEADER_BYTES = 176,
    HSF_BITMAP_RECORD_BYTES = 32,
    HSF_ATTRIBUTE_RECORD_BYTES = 132,
    HSF_BITMAP_SECTION = MP6_HSF_SEC_BITMAP,
    HSF_ATTRIBUTE_SECTION = MP6_HSF_SEC_ATTRIBUTE,
    HSF_STRING_SECTION = MP6_HSF_SEC_STRING,
};

typedef struct BitmapFixture_s {
    uint8_t *data;
    size_t capacity;
} BitmapFixture;

static void fail_check(const char *expression, const char *file, int line)
{
    fprintf(stderr, "HSF bitmap mip self-test failed at %s:%d: %s\n",
            file, line, expression);
    exit(1);
}

#define CHECK(expression) \
    do { if (!(expression)) fail_check(#expression, __FILE__, __LINE__); } while (0)

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void put_s32(uint8_t *dst, int32_t value)
{
    put_u32(dst, (uint32_t)value);
}

static void put_section(uint8_t *data, unsigned section, uint32_t ofs,
                        int32_t count)
{
    put_u32(data + 8u + section * 8u, ofs);
    put_s32(data + 12u + section * 8u, count);
}

static void fixture_destroy(BitmapFixture *fixture)
{
    free(fixture->data);
    fixture->data = NULL;
    fixture->capacity = 0;
}

/* Construct only the records consumed by bitmap/attribute preflight.  The
 * string section is still real and last, so the validator proves the exact
 * logical file end just like a decoded disc HSF. */
static void fixture_build(BitmapFixture *fixture, uint8_t format,
                          uint8_t pixelSize, uint16_t width, uint16_t height,
                          uint32_t bitmapMaxLod, const uint32_t *attrMaxLod,
                          const uint32_t *attrFlags, unsigned attrCount,
                          size_t pixelBytes)
{
    size_t attributeOfs = HSF_HEADER_BYTES;
    size_t bitmapOfs = attributeOfs
                     + (size_t)attrCount * HSF_ATTRIBUTE_RECORD_BYTES;
    size_t pixelOfs = bitmapOfs + HSF_BITMAP_RECORD_BYTES;
    size_t stringOfs = pixelOfs + pixelBytes;
    size_t capacity = stringOfs + 2u;
    uint8_t *bitmap;
    unsigned i;

    fixture->data = (uint8_t *)calloc(capacity, 1u);
    CHECK(fixture->data != NULL);
    fixture->capacity = capacity;

    memcpy(fixture->data, "HSFV037", 7u);
    put_section(fixture->data, HSF_ATTRIBUTE_SECTION, (uint32_t)attributeOfs,
                (int32_t)attrCount);
    put_section(fixture->data, HSF_BITMAP_SECTION, (uint32_t)bitmapOfs, 1);
    put_section(fixture->data, HSF_STRING_SECTION, (uint32_t)stringOfs, 2);

    bitmap = fixture->data + bitmapOfs;
    put_u32(bitmap + 0, 0);                 /* name: string offset 0 */
    put_u32(bitmap + 4, bitmapMaxLod);
    bitmap[8] = format;
    bitmap[9] = pixelSize;
    put_u16(bitmap + 10, width);
    put_u16(bitmap + 12, height);
    put_u16(bitmap + 14, 0);                /* no palette for RGBA8/CMPR */
    put_s32(bitmap + 20, -1);
    put_u32(bitmap + 28, 0);                /* pixel pool offset */

    for (i = 0; i < attrCount; ++i) {
        uint8_t *attribute = fixture->data + attributeOfs
                           + (size_t)i * HSF_ATTRIBUTE_RECORD_BYTES;
        put_u32(attribute + 0, UINT32_MAX); /* optional attribute name */
        put_u32(attribute + 120, attrMaxLod[i]);
        put_u32(attribute + 124, attrFlags[i]);
        put_s32(attribute + 128, 0);        /* shared bitmap #0 */
    }

    memset(fixture->data + pixelOfs, 0xA5, pixelBytes);
    fixture->data[stringOfs + 0] = 'x';
    fixture->data[stringOfs + 1] = '\0';
}

static int fixture_preflight(const BitmapFixture *fixture)
{
    MP6HsfValidated validated;
    char why[160];

    return mp6_hsf_preflight(fixture->data, fixture->capacity, &validated,
                             why, sizeof(why));
}

static uint64_t chain_bytes(uint8_t format, uint8_t pixelSize,
                            uint16_t width, uint16_t height,
                            uint32_t maxLod)
{
    uint64_t bytes = 0;
    CHECK(mp6_hsf_v_bitmap_chain_bytes(format, pixelSize, width, height,
                                       maxLod, &bytes));
    return bytes;
}

static void test_chain_sizes(void)
{
    uint64_t bytes;

    CHECK(mp6_hsf_v_bitmap_chain_bytes(6, 32, 128, 128, 7, &bytes));
    CHECK(bytes == UINT64_C(87488));

    CHECK(mp6_hsf_v_bitmap_chain_bytes(7, 4, 512, 512, 9, &bytes));
    CHECK(bytes == UINT64_C(174848));

    /* The helper stops after the first 1x1 level, even if maxLod is larger. */
    CHECK(mp6_hsf_v_bitmap_chain_bytes(6, 32, 5, 9, 8, &bytes));
    CHECK(bytes == UINT64_C(576));
    CHECK(mp6_hsf_v_bitmap_chain_bytes(7, 4, 9, 17, 4, &bytes));
    CHECK(bytes == UINT64_C(320));
}

static void test_invalid_chain_inputs(void)
{
    uint64_t bytes = UINT64_C(0xDEADBEEF);

    CHECK(!mp6_hsf_v_bitmap_chain_bytes(8, 32, 4, 4, 0, &bytes));
    CHECK(!mp6_hsf_v_bitmap_chain_bytes(6, 16, 4, 4, 0, &bytes));
    CHECK(!mp6_hsf_v_bitmap_chain_bytes(6, 32, 0, 4, 0, &bytes));
    CHECK(!mp6_hsf_v_bitmap_chain_bytes(6, 32, 4, 0, 0, &bytes));
    CHECK(!mp6_hsf_v_bitmap_chain_bytes(6, 32, 4, 4, 16, &bytes));
}

static void test_base_only_legacy_padding(void)
{
    static const uint32_t nonMipAttrLod[] = { 15 };
    static const uint32_t nonMipAttrFlags[] = { 0 };
    BitmapFixture fixture = { 0 };
    const size_t naiveBytes = (size_t)3u * 3u * 32u / 8u;

    /* A base-only 3x3 RGBA8 bitmap has 36 authored bytes but one 64-byte GX
     * tile.  Retail files rely on the loader's zero padding for that tail. */
    fixture_build(&fixture, 6, 32, 3, 3, 0, NULL, NULL, 0, naiveBytes);
    CHECK(fixture_preflight(&fixture));
    fixture_destroy(&fixture);

    /* An attribute maxLod is meaningful only with the mipmap flag. */
    fixture_build(&fixture, 6, 32, 3, 3, 0, nonMipAttrLod,
                  nonMipAttrFlags, 1, naiveBytes);
    CHECK(fixture_preflight(&fixture));
    fixture_destroy(&fixture);

}

static void test_bitmap_header_chain_and_truncation(void)
{
    BitmapFixture fixture = { 0 };
    uint64_t full = chain_bytes(6, 32, 16, 16, 1);
    uint64_t shortChain = chain_bytes(6, 32, 16, 16, 0);

    fixture_build(&fixture, 6, 32, 16, 16, 2, NULL, NULL, 0,
                  (size_t)full);
    CHECK(fixture_preflight(&fixture));
    fixture_destroy(&fixture);

    fixture_build(&fixture, 6, 32, 16, 16, 2, NULL, NULL, 0,
                  (size_t)shortChain);
    CHECK(!fixture_preflight(&fixture));
    fixture_destroy(&fixture);
}

static void test_shared_bitmap_uses_largest_attribute_chain(void)
{
    static const uint32_t attrLod[] = { 1, 3 };
    static const uint32_t attrFlags[] = { 0x80u, 0x80u };
    BitmapFixture fixture = { 0 };
    uint64_t largest = chain_bytes(6, 32, 16, 16, 3);
    uint64_t secondLargest = chain_bytes(6, 32, 16, 16, 2);

    /* Bitmap #0 itself is base-only; two attributes share it and request
     * different mip chains.  The larger flagged request owns the allocation
     * requirement for the shared bitmap. */
    fixture_build(&fixture, 6, 32, 16, 16, 0, attrLod, attrFlags, 2,
                  (size_t)largest);
    CHECK(fixture_preflight(&fixture));
    fixture_destroy(&fixture);

    fixture_build(&fixture, 6, 32, 16, 16, 0, attrLod, attrFlags, 2,
                  (size_t)secondLargest);
    CHECK(!fixture_preflight(&fixture));
    fixture_destroy(&fixture);
}

static void test_invalid_preflight_mip_requests(void)
{
    static const uint32_t badAttrLod[] = { 16 };
    static const uint32_t mipFlag[] = { 0x80u };
    BitmapFixture fixture = { 0 };

    fixture_build(&fixture, 6, 32, 4, 4, 17, NULL, NULL, 0,
                  (size_t)chain_bytes(6, 32, 4, 4, 0));
    CHECK(!fixture_preflight(&fixture));
    fixture_destroy(&fixture);

    fixture_build(&fixture, 6, 32, 4, 4, 0, badAttrLod, mipFlag, 1,
                  (size_t)chain_bytes(6, 32, 4, 4, 0));
    CHECK(!fixture_preflight(&fixture));
    fixture_destroy(&fixture);
}

/* The Python wrapper replaces this marker with the real LoadBitmaps body and
 * a narrow allocator/context seam before compiling this translation unit. */
/* __MP6_HSF_BITMAP_MIPS_LOAD_PROBE__ */

int main(void)
{
    test_chain_sizes();
    test_invalid_chain_inputs();
    test_base_only_legacy_padding();
    test_bitmap_header_chain_and_truncation();
    test_shared_bitmap_uses_largest_attribute_chain();
    test_invalid_preflight_mip_requests();
    CHECK(run_loadbitmaps_probe());
    puts("hsf bitmap mip self-test: ok");
    return 0;
}
