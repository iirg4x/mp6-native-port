#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mp6_enhancements.h"
#include "mp6_heap_scale.h"
#include "mp6_minigame.h"

int mbev_MgCall(void);
int mbMgRouletteNumGet(int type);
int mbMgCallSingleOnCheck(unsigned short overlay);
void mbev_MgCallSingle(int player);
void mbev_MgCallSingleKoopa(int player, int koopa);
void mbev_MgCallKettou(void);
void mbev_MgCallDonkey(void);
void mbev_MgCallKoopa(void);
static int skipped;
void mp6_event_post(const char *key, long value, const char *text)
{
    assert(strcmp(key, "minigame.skipped") == 0);
    assert(value == 0 && text && *text);
    ++skipped;
}

int main(void)
{
    Mp6EnhValues settings;
    mp6_enh_defaults(&settings);
    assert(settings.ambientOcclusion == 0 && mp6_enh_ambient_occlusion() == 0);
    settings.ambientOcclusion = 2;
    mp6_enh_set_values(&settings);
    assert(mp6_enh_ambient_occlusion() == 2);
    settings.ambientOcclusion = 0;
    settings.heapScale = 1;
    mp6_enh_set_values(&settings);
    assert(!mp6_heap_scale_restart_pending());
    settings.heapScale = 4;
    mp6_enh_set_values(&settings);
    assert(!mp6_heap_scale_restart_pending()); /* prelaunch must not latch */
    assert(mp6_heap_scale_active() == 4);
    settings.heapScale = 1;
    mp6_enh_set_values(&settings);
    assert(mp6_heap_scale_restart_pending()); /* existing pointers cannot move */
    assert(mp6_heap_scale_active() == 4);
    settings.heapScale = 4;
    mp6_enh_set_values(&settings);
    assert(!mp6_heap_scale_restart_pending());

    /* TRUE is the board's continue-in-place path; FALSE sleeps indefinitely. */
    assert(!mp6_minigame_take_skipped());
    assert(mbev_MgCall() == 1);
    assert(mp6_minigame_take_skipped());
    assert(!mp6_minigame_take_skipped());
    assert(mbev_MgCall() == 1);
    for (int type = -1; type <= 9; ++type) assert(mbMgRouletteNumGet(type) == 0);
    assert(!mbMgCallSingleOnCheck(0));
    assert(!mbMgCallSingleOnCheck(0xffff));
    mbev_MgCallSingle(0);
    mbev_MgCallSingleKoopa(0, 1);
    mbev_MgCallKettou();
    mbev_MgCallDonkey();
    mbev_MgCallKoopa();
    assert(skipped == 7);
    puts("runtime settings and minigame continuation: PASS");
    return 0;
}
