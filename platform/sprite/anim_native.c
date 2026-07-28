/* Safe native ANM loader/cache.
 *
 * Retail ANM files are packed for a 32-bit big-endian ABI. The original
 * HuSprAnimRead fixed pointer offsets up in place, so the returned object,
 * its packed backing bytes, and its allocation lifetime were one object.
 * On a 64-bit host we must build a separate native pointer graph. This TU
 * preserves the original ownership contract explicitly: every parsed graph
 * records its packed backing allocation, inherits that allocation's tag,
 * and frees both pieces together. */
#include "mp6_anim_native.h"

#include "game/memory.h"
#include "mp6_alloc_size.h"
#include "mp6_boot.h"

#include "anim_validate_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MP6AnimRecord_s {
    void *raw;
    ANIMDATA *anim;
    int32_t rawHeap;
    int32_t animHeap;
    uint32_t rawTag;
    uint32_t animTag;
    unsigned rawOwned : 1;
} MP6AnimRecord;

static MP6AnimRecord *s_animRecords;
static size_t s_animRecordCapacity;
static size_t s_animRecordLive;

static void mp6_anim_fatal(const char *what)
{
    fprintf(stderr, "[FATAL] ANM native loader: %s\n", what);
    fflush(stderr);
    _Exit(EXIT_FAILURE);
}

static MP6AnimRecord *mp6_anim_find_key(const void *key)
{
    size_t i;
    if (key == NULL) return NULL;
    for (i = 0; i < s_animRecordCapacity; ++i) {
        MP6AnimRecord *record = &s_animRecords[i];
        if (record->anim != NULL
                && (record->raw == key || record->anim == key)) {
            return record;
        }
    }
    return NULL;
}

static MP6AnimRecord *mp6_anim_record_slot(void)
{
    size_t i;
    MP6AnimRecord *grown;
    size_t oldCapacity = s_animRecordCapacity;
    size_t newCapacity;

    for (i = 0; i < oldCapacity; ++i) {
        if (s_animRecords[i].anim == NULL) return &s_animRecords[i];
    }
    newCapacity = oldCapacity == 0 ? 64u : oldCapacity * 2u;
    if (newCapacity <= oldCapacity
            || newCapacity > SIZE_MAX / sizeof(*s_animRecords)) {
        mp6_anim_fatal("cache capacity overflow");
    }
    grown = (MP6AnimRecord *)realloc(
        s_animRecords, newCapacity * sizeof(*s_animRecords));
    if (grown == NULL) {
        /* Never return a native graph that was not self-registered. Without
         * this record, a future HuSprAnimRead(anim) would reinterpret host
         * pointers as packed offsets. */
        mp6_anim_fatal("cache growth allocation failed");
    }
    s_animRecords = grown;
    memset(&s_animRecords[oldCapacity], 0,
           (newCapacity - oldCapacity) * sizeof(*s_animRecords));
    s_animRecordCapacity = newCapacity;
    return &s_animRecords[oldCapacity];
}

static void mp6_anim_record_add(void *raw, ANIMDATA *anim,
                                int rawOwned, int32_t rawHeap,
                                uint32_t rawTag, int32_t animHeap,
                                uint32_t animTag)
{
    MP6AnimRecord *record;
    if (raw == NULL || anim == NULL) mp6_anim_fatal("null cache identity");
    if (mp6_anim_find_key(raw) != NULL || mp6_anim_find_key(anim) != NULL) {
        mp6_anim_fatal("duplicate cache identity");
    }
    record = mp6_anim_record_slot();
    record->raw = raw;
    record->anim = anim;
    record->rawOwned = rawOwned != 0;
    record->rawHeap = rawHeap;
    record->rawTag = rawTag;
    record->animHeap = animHeap;
    record->animTag = animTag;
    ++s_animRecordLive;
}

static ANIMDATA *mp6_anim_cache_hit(void *data)
{
    MP6AnimRecord *record = mp6_anim_find_key(data);
    if (record == NULL) return NULL;
    if (record->anim->useNum == INT16_MAX) {
        mp6_anim_fatal("reference count overflow");
    }
    ++record->anim->useNum;
    return record->anim;
}

static int mp6_anim_checked_add(size_t *total, size_t count, size_t stride)
{
    size_t bytes;
    if (stride != 0 && count > SIZE_MAX / stride) return 0;
    bytes = count * stride;
    if (*total > SIZE_MAX - bytes) return 0;
    *total += bytes;
    return 1;
}

