#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "game/hu3d.h"
#include "mp6_shadow_quality.h"
#include "mp6_enhancements.h"

HU3D_SHADOW Hu3DShadowBuf[HU3D_CAM_MAX];
s16 Hu3DShadowCamBit;
static s32 available = 1 << 24;
static int failAllocation, released, freed;
static void *retired;

void *HuMemHeapPtrGet(HEAPID heap) { assert(heap == HEAP_MODEL); return &available; }
s32 HuMemMaxMemorySizeGet(void *heap) { assert(heap == &available); return available; }
void *HuMemDirectMalloc(HEAPID heap, s32 size)
{
    assert(heap == HEAP_MODEL && size > 0);
    if (failAllocation) return NULL;
    uint32_t *allocation = malloc(sizeof(uint32_t) + size);
    assert(allocation);
    *allocation = size;
    return allocation + 1;
}
uint32_t mp6_heap_block_data_size(const void *ptr) { return ((const uint32_t *)ptr)[-1]; }
void HuMemDirectFree(void *ptr)
{
    assert(ptr == retired); /* renderer must retire its reference first */
    ++freed;
    free((uint32_t *)ptr - 1);
}
void GXDestroyCopyTex(void *ptr) { retired = ptr; ++released; }
void GXDestroyFrameBufferCache(void) {}
void GXCreateFrameBuffer(unsigned int w, unsigned int h) { (void)w; (void)h; }
void GXRestoreFrameBuffer(void) {}
void GXSetTexCopyMipGen(unsigned int enable) { (void)enable; }
void GXSetScissorRender(unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{ (void)x; (void)y; (void)w; (void)h; }

int main(void)
{
    Mp6EnhValues settings;
    mp6_enh_defaults(&settings);
    Hu3DShadowCamBit = 1;
    HU3D_SHADOW *shadow = &Hu3DShadowBuf[0];
    shadow->size = 192;
    shadow->buf = HuMemDirectMalloc(HEAP_MODEL, 192 * 192);
    mp6_shadow_latch_size(shadow->buf, 192);
    const int scales[] = {4, 8, 1, 2};
    for (unsigned i = 0; i < sizeof(scales)/sizeof(scales[0]); ++i) {
        settings.shadowQuality = scales[i];
        mp6_enh_set_values(&settings);
        mp6_shadow_apply_quality();
        assert(shadow->size == 192); /* authored projection must not change */
        assert(mp6_shadow_latched_size(shadow->buf, 192) == 192 * scales[i]);
        assert(released == (int)i + 1 && freed == released);
    }
    void *working = shadow->buf;
    settings.shadowQuality = 16;
    mp6_enh_set_values(&settings);
    failAllocation = 1;
    mp6_shadow_apply_quality();
    assert(shadow->buf == working && released == 4);
    failAllocation = 0;
    available = 1;
    mp6_shadow_apply_quality();
    assert(shadow->buf == working && released == 4);
    Hu3DShadowCamBit = 0;
    mp6_shadow_apply_quality();
    assert(shadow->buf == working);
    mp6_shadow_release_buffer(working);
    HuMemDirectFree(working);
    puts("live shadow allocation: PASS");
    return 0;
}
