/* MP6 native port -- GXSetArray real-size registry implementation. See
 * shim/include/mp6_gxarray_registry.h for the full story. Deliberately
 * dependency-free (no decomp or Aurora headers) -- compiled into BOTH
 * build modes (--headless and default/Aurora) since
 * platform/hsf/hsf_load_native.c, the only current writer, is itself
 * common to both; the --headless build simply never has a reader
 * (its null GXSetArray shim doesn't consult this at all), which is
 * harmless -- the table just goes unread there.
 *
 * A plain linear-scan array is deliberately simple: at most a few hundred
 * entries exist at once in real play (4 arrays -- vertex/normal/st/color --
 * per loaded HSF model, times a few dozen models), and both register()
 * (once per array, at model-load time) and lookup() (once per GXSetArray
 * call, a handful of times per drawn face) are cold/infrequent enough that
 * O(n) is not a meaningful cost next to everything else a single game
 * frame does. */
#include "mp6_gxarray_registry.h"
#include <string.h>

#define MP6_GXARRAY_MAX 8192

typedef struct {
    const void *data;
    uint32_t byteSize;
} MP6GXArrayEntry;

static MP6GXArrayEntry g_entries[MP6_GXARRAY_MAX];
static int g_count = 0;

void mp6_gxarray_register(const void *data, uint32_t byteSize)
{
    if (!data || byteSize == 0) {
        return;
    }
    /* Re-registering the same pointer (shouldn't normally happen -- each
     * HSF buffer is a fresh allocation -- but harmless if it ever does,
     * e.g. a future caller reusing a freed address) just updates in place
     * instead of growing the table unboundedly. */
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].data == data) {
            g_entries[i].byteSize = byteSize;
            return;
        }
    }
    if (g_count < MP6_GXARRAY_MAX) {
        g_entries[g_count].data = data;
        g_entries[g_count].byteSize = byteSize;
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
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].data == data) {
            return g_entries[i].byteSize;
        }
    }
    return 0;
}
