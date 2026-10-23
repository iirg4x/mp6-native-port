#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "mp6_mgmtournament_native.h"
#include "mp6_window.h"
#include "game/mgdata.h"
#include "game/memory.h"
#include "game/data.h"
#include "game/sprite.h"
#include "game/thpmain.h"
#include "game/frand.h"

HUWIN winData[HUWIN_MAX];
MGDATA MgDataTbl[102];
ANIMDATA **lbl_1_bss_8C;
s16 lbl_1_bss_88;
s16 lbl_1_bss_0;
u8 lbl_1_bss_2[128];
ANIMDATA *lbl_1_bss_98;
ANIMDATA *lbl_1_bss_84;

static ANIMDATA animations[254];
static ANIMDATA originalStill;
static struct {
    uintptr_t before;
    ANIMDATA *entries[254];
    uintptr_t after;
} allocation;
static int allocations, reads, kills, frees, closes, randomCalls, failAllocation;

void *HuMemDirectMallocNum(HEAPID heap, s32 size, u32 number)
{
    assert(heap == HEAP_HEAP && number == HU_MEMNUM_OVL);
    assert(size > 0 && size <= (s32)sizeof(allocation.entries));
    allocations++;
    allocation.before = allocation.after = UINT64_C(0x123456789abcdef0);
    return failAllocation ? NULL : allocation.entries;
}
void HuMemDirectFree(void *pointer)
{
    assert(pointer == allocation.entries);
    frees++;
}
void *HuDataReadNumHeapShortForce(u32 data, s32 number, HEAPID heap)
{
    (void)data;
    assert(number == HU_MEMNUM_OVL && heap == HEAP_MODEL);
    assert(reads < 254);
    return &animations[reads++];
}
ANIMDATA *HuSprAnimRead(void *data) { return data; }
void HuSprAnimKill(ANIMDATA *animation)
{
    assert(animation >= animations && animation < animations + 254);
    kills++;
}
void HuTHPClose(void) { closes++; }
s32 frandmod(s32 limit) { assert(limit == 2); randomCalls++; return 1; }

/* TOURNAMENT_NATIVE_SUBJECT */

int main(void)
{
    u8 order[128] = {0};
    char text[] = "Hi";
    HUWIN snapshot[HUWIN_MAX];

    /* A borrowed host pointer must survive without the original u32 encoding. */
    assert((uintptr_t)text > UINT32_MAX);
    mp6_window_insert_text(3, text, 15);
    assert(winData[3].messDataInsert[15] == text);
    memcpy(snapshot, winData, sizeof(snapshot));
    mp6_window_insert_text(-1, text, 0);
    mp6_window_insert_text(HUWIN_MAX, text, 0);
    mp6_window_insert_text(3, text, -1);
    mp6_window_insert_text(3, text, HUWIN_INSERTMES_MAX);
    assert(memcmp(snapshot, winData, sizeof(snapshot)) == 0);
    fn_1_199C(3, 15);
    assert(winData[3].messDataInsert[15] != text);
    assert(winData[3].messDataInsert[15][0] == 0);

    /* An empty cycle allocates nothing and leaves the still untouched. */
    lbl_1_bss_98 = &originalStill;
    fn_1_2A4(0, order);
    fn_1_43C();
    assert(allocations == 0 && randomCalls == 0 && closes == 0);
    assert(lbl_1_bss_0 == 0 && lbl_1_bss_2[0] == 255);
    assert(lbl_1_bss_98 == &originalStill);
    fn_1_2A4(-1, order);
    fn_1_2A4(5, NULL);
    assert(allocations == 0);

    /* Capacity reserves one terminator byte, not half of the order array. */
    fn_1_2A4(128, order);
    assert(lbl_1_bss_0 == 127 && lbl_1_bss_2[127] == 255);
    assert(reads == 254 && allocations == 1);
    assert(allocation.before == UINT64_C(0x123456789abcdef0));
    assert(allocation.after == UINT64_C(0x123456789abcdef0));
    for (int i = 0; i < 127; ++i) {
        fn_1_43C();
        assert(lbl_1_bss_98 == &animations[2 * i + 1]);
    }
    assert(lbl_1_bss_88 == 0 && randomCalls == 127);
    fn_1_4F8();
    assert(kills == 254 && frees == 1);
    assert(lbl_1_bss_98 == &originalStill && lbl_1_bss_8C == NULL);
    fn_1_4F8();
    assert(kills == 254 && frees == 1);

    failAllocation = 1;
    fn_1_2A4(5, order);
    fn_1_43C();
    assert(lbl_1_bss_0 == 0 && lbl_1_bss_8C == NULL);
    assert(reads == 254 && lbl_1_bss_98 == &originalStill);
    return 0;
}
