/* Structural validator for a raw GameCube File String Table.
 *
 * Both the importer and the runtime DVD layer consume user-provided fst.bin
 * bytes. Keep the validator header-only so the standalone regression test,
 * C dvd_files.c, and C++ content_import.cpp execute the identical checks.
 */
#ifndef MP6_CONTENT_FST_VALIDATE_H
#define MP6_CONTENT_FST_VALIDATE_H

#include "content_path_safe.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP6_CONTENT_FST_ENTRY_SIZE 12u
#define MP6_CONTENT_FST_MAX_ENTRIES (1u << 16)
#define MP6_CONTENT_FST_MAX_BYTES (8u * 1024u * 1024u)
#define MP6_CONTENT_FST_MAX_PATH 1024u
#define MP6_CONTENT_FST_MAX_DEPTH 256u
#define MP6_CONTENT_BOOT_BYTES 0x440u
/* Maximum logical byte address on a single-layer GameCube optical disc.
 * GP6E01's real tree has 961 entries; the caps above leave roughly 68x
 * entry headroom while keeping hostile validation allocations bounded. */
#define MP6_CONTENT_DISC_MAX_BYTES UINT64_C(1459978240)
#define MP6_CONTENT_WANTED_MAX_BYTES UINT64_C(1610612736)

typedef struct Mp6ContentFstDirFrame {
    uint32_t index;
    uint32_t end;
    size_t pathLen;
} Mp6ContentFstDirFrame;

typedef struct Mp6ContentFstNameKey {
    const unsigned char *name;
    uint64_t hash;
    uint32_t parent;
    uint32_t index;
} Mp6ContentFstNameKey;

static inline uint32_t mp6_content_fst_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int mp6_content_boot_validate(const void *bytes, size_t size)
{
    static const unsigned char gameId[6] = { 'G', 'P', '6', 'E', '0', '1' };
    return bytes != NULL && size == MP6_CONTENT_BOOT_BYTES &&
           memcmp(bytes, gameId, sizeof(gameId)) == 0;
}

static inline void mp6_content_fst_error(char *err, size_t errn, const char *fmt,
                                         unsigned long long a,
                                         unsigned long long b)
{
    if (err != NULL && errn > 0) {
        snprintf(err, errn, fmt, a, b);
    }
}

