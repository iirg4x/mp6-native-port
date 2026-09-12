#include "dolphin.h"
#include "mp6_boot.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char model[1024], sprite[128], stackArray[256];
static int modelLive = 1, spriteLive = 1;
int mp6_heap_block_info(const void *p, int32_t *heap, uint32_t *size, uint32_t *tag)
{
    if (p != model && p != sprite) return 0;
    if ((p == model && !modelLive) || (p == sprite && !spriteLive)) return 0;
    if (heap) *heap = p == model ? 2 : 0;
    if (size) *size = p == model ? sizeof(model) : sizeof(sprite);
    if (tag) *tag = 42;
    return 1;
}
uint32_t mp6_heap_block_data_size(const void *p)
{
    uint32_t size = 0;
    mp6_heap_block_info(p, NULL, &size, NULL);
    return size;
}
#include "../../src/gx/gxarray_registry.c"

static int bindCount;
static const void *boundData;
static u32 boundSize;
static GXAttrType descriptor = GX_INDEX16;
static bool g_mp6InDlRecording;
static void *g_mp6DlRecBuf;
static bool mp6_arrayprobe_enabled(void) { return false; }
static void mp6_arrayprobe_log_setarray(GXAttr a, const void *p, u8 s, u8 path, u32 n) {}
#undef MP6_LOG_ONCE
#define MP6_LOG_ONCE(...) ((void)0)
#undef GXSetArray
#define GXSetArray testSetArray
static void testSetArray(GXAttr a, const void *p, u32 size, u8 stride, bool le)
{
    ++bindCount; boundData = p; boundSize = size;
}
void GXGetVtxDesc(GXAttr attr, GXAttrType *type) { *type = descriptor; }
#include "board_subject.inc"

#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
    /* The same tag in another heap must survive a model bulk free. */
    mp6_gxarray_register(model, 600);
    mp6_gxarray_register(sprite, 100);
    mp6_gxarray_before_bulk_free(2, 42);
    CHECK(mp6_gxarray_lookup(model) == 0);
    CHECK(mp6_gxarray_lookup(sprite) == 100);
    mp6_gxarray_register(model, 400);
    mp6_gxarray_before_direct_free(model);
    CHECK(mp6_gxarray_lookup(model) == 0);
    mp6_gxarray_register(model, 5000);
    CHECK(mp6_gxarray_lookup(model) == sizeof(model));
    /* Repeated load/unload must reclaim slots rather than exhaust the table. */
    for (uintptr_t i = 1; i <= 10000; ++i) {
        const void *p = (const void *)(i * 32);
        mp6_gxarray_register(p, 32);
        CHECK(mp6_gxarray_lookup(p) == 32);
        mp6_gxarray_before_direct_free(p);
        CHECK(mp6_gxarray_lookup(p) == 0);
    }

    /* A learned sprite bind must not overwrite a subsequent model bind. */
    mp6_GXSetArray3(GX_VA_POS, stackArray, 4);
    mp6_gx_array_slots_grow_for_draw(4);
    CHECK(boundData == stackArray && boundSize == 16);
    mp6_GXSetArray3(GX_VA_POS, model, 12);
    int count = bindCount;
    mp6_gx_array_slots_grow_for_draw(1000);
    CHECK(bindCount == count && boundData == model);
    CHECK(boundSize == sizeof(model));

    /* Heap-backed sprite arrays use their allocation bound, not nverts. */
    mp6_gxarray_before_direct_free(sprite);
    mp6_GXSetArray3(GX_VA_POS, sprite, 12);
    count = bindCount;
    mp6_gx_array_slots_grow_for_draw(1000);
    CHECK(bindCount == count && boundSize == sizeof(sprite));

    /* Rebinding the same unknown address discards the previous size hint. */
    mp6_GXSetArray3(GX_VA_POS, stackArray, 4);
    mp6_gx_array_slots_grow_for_draw(32);
    CHECK(boundSize == 128);
    mp6_GXSetArray3(GX_VA_POS, stackArray, 4);
    CHECK(boundSize == 0);
    descriptor = GX_DIRECT;
    count = bindCount;
    mp6_gx_array_slots_grow_for_draw(64);
    CHECK(bindCount == count);
    descriptor = GX_NONE;
    mp6_gx_array_slots_grow_for_draw(64);
    CHECK(bindCount == count);
    descriptor = GX_INDEX8;
    g_mp6InDlRecording = true;
    mp6_gx_array_slots_grow_for_draw(64);
    CHECK(bindCount == count);
    g_mp6InDlRecording = false;
    mp6_gx_array_slots_grow_for_draw(4);
    CHECK(boundSize == 16);
    /* Embedded particle arrays have no allocation header at their address.
     * DL replay must receive their exact span immediately, without GXBegin.
     * An explicit span must also discard the previous learned binding. */
    unsigned char ray[288] = {0};
    const void *vertices = ray + 64;
    CHECK(mp6_heap_block_data_size(vertices) == 0);
    mp6_GXSetArray3(GX_VA_POS, (void *)vertices, 12);
    CHECK(boundSize == 0); /* Original capsule bind: no GPU vertex upload. */
    mp6_gxarray_bind_span(GX_VA_POS, vertices, 16 * 12, 12);
    CHECK(boundData == vertices && boundSize == 192);
    count = bindCount;
    mp6_gx_array_slots_grow_for_draw(200);
    CHECK(bindCount == count && boundData == vertices && boundSize == 192);
    const void *colours = ray + 256;
    mp6_gxarray_bind_span(GX_VA_CLR0, colours, 8 * 4, 4);
    CHECK(boundData == colours && boundSize == 32);
    CHECK(mp6_gxarray_lookup(vertices) == 0); /* No stale lifetime entry. */
    mp6_gxarray_reset();
    CHECK(g_count == 0 && mp6_gxarray_lookup(model) == 0);
    puts("GX array lifetime and binding replacement: PASS");
    return 0;
}
