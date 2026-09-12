#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/gx/gxarray_registry.c"

int mp6_heap_block_info(const void *ptr, int32_t *heap, uint32_t *size, uint32_t *tag)
{
    uintptr_t key=(uintptr_t)ptr/32;
    if (heap) *heap=(int)(key%4);
    if (tag) *tag=(uint32_t)(key%13);
    if (size) *size=4096;
    return ptr!=NULL;
}

static const void *address(int i) { return (const void *)(uintptr_t)(0x100000u+32u*i); }
static uint32_t expected[MP6_GXARRAY_MAX+1];

static void check(void)
{
    int count=0,seen[MP6_GXARRAY_MAX]={0};
    for (int b=0;b<MP6_GXARRAY_BUCKETS;b++) {
        for(int n=g_buckets[b];n;n=g_entries[n-1].next) {
            assert(n>0 && n<=g_count);
            assert(!seen[n-1]++);
            assert(mp6_gxarray_bucket(g_entries[n-1].data)==(unsigned)b);
            count++;
        }
    }
    assert(count==g_count);
    for(int i=0;i<=MP6_GXARRAY_MAX;i++) assert(mp6_gxarray_lookup(address(i))==expected[i]);
    assert(mp6_gxarray_lookup(NULL)==0);
}

int main(void)
{
    mp6_gxarray_reset();
    mp6_gxarray_register(NULL,12);
    mp6_gxarray_register(address(0),0);
    assert(g_count==0);
    for (int i=0;i<MP6_GXARRAY_MAX;i++) {
        mp6_gxarray_register(address(i),(unsigned)i+1);
        expected[i]=(unsigned)i+1>4096 ? 4096 : (unsigned)i+1;
    }
    mp6_gxarray_register(address(MP6_GXARRAY_MAX),17); /* Full table fallback. */
    mp6_gxarray_register(address(0),55); expected[0]=55; /* Updates even when full. */
    check();
    MP6GXArrayEntry *snapshot=malloc(sizeof(g_entries));
    int *buckets=malloc(sizeof(g_buckets));
    assert(snapshot && buckets);
    memcpy(snapshot,g_entries,sizeof(g_entries)); memcpy(buckets,g_buckets,sizeof(g_buckets));
    /* Dense moves across and within colliding chains, checked against an oracle. */
    unsigned random=1234567;
    for (int round=0;round<20000;round++) {
        random=random*1664525u+1013904223u;
        int i=(int)((random>>8)%MP6_GXARRAY_MAX);
        if (random&1) { mp6_gxarray_before_direct_free(address(i)); expected[i]=0; }
        else { mp6_gxarray_register(address(i),99); expected[i]=99; }
        if (round%500==0) check();
    }
    check();
    memcpy(g_entries,snapshot,sizeof(g_entries)); memcpy(g_buckets,buckets,sizeof(g_buckets));
    g_count=MP6_GXARRAY_MAX;
    for(int i=0;i<MP6_GXARRAY_MAX;i++) expected[i]=i+1>4096 ? 4096 : i+1;
    expected[0]=55; check(); /* Quick-state restores index + lifetime table together. */
    for(int heap=0;heap<4;heap++) for(unsigned tag=0;tag<13;tag++) {
        mp6_gxarray_before_bulk_free(heap,tag);
        for(int i=0;i<MP6_GXARRAY_MAX;i++) {
            uintptr_t key=(uintptr_t)address(i)/32;
            if (key%4==(unsigned)heap && key%13==tag) expected[i]=0;
        }
        check();
    }
    assert(g_count==0);
    mp6_gxarray_register(address(0),7000); assert(mp6_gxarray_lookup(address(0))==4096);
    mp6_gxarray_before_direct_free(address(0));
    mp6_gxarray_register(address(0),8); assert(mp6_gxarray_lookup(address(0))==8);
    mp6_gxarray_reset(); assert(g_count==0 && mp6_gxarray_lookup(address(0))==0);
    free(snapshot); free(buckets);
    puts("GX array hash: bounds, collisions, capacity, lifetime, reuse and restore PASS");
}