static inline uint64_t mp6_content_fst_name_hash(const unsigned char *name)
{
    uint64_t h = UINT64_C(1469598103934665603);
    while (*name != '\0') {
        h ^= (uint64_t)mp6_content_ascii_upper(*name++);
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static inline int mp6_content_fst_name_key_compare(const void *ap, const void *bp)
{
    const Mp6ContentFstNameKey *a = (const Mp6ContentFstNameKey *)ap;
    const Mp6ContentFstNameKey *b = (const Mp6ContentFstNameKey *)bp;
    if (a->parent != b->parent) return a->parent < b->parent ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    return a->index < b->index ? -1 : a->index != b->index;
}

static inline int mp6_content_fst_name_case_eq(const unsigned char *a,
                                                const unsigned char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (mp6_content_ascii_upper(*a++) != mp6_content_ascii_upper(*b++)) return 0;
    }
    return *a == *b;
}

/* Returns 1 only when every table/string/tree relation is safe for the
 * runtime's depth-first walker. The check deliberately includes portable
 * path-component rules: fst names later become host filesystem paths. */
static inline int mp6_content_fst_validate(const void *bytes, size_t size,
                                           char *err, size_t errn)
{
    const unsigned char *data = (const unsigned char *)bytes;
    uint32_t count;
    size_t tableBytes, stringBytes;
    const unsigned char *strings;
    Mp6ContentFstDirFrame *stack = NULL;
    size_t depth = 0;
    uint32_t i;

    if (err != NULL && errn > 0) err[0] = '\0';
    if (data == NULL || size < MP6_CONTENT_FST_ENTRY_SIZE) {
        mp6_content_fst_error(err, errn, "FST is too small (%llu bytes)",
                              (unsigned long long)size, 0);
        return 0;
    }
    if (size > MP6_CONTENT_FST_MAX_BYTES) {
        mp6_content_fst_error(err, errn, "FST exceeds the %llu-byte size limit",
                              (unsigned long long)MP6_CONTENT_FST_MAX_BYTES, 0);
        return 0;
    }
    if (mp6_content_fst_be32(data) != 0x01000000u) {
        mp6_content_fst_error(err, errn,
                              "FST root entry is not the canonical directory entry", 0, 0);
        return 0;
    }
    count = mp6_content_fst_be32(data + 8);
    if (mp6_content_fst_be32(data + 4) != 0) {
        mp6_content_fst_error(err, errn, "FST root directory has a nonzero parent index", 0, 0);
        return 0;
    }
    if (count == 0 || count > MP6_CONTENT_FST_MAX_ENTRIES ||
        (size_t)count > size / MP6_CONTENT_FST_ENTRY_SIZE) {
        mp6_content_fst_error(err, errn,
                              "FST entry count %llu does not fit in %llu bytes",
                              (unsigned long long)count, (unsigned long long)size);
        return 0;
    }
    tableBytes = (size_t)count * MP6_CONTENT_FST_ENTRY_SIZE;
    stringBytes = size - tableBytes;
    strings = data + tableBytes;

    stack = (Mp6ContentFstDirFrame *)malloc((size_t)count * sizeof(*stack));
    if (stack == NULL) {
        mp6_content_fst_error(err, errn, "out of memory validating %llu FST entries",
                              (unsigned long long)count, 0);
        return 0;
    }
    stack[depth++] = (Mp6ContentFstDirFrame){ 0, count, 0 };

    for (i = 1; i < count; ++i) {
        const unsigned char *entry = data + (size_t)i * MP6_CONTENT_FST_ENTRY_SIZE;
        uint32_t word0 = mp6_content_fst_be32(entry);
        uint32_t nameOff = word0 & 0x00FFFFFFu;
        uint32_t type = word0 >> 24;
        int isDir = type != 0;
        const unsigned char *nul;
        size_t nameLen, fullLen;

        while (depth > 0 && i >= stack[depth - 1].end) --depth;
        if (depth == 0) {
            mp6_content_fst_error(err, errn,
                                  "FST entry %llu lies outside the root subtree",
                                  (unsigned long long)i, 0);
            free(stack);
            return 0;
        }
        if (type > 1) {
            mp6_content_fst_error(err, errn,
                                  "FST entry %llu has invalid type %llu",
                                  (unsigned long long)i, (unsigned long long)type);
            free(stack);
            return 0;
        }
        if ((size_t)nameOff >= stringBytes) {
            mp6_content_fst_error(err, errn,
                                  "FST entry %llu name offset %llu is out of range",
                                  (unsigned long long)i, (unsigned long long)nameOff);
            free(stack);
            return 0;
        }
        nul = (const unsigned char *)memchr(strings + nameOff, '\0', stringBytes - nameOff);
        if (nul == NULL) {
            mp6_content_fst_error(err, errn,
                                  "FST entry %llu name is not NUL terminated", (unsigned long long)i, 0);
            free(stack);
            return 0;
        }
        nameLen = (size_t)(nul - (strings + nameOff));
        if (nameLen == 0 || !mp6_content_path_is_safe_component((const char *)(strings + nameOff))) {
            mp6_content_fst_error(err, errn,
                                  "FST entry %llu has an unsafe host filename", (unsigned long long)i, 0);
            free(stack);
            return 0;
        }
        fullLen = stack[depth - 1].pathLen +
                  (stack[depth - 1].pathLen != 0 ? 1u : 0u) + nameLen;
        if (fullLen >= MP6_CONTENT_FST_MAX_PATH) {
            mp6_content_fst_error(err, errn,
                                  "FST entry %llu path exceeds %llu bytes",
                                  (unsigned long long)i,
                                  (unsigned long long)(MP6_CONTENT_FST_MAX_PATH - 1));
            free(stack);
            return 0;
        }
        if (isDir) {
            uint32_t parent = mp6_content_fst_be32(entry + 4);
            uint32_t end = mp6_content_fst_be32(entry + 8);
            if (parent != stack[depth - 1].index || end <= i || end > stack[depth - 1].end) {
                mp6_content_fst_error(err, errn,
                                      "FST directory %llu has invalid parent/end %llu",
                                      (unsigned long long)i,
                                      ((unsigned long long)parent << 32) | end);
                free(stack);
                return 0;
            }
            if (depth >= MP6_CONTENT_FST_MAX_DEPTH) {
                mp6_content_fst_error(err, errn,
                                      "FST directory nesting exceeds %llu levels",
                                      (unsigned long long)MP6_CONTENT_FST_MAX_DEPTH, 0);
                free(stack);
                return 0;
            }
            stack[depth++] = (Mp6ContentFstDirFrame){ i, end, fullLen };
        } else {
            uint64_t fileEnd = (uint64_t)mp6_content_fst_be32(entry + 4) +
                               (uint64_t)mp6_content_fst_be32(entry + 8);
            if (fileEnd > MP6_CONTENT_DISC_MAX_BYTES) {
                mp6_content_fst_error(err, errn,
                                      "FST file %llu offset/size exceeds the GameCube disc range",
                                      (unsigned long long)i, 0);
                free(stack);
                return 0;
            }
        }
    }
    free(stack);

    /* Windows aliases sibling names that differ only in ASCII case. Reject
     * that ambiguity before extraction can overwrite one entry with another.
     * Re-walking the already-validated tree gives each compact key its parent
     * without materializing up to a million full paths. */
    if (count > 1) {
        Mp6ContentFstNameKey *keys = (Mp6ContentFstNameKey *)malloc(
            ((size_t)count - 1u) * sizeof(*keys));
        stack = (Mp6ContentFstDirFrame *)malloc((size_t)count * sizeof(*stack));
        if (keys == NULL || stack == NULL) {
            free(keys);
            free(stack);
            mp6_content_fst_error(err, errn,
                                  "out of memory checking %llu FST names",
                                  (unsigned long long)count, 0);
            return 0;
        }
        depth = 0;
        stack[depth++] = (Mp6ContentFstDirFrame){ 0, count, 0 };
        for (i = 1; i < count; ++i) {
            const unsigned char *entry = data + (size_t)i * MP6_CONTENT_FST_ENTRY_SIZE;
            const unsigned char *name = strings +
                (mp6_content_fst_be32(entry) & 0x00FFFFFFu);
            while (i >= stack[depth - 1].end) --depth;
            keys[i - 1].name = name;
            keys[i - 1].hash = mp6_content_fst_name_hash(name);
            keys[i - 1].parent = stack[depth - 1].index;
            keys[i - 1].index = i;
            if ((mp6_content_fst_be32(entry) >> 24) == 1u) {
                stack[depth++] = (Mp6ContentFstDirFrame){
                    i, mp6_content_fst_be32(entry + 8), 0
                };
            }
        }
        free(stack);
        qsort(keys, (size_t)count - 1u, sizeof(*keys), mp6_content_fst_name_key_compare);
        for (i = 1; i + 1u < count; ++i) {
            const Mp6ContentFstNameKey *a = &keys[i - 1];
            const Mp6ContentFstNameKey *b = &keys[i];
            if (a->parent == b->parent && a->hash == b->hash &&
                mp6_content_fst_name_case_eq(a->name, b->name)) {
                mp6_content_fst_error(err, errn,
                                      "FST sibling entries %llu and %llu collide on the host",
                                      (unsigned long long)a->index,
                                      (unsigned long long)b->index);
                free(keys);
                return 0;
            }
        }
        free(keys);
    }
    return 1;
}

#endif /* MP6_CONTENT_FST_VALIDATE_H */
