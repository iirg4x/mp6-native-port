#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/object.h"
#include "mp6_minigame.h"

s32 omovlhisidx;
static OMOVLHIS history[16];
static int returned, destination, changed, events;
OMOVLHIS *omOvlHisGet(s32 offset)
{
    assert(offset >= 0 && offset <= omovlhisidx && offset < 16);
    return &history[omovlhisidx - offset];
}
void omOvlReturnEx(s16 offset, s16 unlink)
{
    assert(unlink == TRUE);
    returned = offset;
    destination = history[omovlhisidx - offset].ovl;
}
void omOvlHisChg(s32 offset, OMOVL overlay, s32 event, s32 status)
{
    assert(omovlhisidx == 0 && offset == 0 && event == 0 && status == 0);
    history[0].ovl = overlay;
    changed++;
}
void omOvlGotoEx(OMOVL overlay, s16 unlink, s32 event, s32 status)
{
    assert(unlink && event == 0 && status == 0);
    destination = overlay;
}
void mp6_event_post(const char *key, long number, const char *value)
{
    assert(strcmp(key, "overlay.skipped") == 0 && value);
    assert(number >= 0);
    events++;
}
static void reset(int depth)
{
    omovlhisidx = depth;
    returned = changed = events = 0;
    destination = DLL_NONE;
    for (int i = 0; i < 16; ++i) history[i].ovl = DLL_m601dll;
}
int main(void)
{
    assert(!mp6_unavailable_overlay_take_returned());
    reset(2);
    history[0].ovl = DLL_mdseldll;
    history[1].ovl = DLL_w01dll;
    history[1].evtno = 9; history[1].stat = 42;
    mp6_unavailable_overlay_return();
    assert(mp6_unavailable_overlay_take_returned());
    assert(!mp6_unavailable_overlay_take_returned());
    assert(returned == 1 && destination == DLL_w01dll && events == 1);
    assert(history[1].evtno == 9 && history[1].stat == 42);
    reset(3); history[1].ovl = DLL_mdpartydll;
    mp6_unavailable_overlay_return();
    assert(returned == 2 && destination == DLL_mdpartydll);
    for (int depth = -1; depth < 16; ++depth) {
        reset(depth);
        mp6_unavailable_overlay_return();
        assert(!returned && changed == 1 && events == 1);
        assert(destination == DLL_mdseldll && history[0].ovl == DLL_mdseldll);
    }
    puts("missing overlay return: PASS");
}
