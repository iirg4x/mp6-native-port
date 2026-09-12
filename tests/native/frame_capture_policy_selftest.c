#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mp6_parse.h"
#include "selftest_assert.h"

static const char *tick_env;
static char *test_getenv(const char *key) {
    return strcmp(key, "MP6_TICK_HZ") == 0 ? (char *)tick_env : NULL;
}
#define getenv test_getenv
static double configured_hz;
static int unlocked = 1;
double mp6_launcher_cfg_tick_hz(void) { return configured_hz; }
int mp6_host_init(void) { return 1; }
#include "scheduler-under-test.inc"
#include "replay-under-test.inc"

long mp6_tick_count;
static int arms, disarms, ao_frames, layouts, model_resets, snapshots, animlogs;
static void (*capture)(const void *, uint32_t, void *);
int mp6_enh_unlocked_fps(void) { return unlocked; }
int mp6_console_cvar_get(int id, int value) { (void)id; return value; }
int mp6_fi_model_camera_stable(int camera) { (void)camera; return 1; }
int mp6_fi_capture_camera_id(void) { return -1; }
int mp6_fi_capture_context_next(int *c, int *m, uint32_t *g, uint16_t *s, uint16_t *o) {
    (void)c; (void)m; (void)g; (void)s; (void)o; return 0;
}
void mp6_ao_begin_frame(void) { ++ao_frames; }
void aurora_gx_set_drain_capture(void (*fn)(const void *, uint32_t, void *), void *user) {
    assert(user == NULL);
    if (fn) { assert(!capture); ++arms; }
    else { assert(capture); ++disarms; }
    capture = fn;
}
void aurora_gx_export_vtx_layout(uint8_t *desc, uint8_t *cnt, uint8_t *type) {
    memset(desc, 0, 21); memset(cnt, 0, 8*21); memset(type, 0, 8*21); ++layouts;
}
void mp6_fi_model_reset(void) { ++model_resets; }
void mp6_fi_model_snapshot(void) { ++snapshots; }
void mp6_fi_model_animlog(long tick) { assert(tick == ++animlogs); }
uint64_t mp6_host_monotonic_ns(void) { static uint64_t clock; return ++clock; }

static void frame(void) {
    static const uint8_t command[] = {GX_NOP, GX_NOP};
    mp6_fi_note_frame_begin();
    if (capture) capture(command, sizeof(command), NULL);
    mp6_fi_note_frame_end();
}
static void rate(double hz, const char *env) {
    tick_env = NULL;
    configured_hz = hz;
    mp6_tick_rate_refresh();
    tick_env = env;
}

int main(void) {
    rate(0, NULL);
    for (int i=0; i<100; ++i) frame();
    assert(unlocked && !s_active && !capture);
    assert(!arms && !layouts && !test_walk_calls && !snapshots);
    assert(!s_streams[0].data && !s_streams[1].data);
    assert(ao_frames == 100 && animlogs == 100);

    rate(60, NULL);
    frame();
    assert(s_active && capture && arms == 1 && layouts == 1);
    assert(s_latest >= 0 && s_prev == -1 && s_streams[s_latest].walkOk);
    frame();
    assert(s_prev >= 0 && snapshots == 2 && test_walk_calls > 0);
    unsigned walks = test_walk_calls;
    rate(0, NULL);
    for (int i=0; i<100; ++i) frame();
    assert(!s_active && !capture && disarms == 1 && model_resets == 1);
    assert(s_cur == -1 && s_latest == -1 && s_prev == -1);
    assert(test_walk_calls == walks && snapshots == 2 && layouts == 2);

    rate(60, NULL); frame();
    assert(s_active && arms == 2 && s_prev == -1); // no pre-Fast-Forward pairing
    unlocked = 0; frame();
    assert(!s_active && disarms == 2);
    unlocked = 1; frame();
    assert(s_active && arms == 3 && s_prev == -1);

    rate(60, "0"); frame();
    assert(!s_active && !mp6_tick_interpolation_possible());
    configured_hz = 120; mp6_tick_rate_refresh(); frame();
    assert(!s_active); // explicit environment override still wins
    rate(0, "60"); frame(); assert(s_active);
    rate(0, "invalid"); frame(); assert(s_active && g_tickHz == 60);
    rate(0, "0.001"); frame(); assert(s_active && g_tickHz == 60);
    rate(0, "1000"); frame(); assert(s_active && g_tickHz == 1000);
    g_tickHz = 0; frame(); assert(!s_active); // scheduler fail-safe disables capture
    rate(60, NULL); frame(); assert(s_active && s_prev == -1);
    mp6_fi_savestate_reset(); frame();
    assert(s_active && s_prev == -1 && s_latest >= 0);
    assert(ao_frames == animlogs); // AO and diagnostics never depend on retention
    puts("Capture policy: free-run, live toggles, env overrides and restore passed");
    return 0;
}
