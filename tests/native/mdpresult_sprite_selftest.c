#include <stdint.h>
#include <stdio.h>
#include <assert.h>

typedef int16_t s16;
typedef int32_t s32;
typedef s16 HUSPR_GROUPID;

#define HUSPR_ATTR_DISPOFF 4

typedef struct {
    HUSPR_GROUPID group;
} MDRESULT_GROUP_WORK;

typedef struct {
    s16 values[4];
} MDRESULT_BSS_1278_WORK;

static MDRESULT_GROUP_WORK lbl_1_bss_714 = { 23 };
static MDRESULT_BSS_1278_WORK lbl_1_bss_1278;

static s16 attrGroup;
static s32 attrMask;
static s16 bankGroup[2];
static s16 bankMember[2];
static s16 bankValue[2];
static s16 bankCount;

void fn_1_20188(HUSPR_GROUPID groupId, s32 attr)
{
    attrGroup = groupId;
    attrMask = attr;
}

void HuSprBankSet(HUSPR_GROUPID groupId, s16 memberNo, s16 bank)
{
    assert(bankCount < 2);
    bankGroup[bankCount] = groupId;
    bankMember[bankCount] = memberNo;
    bankValue[bankCount] = bank;
    bankCount++;
}

/* MDPRESULT_SPRITE_SUBJECT */

static int run_case(s16 board, s16 turns, s16 expectedOtherBank)
{
    lbl_1_bss_1278.values[0] = board;
    lbl_1_bss_1278.values[1] = turns;
    bankCount = 0;
    fn_1_17CF4();
    assert(attrGroup == lbl_1_bss_714.group);
    assert(attrMask == HUSPR_ATTR_DISPOFF);
    assert(bankCount == 2);
    assert(bankGroup[0] == lbl_1_bss_714.group);
    assert(bankMember[0] == 1 && bankValue[0] == board);
    assert(bankMember[1] == 2 && bankValue[1] == expectedOtherBank);
    assert(bankValue[1] >= 0);
    return 0;
}

int main(void)
{
    for (int board=0; board<6; ++board)
        for (int turns=10; turns<=50; turns+=5)
            assert(run_case(board,turns,(turns-10)/5)==0);
    puts("mdpresult sprite banks: every legal board/turn pair PASS");
    return 0;
}
