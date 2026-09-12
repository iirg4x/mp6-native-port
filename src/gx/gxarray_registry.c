/* MP6 native port -- GXSetArray real-size registry implementation. See
 * include/mp6_gxarray_registry.h for the full story. Deliberately
 * dependency-free (no decomp or Aurora headers) -- compiled into BOTH
 * build modes (--headless and default/Aurora) since
 * src/hsf/hsf_load_native.c, the only current writer, is itself
 * common to both; the --headless build simply never has a reader
 * (its null GXSetArray shim doesn't consult this at all), which is
 * harmless -- the table just goes unread there.
 *
 * GXSetArray is a hot path: a board frame performs hundreds of lookups.
 * Hash chains index the dense lifetime table without allocating. Both the
 * table and index are ordinary game globals, restored together by quick saves. */
#include "mp6_gxarray_registry.h"
#include "mp6_boot.h"
#include <string.h>

#define MP6_GXARRAY_MAX 8192
#define MP6_GXARRAY_BUCKETS 16384

typedef struct {
    const void *data;
    uint32_t byteSize;
    int32_t heap;
    uint32_t tag;
    int next; /* entry index + 1; zero terminates the chain */
} MP6GXArrayEntry;

static MP6GXArrayEntry g_entries[MP6_GXARRAY_MAX];
static int g_count = 0;
static int g_buckets[MP6_GXARRAY_BUCKETS];

static uint32_t mp6_gxarray_bucket(const void *data)
{
    uintptr_t key = (uintptr_t)data;
    /* Drop allocation alignment and mix high bits on both 32/64-bit hosts. */
    key >>= 4;
    key ^= key >> 17;
    key *= (uintptr_t)0x9e3779b1u;
    key ^= key >> 16;
    return (uint32_t)key & (MP6_GXARRAY_BUCKETS - 1);
}

static int *mp6_gxarray_link(const void *data)
{
    int *link = &g_buckets[mp6_gxarray_bucket(data)];
    while (*link && g_entries[*link - 1].data != data)
        link = &g_entries[*link - 1].next;
    return link;
}

#ifdef MP6_HEADLESS_BUILD
void mp6_gxarray_bind_span(int attr, const void *data, uint32_t byteSize,
                         uint8_t stride)
{
    (void)attr; (void)data; (void)byteSize; (void)stride;
}
#endif

void mp6_gxarray_reset(void)
{
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_buckets, 0, sizeof(g_buckets));
    g_count = 0;
}

static void mp6_gxarray_remove(int index)
{
    int *link = mp6_gxarray_link(g_entries[index].data);
    *link = g_entries[index].next;
    --g_count;
    if (index != g_count) {
        /* Repair the link to the dense table's last entry before moving it.
         * This also works when both entries share a hash chain. */
        link = mp6_gxarray_link(g_entries[g_count].data);
        *link = index + 1;
        g_entries[index] = g_entries[g_count];
    }
    memset(&g_entries[g_count], 0, sizeof(g_entries[g_count]));
}

void mp6_gxarray_before_direct_free(const void *data)
{
    int index = *mp6_gxarray_link(data);
    if (index) mp6_gxarray_remove(index - 1);
}

void mp6_gxarray_before_bulk_free(int heap, uint32_t tag)
{
    for (int i = 0; i < g_count;) {
        if (g_entries[i].heap == heap && g_entries[i].tag == tag)
            mp6_gxarray_remove(i);
        else i++;
    }
}

void mp6_gxarray_register(const void *data, uint32_t byteSize)
{
    int32_t heap = -1;
    uint32_t allocationSize = 0, tag = 0;
    if (!data || byteSize == 0) {
        return;
    }
    /* Keep lifetime metadata beside the pointer in restorable game globals.
     * Reusing a model's address must not resurrect its previous array size. */
    if (mp6_heap_block_info(data, &heap, &allocationSize, &tag) &&
        byteSize > allocationSize) byteSize = allocationSize;
    /* Re-registering the same pointer (shouldn't normally happen -- each
     * HSF buffer is a fresh allocation -- but harmless if it ever does,
     * e.g. a future caller reusing a freed address) just updates in place
     * instead of growing the table unboundedly. */
    int *link = mp6_gxarray_link(data);
    if (*link) {
        MP6GXArrayEntry *entry = &g_entries[*link - 1];
        entry->byteSize = byteSize;
        entry->heap = heap;
        entry->tag = tag;
        return;
    }
    if (g_count < MP6_GXARRAY_MAX) {
        g_entries[g_count].data = data;
        g_entries[g_count].byteSize = byteSize;
        g_entries[g_count].heap = heap;
        g_entries[g_count].tag = tag;
        g_entries[g_count].next = 0;
        *link = g_count + 1;
        g_count++;
    }
    /* Table full: silently drop (matches this file's own "cold path,
     * generous fixed bound" design -- see MP6_GXARRAY_MAX). A dropped
     * entry just makes mp6_gxarray_lookup() return 0 for that pointer, the
     * same safe "unknown size" fallback every caller already handles, not
     * a new failure mode. */
}

uint32_t mp6_gxarray_lookup(const void *data)
{
    if (!data) {
        return 0;
    }
    int index = *mp6_gxarray_link(data);
    return index ? g_entries[index - 1].byteSize : 0;
}
