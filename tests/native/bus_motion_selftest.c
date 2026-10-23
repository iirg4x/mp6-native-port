#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef int16_t s16;
typedef uint8_t u8;
typedef float f32;

typedef struct {
    f32 x;
    f32 y;
    f32 z;
} Vec;

typedef struct OMOBJ {
    s16 mdlId[15];
    s16 mtnId[15];
} OMOBJ;

/* Filled from the recovered source by test_native_bus_motion.py.  Keeping
 * this exact production typedef in the fixture catches accidental ABI drift
 * in the state record used by fn_1_1C74. */
/* MGMFREE_MODEL_STATE_TYPE */

enum {
    HU3D_ATTR_DISPOFF = 0x20,
    MGMFREE_SE_RANDOM_MODEL_DISPLAY_START = 1220,
};

static f32 g_scale = 1.0f;
static f32 g_motion_speed[16];
static f32 g_motion_time[16];
static f32 g_model_x[16];
static unsigned g_motion_resets[16];
static unsigned g_random_value;
static unsigned g_random_calls;
static unsigned g_tile_calls;
static unsigned g_sound_calls;

static OMOBJ g_marker;
static OMOBJ g_object;
static MGMFREE_MODEL_STATE lbl_1_bss_A8[16];
static s16 lbl_1_bss_9D0[4];
static OMOBJ *lbl_1_bss_30 = &g_marker;

static f32
linear(f32 start, f32 end, f32 time, f32 duration)
{
    if (duration <= 0.0f) return start;
    return start + (end - start) * (time / duration);
}

f32 fn_1_E35C(f32 start, f32 end, f32 time, f32 duration)
{
    return linear(start, end, time, duration);
}

f32 fn_1_E3E8(f32 start, f32 end, f32 time, f32 duration)
{
    return linear(start, end, time, duration);
}

f32 mp6_widescreen_scale_factor(void)
{
    return g_scale;
}

u8 rand8(void)
{
    ++g_random_calls;
    return (u8)g_random_value;
}

f32 frandmod(s16 range)
{
    (void)range;
    return 0.0f;
}

void Hu3DMotionSpeedSet(s16 model, f32 speed)
{
    assert(model >= 0 && model < 16);
    g_motion_speed[model] = speed;
}

void Hu3DModelPosSet(s16 model, f32 x, f32 y, f32 z)
{
    (void)y;
    (void)z;
    assert(model >= 0 && model < 16);
    g_model_x[model] = x;
}

void Hu3DModelPosGet(s16 model, Vec *position)
{
    assert(model >= 0 && model < 16);
    position->x = g_model_x[model];
    position->y = 0.0f;
    position->z = 0.0f;
}

void Hu3DModelPosSetV(s16 model, const Vec *position)
{
    Hu3DModelPosSet(model, position->x, position->y, position->z);
}

void Hu3DMotionSet(s16 model, s16 motion)
{
    assert(model >= 0 && model < 16);
    (void)motion;
    ++g_motion_resets[model];
    /* Hu3DMotionSet starts the selected motion at frame zero. */
    g_motion_time[model] = 0.0f;
}

f32 Hu3DMotionTimeGet(s16 model)
{
    assert(model >= 0 && model < 16);
    return g_motion_time[model];
}

void Hu3DMotionTimeSet(s16 model, f32 time)
{
    assert(model >= 0 && model < 16);
    g_motion_time[model] = time;
}

void Hu3DModelAttrSet(s16 model, int attr)
{
    (void)model;
    (void)attr;
}

void Hu3DModelAttrReset(s16 model, int attr)
{
    (void)model;
    (void)attr;
}

void HuAudFXPlay(s16 sound)
{
    (void)sound;
    ++g_sound_calls;
}

void fn_1_6848(s16 mode)
{
    (void)mode;
}

void mp6_widescreen_tile_x(s16 model, f32 period, f32 anchor)
{
    (void)model;
    (void)period;
    (void)anchor;
    ++g_tile_calls;
}

/* BUS_MOTION_SUBJECT */

static void
reset_fixture(void)
{
    memset(&g_object, 0, sizeof(g_object));
    memset(&g_marker, 0, sizeof(g_marker));
    memset(lbl_1_bss_A8, 0, sizeof(lbl_1_bss_A8));
    memset(lbl_1_bss_9D0, 0, sizeof(lbl_1_bss_9D0));
    memset(g_motion_speed, 0, sizeof(g_motion_speed));
    memset(g_motion_time, 0, sizeof(g_motion_time));
    memset(g_model_x, 0, sizeof(g_model_x));
    memset(g_motion_resets, 0, sizeof(g_motion_resets));
    g_scale = 1.0f;
    g_random_value = 0;
    g_random_calls = 0;
    g_tile_calls = 0;
    g_sound_calls = 0;
    lbl_1_bss_30 = &g_marker;
    for (int i = 0; i < 15; ++i) {
        g_object.mdlId[i] = (s16)i;
        g_object.mtnId[i] = (s16)(100 + i);
    }
    /* Keep unrelated random scenery dormant so only layer 6's skip branch is
     * under test. */
    for (int i = 0; i < 4; ++i) {
        static const int index[] = {4, 7, 8, 9};
        lbl_1_bss_A8[index[i]].value.countdown = 100000.0f;
    }
    lbl_1_bss_A8[2].interpolationDuration = 10800.0f;
    lbl_1_bss_A8[3].interpolationDuration = 5400.0f;
    lbl_1_bss_A8[12].interpolationDuration = 600.0f;
}

