#include "content_fst_validate.h"

#include <cstdio>
#include <cstring>
#include <vector>

static void put32(std::vector<unsigned char> &v, size_t off, uint32_t x)
{
    v[off + 0] = (unsigned char)(x >> 24);
    v[off + 1] = (unsigned char)(x >> 16);
    v[off + 2] = (unsigned char)(x >> 8);
    v[off + 3] = (unsigned char)x;
}

static std::vector<unsigned char> valid_fst()
{
    /* root, data/ directory, data/file.bin */
    const char strings[] = "\0data\0file.bin\0";
    std::vector<unsigned char> v(3 * 12 + sizeof(strings));
    put32(v, 0, 0x01000000u); put32(v, 4, 0); put32(v, 8, 3);
    put32(v, 12, 0x01000001u); put32(v, 16, 0); put32(v, 20, 3);
    put32(v, 24, 6u); put32(v, 28, 0x1000); put32(v, 32, 7);
    memcpy(v.data() + 36, strings, sizeof(strings));
    return v;
}

static std::vector<unsigned char> retail_style_root_fst()
{
    /* GP6E01 does not store an empty root name: string offset 0 belongs to
     * the first child (CVS). The root's nameOffset is implicit/unused. */
    const char strings[] = "CVS\0Entries\0";
    std::vector<unsigned char> v(3 * 12 + sizeof(strings));
    put32(v, 0, 0x01000000u); put32(v, 4, 0); put32(v, 8, 3);
    put32(v, 12, 0x01000000u); put32(v, 16, 0); put32(v, 20, 3);
    put32(v, 24, 4u); put32(v, 28, 0x1000); put32(v, 32, 7);
    memcpy(v.data() + 36, strings, sizeof(strings));
    return v;
}

static std::vector<unsigned char> colliding_fst()
{
    const char strings[] = "\0file.bin\0FILE.BIN\0";
    std::vector<unsigned char> v(3 * 12 + sizeof(strings));
    put32(v, 0, 0x01000000u); put32(v, 4, 0); put32(v, 8, 3);
    put32(v, 12, 1u); put32(v, 16, 0x1000); put32(v, 20, 7);
    put32(v, 24, 10u); put32(v, 28, 0x2000); put32(v, 32, 7);
    memcpy(v.data() + 36, strings, sizeof(strings));
    return v;
}

static std::vector<unsigned char> entry_limit_fst()
{
    const uint32_t count = MP6_CONTENT_FST_MAX_ENTRIES;
    std::vector<unsigned char> strings(1, 0);
    std::vector<unsigned char> v((size_t)count * 12u);
    put32(v, 0, 0x01000000u); put32(v, 4, 0); put32(v, 8, count);
    for (uint32_t i = 1; i < count; ++i) {
        char name[16];
        int n = snprintf(name, sizeof(name), "f%05u", i);
        uint32_t nameOff = (uint32_t)strings.size();
        strings.insert(strings.end(), name, name + n + 1);
        put32(v, (size_t)i * 12u, nameOff);
        put32(v, (size_t)i * 12u + 4u, 0);
        put32(v, (size_t)i * 12u + 8u, 0);
    }
    v.insert(v.end(), strings.begin(), strings.end());
    return v;
}

static int expect(const char *name, std::vector<unsigned char> v, int want)
{
    char err[256];
    int got = mp6_content_fst_validate(v.data(), v.size(), err, sizeof(err));
    if (got != want) {
        printf("FAIL %-22s got=%d want=%d err=%s\n", name, got, want, err);
        return 1;
    }
    return 0;
}

static int expect_boot(const char *name, size_t size, int want)
{
    std::vector<unsigned char> boot(size);
    if (size >= 6) memcpy(boot.data(), "GP6E01", 6);
    int got = mp6_content_boot_validate(boot.data(), boot.size());
    if (got != want) {
        printf("FAIL %-22s got=%d want=%d\n", name, got, want);
        return 1;
    }
    return 0;
}

int main()
{
    int fail = 0;
    std::vector<unsigned char> v;
    fail += expect("valid", valid_fst(), 1);
    fail += expect("retail-style implicit root name", retail_style_root_fst(), 1);
    v = valid_fst(); put32(v, 8, 0x40000000u); fail += expect("count outside file", v, 0);
    v = valid_fst(); put32(v, 24, 0x00FFFFFEu); fail += expect("bad string offset", v, 0);
    v = valid_fst(); v[v.size() - 1] = 'x'; v[v.size() - 2] = 'x';
    fail += expect("unterminated name", v, 0);
    v = valid_fst(); put32(v, 20, 4); fail += expect("dir end past parent", v, 0);
    v = valid_fst(); memcpy(v.data() + 37, "..\0", 3); fail += expect("unsafe name", v, 0);
    v = valid_fst(); put32(v, 12, 0x02000001u); fail += expect("invalid entry type", v, 0);
    v = valid_fst(); memcpy(v.data() + 37, "d/ta", 4); fail += expect("name contains slash", v, 0);
    v = valid_fst(); put32(v, 28, 0xFFFFFFFEu); fail += expect("file span overflow", v, 0);
    fail += expect("case-colliding siblings", colliding_fst(), 0);
    fail += expect("entry cap accepted", entry_limit_fst(), 1);
    v = valid_fst(); put32(v, 8, MP6_CONTENT_FST_MAX_ENTRIES + 1u);
    fail += expect("entry cap exceeded", v, 0);
    v.assign(MP6_CONTENT_FST_MAX_BYTES, 0);
    put32(v, 0, 0x01000000u); put32(v, 4, 0); put32(v, 8, 1);
    fail += expect("byte cap accepted", v, 1);
    v.push_back(0);
    fail += expect("byte cap exceeded", v, 0);
    v = valid_fst(); put32(v, 4, 1); fail += expect("root parent nonzero", v, 0);
    v = valid_fst(); memcpy(v.data() + 37, "d/ta", 4); fail += expect("slash in component", v, 0);
    fail += expect_boot("boot one byte short", MP6_CONTENT_BOOT_BYTES - 1u, 0);
    fail += expect_boot("boot exact size", MP6_CONTENT_BOOT_BYTES, 1);
    fail += expect_boot("boot one byte long", MP6_CONTENT_BOOT_BYTES + 1u, 0);
    {
        std::vector<unsigned char> boot(MP6_CONTENT_BOOT_BYTES);
        memcpy(boot.data(), "GP6P01", 6);
        if (mp6_content_boot_validate(boot.data(), boot.size())) {
            puts("FAIL boot wrong game ID");
            ++fail;
        }
    }
    if (fail) return 1;
    puts("[test_fst_validate] PASS: valid tree accepted; malformed counts, strings, directories, and names rejected");
    return 0;
}
