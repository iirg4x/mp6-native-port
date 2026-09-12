#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "game/data.h"
#include "game/memory.h"
#include "be.h"
#define OSRoundUp32B(value) (((value)+31)&~31)
#define OSRoundDown32B(value) ((value)&~31)

static unsigned char archive[4096];
static int archive_len, expected_offset, bad_read, decoded, closed;
static int token;
void OSReport(const char *format, ...) { (void)format; }
void *HuMemDirectMalloc(HEAPID heap, s32 size) { (void)heap; return malloc(size); }
void HuMemDirectFree(void *data) { free(data); }
BOOL HuMemMemoryFileSet(void *ptr, u32 num) { (void)ptr; (void)num; return TRUE; }
BOOL DVDClose(DVDFileInfo *info) { (void)info; closed++; return TRUE; }
BOOL HuDataDVDdirDirectOpen(s32 number, DVDFileInfo *info)
{
    (void)number;
    info->length = archive_len;
    return TRUE;
}
BOOL HuDataDVDdirDirectRead(DVDFileInfo *info, void *dst, s32 len, s32 offset)
{
    (void)info;
    if (offset < 0 || len <= 0 || len > sizeof(archive) ||
        (unsigned)offset > sizeof(archive)-(unsigned)len) {
        bad_read++;
        return FALSE;
    }
    memcpy(dst,archive+offset,len);
    return TRUE;
}
void *HuDataDecodeIt(void *buf, s32 offset, s32 num, HEAPID heap)
{
    (void)num; (void)heap;
    if (offset != (expected_offset & 31) ||
        memcmp((unsigned char *)buf+offset,archive+expected_offset,16)) {
        bad_read++;
        return NULL;
    }
    decoded++;
    return &token;
}
#include "board_subject.inc"
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n",__LINE__,#c); return 1; } } while (0)
static void put32(int pos, unsigned n)
{
    archive[pos] = n>>24; archive[pos+1] = n>>16;
    archive[pos+2] = n>>8; archive[pos+3] = n;
}
int main(void)
{
    /* 128 becomes a negative native s32 without byte swapping. Exercise
     * first/middle/last entries and both the next-offset and EOF branches. */
    for (int count=128;count<=129;count++) {
        memset(archive,0,sizeof(archive));
        put32(0,count);
        archive_len = 4+4*count+16*count;
        for (int i=0;i<count;i++) {
            int off=4+4*count+16*i;
            put32(4+4*i,off);
            memset(archive+off,i+1,16);
        }
        int files[]={0,39,count-2,count-1};
        for (int j=0;j<4;j++) {
            expected_offset=4+4*count+16*files[j];
            int before=decoded;
            CHECK(HuDataReadNumHeapShortForce(0x00f30000|files[j],0,HEAP_MODEL)==&token);
            CHECK(decoded==before+1 && bad_read==0);
        }
        CHECK(HuDataReadNumHeapShortForce(0x00f30000|count,0,HEAP_MODEL)==NULL);
    }
    CHECK(closed==10);
    puts("short archive reader big-endian count and offsets: PASS");
    return 0;
}