static void
advance_motion_one_tick(void)
{
    g_motion_time[5] += g_motion_speed[5];
    g_motion_time[6] += g_motion_speed[6];
}

static int
close_to(f32 actual, f32 expected)
{
    return fabsf(actual - expected) < 0.0005f;
}

static void
check_scale(f32 scale, int motion_complete_tick, int timer_tick)
{
    reset_fixture();
    g_scale = scale;
    fn_1_1C74(&g_object);
    assert(g_motion_resets[5] == 1);
    assert(g_motion_resets[6] == 1);
    assert(close_to(g_motion_speed[0], 0.5f));
    assert(close_to(g_motion_speed[5], 1.0f / scale));
    assert(close_to(g_motion_speed[6], 1.0f / scale));
    assert(close_to(lbl_1_bss_A8[5].value.countdown, 1080.0f));
    assert(close_to(lbl_1_bss_A8[6].value.countdown, 1080.0f));
    assert(g_tile_calls == 3);

    /* The lamp's authored motion has 1079 frames.  It must reach that final
     * frame before the timer can request a new motion, at every aspect. */
    for (int tick = 1; tick <= motion_complete_tick; ++tick) {
        advance_motion_one_tick();
        fn_1_1C74(&g_object);
        assert(g_motion_resets[5] == 1);
        assert(g_motion_resets[6] == 1);
    }
    assert(g_motion_time[5] >= 1079.0f);

    for (int tick = motion_complete_tick + 1; tick < timer_tick; ++tick) {
        advance_motion_one_tick();
        fn_1_1C74(&g_object);
        assert(g_motion_resets[5] == 1);
        assert(g_motion_resets[6] == 1);
    }
    advance_motion_one_tick();
    fn_1_1C74(&g_object);
    assert(g_motion_resets[5] == 2);
    assert(g_motion_resets[6] == 2);
    assert(close_to(lbl_1_bss_A8[5].value.countdown, 1080.0f));
    assert(close_to(lbl_1_bss_A8[6].value.countdown, 1080.0f));
}

static void
check_live_aspect_toggle(void)
{
    reset_fixture();
    g_scale = 1.0f;
    fn_1_1C74(&g_object);
    advance_motion_one_tick();
    g_scale = 4.0f / 3.0f;
    fn_1_1C74(&g_object);
    assert(close_to(g_motion_speed[5], 0.75f));
    assert(close_to(g_motion_speed[6], 0.75f));
    assert(close_to(lbl_1_bss_A8[5].value.countdown, 1079.25f));
    advance_motion_one_tick();
    g_scale = 1.75f;
    fn_1_1C74(&g_object);
    assert(close_to(g_motion_speed[5], 1.0f / 1.75f));
    assert(close_to(g_motion_speed[6], 1.0f / 1.75f));
    assert(g_motion_resets[5] == 1 && g_motion_resets[6] == 1);
}

static void
check_layer6_random_skip(void)
{
    reset_fixture();
    g_random_value = 1; /* rand8()%2 == 1: keep the layer 6 motion parked. */
    fn_1_1C74(&g_object);
    assert(g_motion_resets[5] == 1);
    assert(g_motion_resets[6] == 0);
    assert(close_to(lbl_1_bss_A8[6].value.countdown, 1080.0f));

    lbl_1_bss_A8[6].value.countdown = -1.0f;
    fn_1_1C74(&g_object);
    assert(g_motion_resets[6] == 0);
    assert(close_to(lbl_1_bss_A8[6].value.countdown, 1080.0f));

    g_random_value = 0;
    lbl_1_bss_A8[6].value.countdown = -1.0f;
    fn_1_1C74(&g_object);
    assert(g_motion_resets[6] == 1);
}

int
main(void)
{
    /* timer_tick is the first update after the 1080-frame countdown reaches
     * zero; motion_complete_tick is the first update at/after authored frame
     * 1079. */
    check_scale(1.0f, 1079, 1081);
    check_scale(4.0f / 3.0f, 1439, 1441);
    check_scale(1.75f, 1889, 1891);
    check_live_aspect_toggle();
    check_layer6_random_skip();
    puts("MGMfree bus motion timing: PASS");
    return 0;
}
