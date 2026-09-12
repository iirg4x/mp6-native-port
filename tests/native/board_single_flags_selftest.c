#include <stdio.h>
#include <string.h>
#include "game/gamework.h"
#include "game/mgdata.h"

u32 GwSingleMgFlag[3];
#include "board_subject.inc"
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    for (int bit = 0; bit < 96; ++bit) {
        memset(GwSingleMgFlag, 0, sizeof(GwSingleMgFlag));
        CHECK(!GWSingleMgFlagGet(GW_MGNO_BASE + bit));
        CHECK(GWSingleMgFlagSet(GW_MGNO_BASE + bit) == bit);
        CHECK(GWSingleMgFlagGet(GW_MGNO_BASE + bit));
        for (int word = 0; word < 3; ++word)
            CHECK(GwSingleMgFlag[word] == (word == bit / 32 ? 1u << (bit % 32) : 0));
    }
    u32 before[3];
    memcpy(before, GwSingleMgFlag, sizeof(before));
    CHECK(GWSingleMgFlagSet(GW_MGNO_BASE - 1) == -1);
    CHECK(GWSingleMgFlagSet(GW_MGNO_BASE + 96) == 96);
    CHECK(!GWSingleMgFlagGet(GW_MGNO_BASE - 1));
    CHECK(!GWSingleMgFlagGet(GW_MGNO_BASE + 96));
    CHECK(!memcmp(before, GwSingleMgFlag, sizeof(before)));
    CHECK(!GWMgCustomGet(GW_MGNO_BASE));
    puts("Single recovered flag API and custom-list stub: PASS");
    return 0;
}