static void *mp6_anim_alloc(size_t size, int32_t heap, uint32_t tag)
{
    int32_t request;
    int32_t checked;
    void *result;
    if (size > (size_t)INT32_MAX) mp6_anim_fatal("native allocation overflow");
    request = (int32_t)size;
    if (!mp6_humem_checked_request(request, &checked)) {
        mp6_anim_fatal("native allocation exceeds allocator contract");
    }
    result = HuMemDirectMallocNum((HEAPID)heap, request, tag);
    if (result == NULL) mp6_anim_fatal("game heap exhausted");
    return result;
}

static ANIMDATA *mp6_anim_make_inert(void *raw, int rawOwned,
                                     int32_t rawHeap, uint32_t rawTag,
                                     int32_t animHeap, uint32_t animTag)
{
    size_t skeletonSize = sizeof(ANIMDATA) + sizeof(ANIMBANK)
                        + sizeof(ANIMPAT) + sizeof(ANIMBMP)
                        + sizeof(ANIMFRAME) + sizeof(ANIMLAYER);
    uint8_t *cursor;
    ANIMDATA *anim = (ANIMDATA *)mp6_anim_alloc(
        skeletonSize, animHeap, animTag);
    ANIMBANK *bank;
    ANIMPAT *pat;
    ANIMBMP *bmp;
    ANIMFRAME *frame;
    ANIMLAYER *layer;
    void *pixels = mp6_anim_alloc(32u, animHeap, animTag);

    memset(anim, 0, skeletonSize);
    memset(pixels, 0, 32u);
    cursor = (uint8_t *)anim + sizeof(*anim);
    bank = (ANIMBANK *)cursor; cursor += sizeof(*bank);
    pat = (ANIMPAT *)cursor; cursor += sizeof(*pat);
    bmp = (ANIMBMP *)cursor; cursor += sizeof(*bmp);
    frame = (ANIMFRAME *)cursor; cursor += sizeof(*frame);
    layer = (ANIMLAYER *)cursor;

    anim->bankNum = 1;
    anim->patNum = 1;
    anim->bmpNum = (s16)(1 | ANIM_BMP_ALLOC);
    anim->bank = bank;
    anim->pat = pat;
    anim->bmp = bmp;
    bank->timeNum = 1;
    bank->unk = 1;
    bank->frame = frame;
    frame->pat = 0;
    frame->time = 1;
    pat->layerNum = 1;
    pat->centerX = 4;
    pat->centerY = 4;
    pat->sizeX = 8;
    pat->sizeY = 8;
    pat->layer = layer;
    layer->alpha = 0;
    layer->bmpNo = 0;
    layer->sizeX = 8;
    layer->sizeY = 8;
    layer->vtx[2] = 8;
    layer->vtx[4] = 8;
    layer->vtx[5] = 8;
    layer->vtx[7] = 8;
    bmp->pixSize = 4;
    bmp->dataFmt = ANIM_BMP_I4;
    bmp->sizeX = 8;
    bmp->sizeY = 8;
    bmp->dataSize = 32;
    bmp->data = pixels;

    mp6_anim_record_add(raw, anim, rawOwned, rawHeap, rawTag,
                        animHeap, animTag);
    return anim;
}

