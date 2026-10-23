/* Unlock All mod contract (include/mp6_unlock_all.h), against the patched
 * game/gamework.c and the real save marshal (src/os/save_endian.c):
 *   - off: every query answers from GwCommon;
 *   - on: unlock queries answer owned, bank flag 60 (an option) stays real;
 *   - the Star Bank's exit write-back cannot leak into the card image, while
 *     the option flag stays settable;
 *   - off again: the save's real state is back.
 * Built and run by tests/test_unlock_all.py. */
#include <stdio.h>
#include <string.h>

#include "dolphin/types.h"
#include "game/gamework.h"
#include "mp6_save_endian.h"
#include "mp6_unlock_all.h"

static int fails;
#define CHK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

char *mp6_test_getenv(const char *name)
{
    (void)name;
    return NULL;
}

/* gamework.c's only links outside this test. */
void _ClearFlag(u32 flag) { (void)flag; }
void _SetFlag(u32 flag) { (void)flag; }
BOOL _CheckFlag(u32 flag) { (void)flag; return FALSE; }
void _InitFlag(void) {}
int omPadErrChk(int padNo) { (void)padNo; return 0; }
u32 SIProbe(s32 chan) { (void)chan; return 0; }

static u8 image_a[sizeof(GW_COMMON) + 64];
static u8 image_b[sizeof(GW_COMMON) + 64];

/* What mdbank.c's exit (fn_1_83CC) does with every item it showed as owned. */
static void bank_exit_writeback(void)
{
    int flag;
    for (flag = 0; flag <= 63; flag++) {
        if (GWBankFlagGet((GW_BANK_FLAG)flag)) GWBankFlagSet((GW_BANK_FLAG)flag);
    }
    if (GWBankFlagGet((GW_BANK_FLAG)6)) GWMgUnlockSet(677);
}

int main(void)
{
    size_t i, diffs;

    /* A real save's state: one earned minigame, Toadette bought, nothing else. */
    memset(&GwCommon, 0, sizeof(GwCommon));
    memcpy(GwCommon.magic, "SAVE", 4);
    GwCommon.mgUnlock[2] = 1u << (665 - GW_MGNO_BASE - 64);
    GWBankFlagSet((GW_BANK_FLAG)2);
    GwCommon.bankStar = 123;
    mp6_save_common_to_be(image_a, &GwCommon);

    CHK(!mp6_unlock_all_enabled(), "mod must default off");
    CHK(GWMgUnlockGet(665) && !GWMgUnlockGet(601), "off: minigame queries are real");
    CHK(GWBankFlagGet((GW_BANK_FLAG)2) && !GWBankFlagGet((GW_BANK_FLAG)3), "off: bank queries are real");
    CHK(!GWSinglePrizeSaveFlagGet((GW_SINGLE_PRIZE_FLAG)1), "off: prize queries are real");

    mp6_unlock_all_set_enabled(1);
    CHK(mp6_unlock_all_enabled(), "published setting must enable the mod");
    for (i = 601; i < 601 + 96; i++) CHK(GWMgUnlockGet((int)i), "on: minigame %d unlocked", (int)i);
    for (i = 0; i <= 63; i++) {
        if (i == MP6_UNLOCK_ALL_BANK_OPTION_FLAG) continue;
        CHK(GWBankFlagGet((GW_BANK_FLAG)i), "on: bank flag %d owned", (int)i);
    }
    CHK(!GWBankFlagGet((GW_BANK_FLAG)MP6_UNLOCK_ALL_BANK_OPTION_FLAG), "on: option flag 60 stays real");
    CHK(GWSinglePrizeSaveFlagGet((GW_SINGLE_PRIZE_FLAG)1), "on: prize records shown");
    CHK(!GWBankFlagGet((GW_BANK_FLAG)64) && !GWSinglePrizeSaveFlagGet((GW_SINGLE_PRIZE_FLAG)64),
        "on: out-of-range flags stay FALSE");

    bank_exit_writeback();
    mp6_save_common_to_be(image_b, &GwCommon);
    CHK(memcmp(image_a, image_b, sizeof(GW_COMMON)) == 0, "bank write-back leaked into the save image");

    /* The bank's option toggle still works and is the only save change. */
    GWBankFlagSet((GW_BANK_FLAG)MP6_UNLOCK_ALL_BANK_OPTION_FLAG);
    CHK(GWBankFlagGet((GW_BANK_FLAG)MP6_UNLOCK_ALL_BANK_OPTION_FLAG), "on: option flag 60 settable");
    mp6_save_common_to_be(image_b, &GwCommon);
    for (i = 0, diffs = 0; i < sizeof(GW_COMMON); i++) diffs += image_a[i] != image_b[i];
    CHK(diffs == 1, "option toggle must change exactly one save byte (got %u)", (unsigned)diffs);
    GWBankFlagReset((GW_BANK_FLAG)MP6_UNLOCK_ALL_BANK_OPTION_FLAG);

    mp6_unlock_all_set_enabled(0);
    CHK(GWMgUnlockGet(665) && !GWMgUnlockGet(601) && !GWMgUnlockGet(677), "off again: real minigames");
    CHK(GWBankFlagGet((GW_BANK_FLAG)2) && !GWBankFlagGet((GW_BANK_FLAG)6), "off again: real bank flags");
    mp6_save_common_to_be(image_b, &GwCommon);
    CHK(memcmp(image_a, image_b, sizeof(GW_COMMON)) == 0, "off again: save image is the original");
    GWMgUnlockSet(601);
    CHK(GWMgUnlockGet(601), "off: earned unlocks are recorded again");

    printf("%s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
