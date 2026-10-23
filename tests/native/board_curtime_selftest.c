#include <assert.h>
#include <string.h>

#include "game/gamework.h"

_Static_assert(sizeof(GW_SYSTEM) == 0x2C0, "fixture must use the real GW_SYSTEM layout");

GW_SYSTEM GwSystem;
s16 lbl_1_bss_5C4;
s16 lbl_1_bss_18;
void *lbl_1_bss_8;

static int light_red;
static int light_green;
static int light_blue;
static int light_model;
static int light_slot;
static int light_calls;
static int current_space_attr;
static int day_event_calls;
static int night_event_calls;
static int alternate_event_calls;
static int other_event_calls;

void Hu3DBGColorSet(u8 red, u8 green, u8 blue)
{
    light_red = red;
    light_green = green;
    light_blue = blue;
    ++light_calls;
}

s16 mbObjModelIDGet(s16 object)
{
    assert(object == lbl_1_bss_5C4 || object == lbl_1_bss_18);
    return object + 100;
}

void Hu3DModelLightInfoSet(s16 model, s16 slot)
{
    light_model = model;
    light_slot = slot;
    ++light_calls;
}

s32 mbMasuMAttrGet(s16 spaceId)
{
    assert(spaceId == 42 || spaceId == 7);
    return current_space_attr;
}

void fn_1_5C64(int playerNo, s16 spaceId)
{
    (void)playerNo;
    (void)spaceId;
    ++other_event_calls;
}

void fn_1_709C(int playerNo, s16 spaceId)
{
    (void)playerNo;
    (void)spaceId;
    ++day_event_calls;
}

void fn_1_AC10(int playerNo, s16 spaceId)
{
    (void)playerNo;
    (void)spaceId;
    ++night_event_calls;
}

void fn_1_E4A0(int playerNo, s16 spaceId)
{
    (void)playerNo;
    (void)spaceId;
    ++other_event_calls;
}

void fn_1_12D20(int playerNo, s16 spaceId)
{
    (void)playerNo;
    (void)spaceId;
    ++night_event_calls;
}

void fn_1_13C3C(int playerNo, s16 spaceId)
{
    (void)playerNo;
    (void)spaceId;
    ++day_event_calls;
}

void fn_1_160E4(int playerNo, s16 spaceId)
{
    (void)playerNo;
    (void)spaceId;
    ++alternate_event_calls;
}

/* BOARD_CURTIME_NATIVE_SUBJECT */

static void set_time_bits(int curTime, int nextTime)
{
    memset(&GwSystem, 0, sizeof(GwSystem));
    GwSystem.curTime = curTime;
    GwSystem.nextTime = nextTime;
}

static void reset_counters(void)
{
    light_red = -1;
    light_green = -1;
    light_blue = -1;
    light_model = -1;
    light_slot = -1;
    light_calls = 0;
    day_event_calls = 0;
    night_event_calls = 0;
    alternate_event_calls = 0;
    other_event_calls = 0;
}

static void test_w03_lighting_uses_cur_time(void)
{
    for (int curTime = 0; curTime <= 1; ++curTime) {
        reset_counters();
        set_time_bits(curTime, 1 - curTime);
        lbl_1_bss_5C4 = 12;

        fn_1_1BB8();

        assert(light_calls == 2);
        assert(light_model == 112 && light_slot == 1);
        if (curTime == 0) {
            assert(light_red == 255 && light_green == 255 && light_blue == 255);
        } else {
            assert(light_red == 0 && light_green == 0 && light_blue == 0);
        }
    }
}

static void test_w04_dispatch_uses_cur_time(void)
{
    for (int curTime = 0; curTime <= 1; ++curTime) {
        for (int nextTime = 0; nextTime <= 1; ++nextTime) {
            reset_counters();
            set_time_bits(curTime, nextTime);
            current_space_attr = 8;

            assert(fn_1_D3C(2, 42) == 0);
            assert(other_event_calls == 0);
            if (curTime == 0) {
                assert(night_event_calls == 1 && day_event_calls == 0);
            } else {
                assert(day_event_calls == 1 && night_event_calls == 0);
            }
        }
    }
}

static void test_w05_lighting_and_dispatch_use_cur_time(void)
{
    for (int curTime = 0; curTime <= 1; ++curTime) {
        for (int nextTime = 0; nextTime <= 1; ++nextTime) {
            reset_counters();
            set_time_bits(curTime, nextTime);
            lbl_1_bss_18 = 23;
            fn_1_2010();
            assert(light_calls == 2);
            assert(light_model == 123 && light_slot == 1);
            if (curTime == 0) {
                assert(light_red == 127 && light_green == 127 && light_blue == 192);
            } else {
                assert(light_red == 64 && light_green == 64 && light_blue == 127);
            }

            reset_counters();
            set_time_bits(curTime, nextTime);
            current_space_attr = 8;
            assert(fn_1_20F8(3, 7) == 1);
            assert(other_event_calls == 0 && alternate_event_calls == 0);
            if (curTime == 0) {
                assert(night_event_calls == 1 && day_event_calls == 0);
            } else {
                assert(day_event_calls == 1 && night_event_calls == 0);
            }
        }
    }
}

int main(void)
{
    test_w03_lighting_uses_cur_time();
    test_w04_dispatch_uses_cur_time();
    test_w05_lighting_and_dispatch_use_cur_time();
    return 0;
}