static ANIMDATA *mp6_anim_parse(void *data, size_t explicitSize,
                                int hasExplicitSize)
{
    uint8_t *raw = (uint8_t *)data;
    uint32_t blockSize = 0;
    uint32_t rawTag = (uint32_t)-256;
    uint32_t animTag;
    int32_t animHeap;
    int32_t rawHeap = -1;
    int rawOwned;
    size_t capacity;
    MP6AnimValidated valid;
    char why[160];
    size_t skeletonSize = sizeof(ANIMDATA);
    uint8_t *cursor;
    ANIMDATA *anim;
    ANIMBANK *banks;
    ANIMPAT *patterns;
    ANIMBMP *bitmaps;
    int32_t i;

    anim = mp6_anim_cache_hit(data);
    if (anim != NULL) return anim;

    rawOwned = mp6_heap_block_info(data, &rawHeap, &blockSize, &rawTag);
    if (hasExplicitSize) {
        capacity = explicitSize;
        if (rawOwned && explicitSize > blockSize) {
            capacity = 0;
        }
    } else {
        capacity = rawOwned ? (size_t)blockSize : 0u;
    }
    animTag = rawOwned ? rawTag : (uint32_t)-256;
    /* The original in-place object lived in exactly the input block's heap
     * and tag. Keep both coordinates, not merely the tag, so one tagged
     * bulk-free operation reclaims raw bytes and native graph atomically. */
    animHeap = rawOwned ? rawHeap : HEAP_MODEL;

    if (!mp6_anim_preflight(data, capacity, &valid, why, sizeof(why))) {
        fprintf(stderr, "[ANM] rejected malformed/unsized sprite: %s\n",
                why[0] != '\0' ? why : "unknown validation failure");
        fflush(stderr);
        return mp6_anim_make_inert(data, rawOwned, rawHeap, rawTag,
                                   animHeap, animTag);
    }

    if (!mp6_anim_checked_add(&skeletonSize, (size_t)valid.bankNum,
                              sizeof(ANIMBANK))
            || !mp6_anim_checked_add(&skeletonSize, (size_t)valid.patNum,
                                     sizeof(ANIMPAT))
            || !mp6_anim_checked_add(&skeletonSize, (size_t)valid.bmpNum,
                                     sizeof(ANIMBMP))
            || !mp6_anim_checked_add(&skeletonSize, valid.frameTotal,
                                     sizeof(ANIMFRAME))
            || !mp6_anim_checked_add(&skeletonSize, valid.layerTotal,
                                     sizeof(ANIMLAYER))) {
        mp6_anim_fatal("native skeleton size overflow");
    }

    anim = (ANIMDATA *)mp6_anim_alloc(skeletonSize, animHeap, animTag);
    memset(anim, 0, skeletonSize);
    cursor = (uint8_t *)anim + sizeof(*anim);
    banks = (ANIMBANK *)cursor;
    cursor += (size_t)valid.bankNum * sizeof(*banks);
    patterns = (ANIMPAT *)cursor;
    cursor += (size_t)valid.patNum * sizeof(*patterns);
    bitmaps = (ANIMBMP *)cursor;
    cursor += (size_t)valid.bmpNum * sizeof(*bitmaps);

    anim->bankNum = valid.bankNum;
    anim->patNum = valid.patNum;
    anim->bmpNum = valid.bmpNum;
    anim->useNum = 0;
    anim->bank = banks;
    anim->pat = patterns;
    anim->bmp = bitmaps;

    for (i = 0; i < valid.bankNum; ++i) {
        const uint8_t *src = raw + valid.bankOffset
                           + (size_t)i * MP6_ANIM_BANK_SIZE;
        int32_t count = mp6_anim_v_s16(src);
        uint32_t offset = mp6_anim_v_u32(src + 4);
        ANIMFRAME *frames = (ANIMFRAME *)cursor;
        int32_t j;
        cursor += (size_t)count * sizeof(*frames);
        banks[i].timeNum = (s16)count;
        banks[i].unk = (s16)mp6_anim_v_s16(src + 2);
        banks[i].frame = frames;
        for (j = 0; j < count; ++j) {
            const uint8_t *packed = raw + offset
                                  + (size_t)j * MP6_ANIM_FRAME_SIZE;
            frames[j].pat = (s16)mp6_anim_v_s16(packed);
            frames[j].time = (s16)mp6_anim_v_s16(packed + 2);
            frames[j].shiftX = (s16)mp6_anim_v_s16(packed + 4);
            frames[j].shiftY = (s16)mp6_anim_v_s16(packed + 6);
            frames[j].flip = (s16)mp6_anim_v_s16(packed + 8);
            frames[j].pad = (s16)mp6_anim_v_s16(packed + 10);
        }
    }

    for (i = 0; i < valid.patNum; ++i) {
        const uint8_t *src = raw + valid.patOffset
                           + (size_t)i * MP6_ANIM_PAT_SIZE;
        int32_t count = mp6_anim_v_s16(src);
        uint32_t offset = mp6_anim_v_u32(src + 12);
        ANIMLAYER *layers = (ANIMLAYER *)cursor;
        int32_t j;
        cursor += (size_t)count * sizeof(*layers);
        patterns[i].layerNum = (s16)count;
        patterns[i].centerX = (s16)mp6_anim_v_s16(src + 2);
        patterns[i].centerY = (s16)mp6_anim_v_s16(src + 4);
        patterns[i].sizeX = (s16)mp6_anim_v_s16(src + 6);
        patterns[i].sizeY = (s16)mp6_anim_v_s16(src + 8);
        patterns[i].layer = layers;
        for (j = 0; j < count; ++j) {
            const uint8_t *packed = raw + offset
                                  + (size_t)j * MP6_ANIM_LAYER_SIZE;
            int32_t k;
            layers[j].alpha = packed[0];
            layers[j].flip = packed[1];
            layers[j].bmpNo = (s16)mp6_anim_v_s16(packed + 2);
            layers[j].startX = (s16)mp6_anim_v_s16(packed + 4);
            layers[j].startY = (s16)mp6_anim_v_s16(packed + 6);
            layers[j].sizeX = (s16)mp6_anim_v_s16(packed + 8);
            layers[j].sizeY = (s16)mp6_anim_v_s16(packed + 10);
            layers[j].shiftX = (s16)mp6_anim_v_s16(packed + 12);
            layers[j].shiftY = (s16)mp6_anim_v_s16(packed + 14);
            for (k = 0; k < 8; ++k) {
                layers[j].vtx[k] = (s16)mp6_anim_v_s16(
                    packed + 16 + (size_t)k * 2u);
            }
        }
    }

    for (i = 0; i < valid.bmpNum; ++i) {
        const uint8_t *src = raw + valid.bmpOffset
                           + (size_t)i * MP6_ANIM_BMP_SIZE;
        uint32_t paletteOffset = mp6_anim_v_u32(src + 12);
        uint32_t dataOffset = mp6_anim_v_u32(src + 16);
        bitmaps[i].pixSize = src[0];
        bitmaps[i].dataFmt = src[1];
        bitmaps[i].palNum = (s16)mp6_anim_v_s16(src + 2);
        bitmaps[i].sizeX = (s16)mp6_anim_v_s16(src + 4);
        bitmaps[i].sizeY = (s16)mp6_anim_v_s16(src + 6);
        bitmaps[i].dataSize = mp6_anim_v_u32(src + 8);
        bitmaps[i].palData = paletteOffset != 0
                           ? (void *)(raw + paletteOffset) : NULL;
        bitmaps[i].data = (void *)(raw + dataOffset);
    }

    mp6_anim_record_add(data, anim, rawOwned, rawHeap, rawTag,
                        animHeap, animTag);
    return anim;
}

