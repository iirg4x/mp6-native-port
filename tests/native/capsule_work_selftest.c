#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "game/object.h"
#include "game/gamework.h"
#include "game/memory.h"
#define CAP_WORK_MAX 64
static size_t allocated;
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 number)
{
    (void)heap; (void)number;
    allocated=bytes;
    return malloc(bytes);
}
#include "board_subject.inc"
_Static_assert(sizeof(CAPWORK)==sizeof(OWNER_CAPWORK), "event owner layout differs");
_Static_assert(offsetof(CAPWORK,processNo)==offsetof(OWNER_CAPWORK,processNo), "event tail moved");
_Static_assert(offsetof(CAPWORK,capLoseObj)==offsetof(OWNER_CAPWORK,capLoseObj), "last pointer moved");
_Static_assert(offsetof(CAPWORK,eventData)==offsetof(OWNER_CAPWORK,_unkB6C), "payload moved");
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
    CAPWORK work, expected;
    OMOBJ obj={0};
    memset(&work,0x5a,sizeof(work));
    expected=work;
    for (int i=0;i<4;i++) work.eventData[i+2]=i+123;
    memcpy((unsigned char *)&expected+offsetof(CAPWORK,eventData)+2*sizeof(int),
           &work.eventData[2],4*sizeof(int));
    CHECK(memcmp(&expected,&work,sizeof(work))==0);
    CHECK(work.objWork.bgId==expected.objWork.bgId);
    subject_copy(&obj,&work);
    CHECK(allocated==sizeof(CAPWORK));
    CHECK(memcmp(obj.data,&work,sizeof(work))==0);
    CHECK(((CAPWORK *)obj.data)->capLoseObj==work.capLoseObj);
    free(obj.data);
    puts("capsule native layout and full event copy: PASS");
    return 0;
}
