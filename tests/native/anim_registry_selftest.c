/* Include the production implementation so the test observes its real cache. */
#include "game/memory.h"
#include "mp6_boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static union { max_align_t alignment; unsigned char bytes[1048576]; } arena;
static size_t arenaUsed;
static struct { void *pointer; uint32_t size, tag; int live; } blocks[1024];
static size_t blockCount;

void *HuMemDirectMallocNum(HEAPID heap, s32 size, u32 tag)
{
    (void)heap;
    size_t rounded=((size_t)size+31)&~(size_t)31;
    if (size<=0 || arenaUsed+rounded>sizeof(arena.bytes) || blockCount==1024) abort();
    void *p=arena.bytes+arenaUsed;
    blocks[blockCount].pointer=p; blocks[blockCount].size=size;
    blocks[blockCount].tag=tag; blocks[blockCount++].live=1;
    arenaUsed+=rounded;
    return p;
}
int mp6_heap_block_info(const void *p, int32_t *heap, uint32_t *size, uint32_t *tag)
{
    for (size_t i=0;i<blockCount;i++) if (blocks[i].live && blocks[i].pointer==p) {
        if(heap) *heap=HEAP_HEAP;
        if(size) *size=blocks[i].size;
        if(tag) *tag=blocks[i].tag;
        return 1;
    }
    return 0;
}

#include "../../src/sprite/anim_native.c"

void HuMemDirectFree(void *p)
{
    mp6_anim_before_direct_free(p);
    for (size_t i=0;i<blockCount;i++) if (blocks[i].live && blocks[i].pointer==p) {
        blocks[i].live=0; return;
    }
    abort();
}
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    ANIMDATA *animations[70];
    for (int i=0;i<70;i++) {
        animations[i]=HuMemDirectMallocNum(HEAP_HEAP,sizeof(ANIMDATA),123);
        memset(animations[i],0,sizeof(ANIMDATA));
        mp6_anim_register_native(animations[i]);
    }
    CHECK(s_animRecordCapacity==128 && s_animRecordLive==70);
    CHECK(mp6_heap_block_info(s_animRecords,NULL,NULL,NULL));
    static unsigned char savedArena[sizeof(arena.bytes)];
    memcpy(savedArena,arena.bytes,sizeof(savedArena));
    MP6AnimRecord *savedRecords=s_animRecords;
    size_t savedCapacity=s_animRecordCapacity, savedLive=s_animRecordLive;
    for (int i=0;i<200;i++) {
        ANIMDATA *anim=HuMemDirectMallocNum(HEAP_HEAP,sizeof(ANIMDATA),123);
        memset(anim,0,sizeof(*anim)); mp6_anim_register_native(anim);
    }
    CHECK(s_animRecords!=savedRecords && s_animRecordCapacity==512);
    animations[0]->useNum=25;
    /* The quick-save mechanism restores image globals plus the game arena. */
    memcpy(arena.bytes,savedArena,sizeof(savedArena));
    s_animRecords=savedRecords; s_animRecordCapacity=savedCapacity; s_animRecordLive=savedLive;
    for (int i=0;i<70;i++) {
        CHECK(mp6_anim_cache_hit(animations[i])==animations[i]);
        CHECK(animations[i]->useNum==1);
    }
    CHECK(mp6_anim_cache_live_count()==70);
    mp6_anim_before_bulk_free(HEAP_HEAP,123);
    CHECK(mp6_anim_cache_live_count()==0);
    puts("animation registry arena restore: PASS");
    return 0;
}