ANIMDATA *mp6_anim_read(void *data)
{
    return mp6_anim_parse(data, 0, 0);
}

ANIMDATA *mp6_anim_read_sized(void *data, size_t size)
{
    return mp6_anim_parse(data, size, 1);
}

void mp6_anim_register_native(ANIMDATA *anim)
{
    int32_t heap = -1;
    uint32_t size = 0;
    uint32_t tag = 0;
    if (anim == NULL) mp6_anim_fatal("attempted to register null native graph");
    if (mp6_anim_find_key(anim) != NULL) return;
    if (!mp6_heap_block_info(anim, &heap, &size, &tag) || size < sizeof(*anim)) {
        mp6_anim_fatal("native graph is not a live allocation base");
    }
    mp6_anim_record_add(anim, anim, 0, heap, tag, heap, tag);
}

void *mp6_anim_unregister_for_free(ANIMDATA *anim)
{
    MP6AnimRecord *record = mp6_anim_find_key(anim);
    void *raw;
    if (record == NULL || record->anim != anim) {
        mp6_anim_fatal("free of unregistered native graph");
    }
    raw = record->rawOwned && record->raw != (void *)anim
        ? record->raw : NULL;
    memset(record, 0, sizeof(*record));
    --s_animRecordLive;
    return raw;
}

void mp6_anim_before_direct_free(const void *ptr)
{
    if (mp6_anim_find_key(ptr) != NULL) {
        mp6_anim_fatal("direct free bypassed ANM ownership teardown");
    }
}

void mp6_anim_before_bulk_free(int heap, uint32_t tag)
{
    size_t i;
    for (i = 0; i < s_animRecordCapacity; ++i) {
        MP6AnimRecord *record = &s_animRecords[i];
        int rawMatch;
        int animMatch;
        if (record->anim == NULL) continue;
        rawMatch = record->rawOwned && record->rawHeap == heap
                && record->rawTag == tag;
        animMatch = record->animHeap == heap && record->animTag == tag;
        if (!rawMatch && !animMatch) continue;
        if (record->rawOwned && record->raw != (void *)record->anim
                && rawMatch != animMatch) {
            mp6_anim_fatal("bulk free would split native graph from backing bytes");
        }
        memset(record, 0, sizeof(*record));
        --s_animRecordLive;
    }
}

size_t mp6_anim_cache_live_count(void)
{
    return s_animRecordLive;
}
