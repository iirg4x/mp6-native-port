#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "mp6_m675.h"
#include "mp6_widescreen.h"
#include "m675_staging_types.inc"

/* The subject is the recovered player body. Keep its native vector operation
 * visible while making the fixture independent of the full game runtime. */
#define PSVECAdd(a, b, c) do { \
    Vec m675_add_a = *(a); \
    Vec m675_add_b = *(b); \
    (c)->x = m675_add_a.x + m675_add_b.x; \
    (c)->y = m675_add_a.y + m675_add_b.y; \
    (c)->z = m675_add_a.z + m675_add_b.z; \
} while (0)

M675PlayerWork *lbl_1_bss_2E4[5];
Vec lbl_1_data_1A8[4] = {
    {-1300.0f, 200.0f, 660.0f},
    {-1000.0f, 200.0f, 660.0f},
    {1000.0f, 200.0f, 660.0f},
    {1300.0f, 200.0f, 660.0f},
};
f32 lbl_1_data_1D8[4] = {0.0f, 0.0f, 0.0f, 0.0f};

static f32 liveScale = 1.0f;
f32 mp6_widescreen_scale_factor(void) { return liveScale; }

/* fn_1_55F4's unrelated native work is deliberately inert here. Returning
 * zero leaves each phase in place, so the fixture isolates the staging gate. */
void fn_1_74C8(M675ActorState *state) { (void)state; }
void fn_1_61F4(M675ActorState *state) { (void)state; }
s32 fn_1_5740(M675PlayerWork *work) { (void)work; return 0; }
s32 fn_1_5998(M675PlayerWork *work) { (void)work; return 0; }
s32 fn_1_5CB4(M675PlayerWork *work) { (void)work; return 0; }
s32 fn_1_5EB4(M675PlayerWork *work) { (void)work; return 0; }
void fn_1_67CC(M675PlayerWork *state) { (void)state; }
void fn_1_621C(OMOBJ *object) { (void)object; }
void fn_1_5FCC(OMOBJ *object) { (void)object; }
s32 fn_1_66EC(void *callback, void *event) { (void)callback; (void)event; return 0; }

#include "m675_staging_subject.inc"

static int nearf(f32 left, f32 right) {
    return fabsf(left - right) < 0.001f;
}

static void assert_vec_eq(Vec actual, Vec expected) {
    assert(nearf(actual.x, expected.x));
    assert(nearf(actual.y, expected.y));
    assert(nearf(actual.z, expected.z));
}

static void assert_actor_runtime_unchanged(const M675ActorState *actual,
                                           const M675ActorState *before) {
    assert_vec_eq(actual->velocity, before->velocity);
    assert_vec_eq(actual->rotation, before->rotation);
    assert(nearf(actual->baseYaw, before->baseYaw));
    assert(actual->buttonsDown == before->buttonsDown);
    assert(actual->buttons == before->buttons);
    assert(nearf(actual->stickAngle, before->stickAngle));
    assert(nearf(actual->stickMagnitude, before->stickMagnitude));
    assert(actual->motion == before->motion);
    assert(actual->motionFrame == before->motionFrame);
    assert(actual->requestedMotion == before->requestedMotion);
    assert(nearf(actual->motionSpeed, before->motionSpeed));
    assert(actual->active == before->active);
    assert(actual->formationIndex == before->formationIndex);
}

static void assert_player_runtime_unchanged(const M675PlayerWork *actual,
                                            const M675PlayerWork *before) {
    assert(actual->phase == before->phase);
    assert(actual->phaseTimer == before->phaseTimer);
    assert(actual->updateCounter == before->updateCounter);
    assert(actual->computerPhase == before->computerPhase);
    assert(actual->actionDelay == before->actionDelay);
    assert(actual->randomDelay == before->randomDelay);
    assert(nearf(actual->triggerHeight, before->triggerHeight));
    assert(actual->motionActive == before->motionActive);
    assert(actual->motionTimer == before->motionTimer);
    assert(actual->movementInProgress == before->movementInProgress);
    assert(actual->contactEvent == before->contactEvent);
    assert(actual->contactCooldown == before->contactCooldown);
    assert(actual->contactPhase == before->contactPhase);
    assert(actual->initializedCounter == before->initializedCounter);
    assert(nearf(actual->initializedScale, before->initializedScale));
    assert(actual->processPhase == before->processPhase);
    assert(actual->updateCount == before->updateCount);
    assert(actual->voiceState == before->voiceState);
    assert_actor_runtime_unchanged(&actual->actor, &before->actor);
}

static void test_helper_is_native_baselined_and_side_effect_free(void) {
    const Vec authored[4] = {
        {-1300.0f, 200.0f, 660.0f},
        {-1000.0f, 200.0f, 660.0f},
        {1000.0f, 200.0f, 660.0f},
        {1300.0f, 200.0f, 660.0f},
    };
    M675PlayerWork work = {0};
    M675PlayerWork before;

    work.actor.formationIndex = 0;
    work.actor.basePosition = (Vec){11.0f, 22.0f, 33.0f};
    work.actor.position = (Vec){44.0f, 55.0f, 66.0f};
    work.actor.velocity = (Vec){7.0f, 8.0f, 9.0f};
    work.actor.rotation = (Vec){10.0f, 11.0f, 12.0f};
    work.actor.baseYaw = 13.0f;
    work.actor.buttonsDown = 14;
    work.actor.buttons = 15;
    work.actor.stickAngle = 16.0f;
    work.actor.stickMagnitude = 17.0f;
    work.actor.motion = 18;
    work.actor.motionFrame = 19;
    work.actor.requestedMotion = 20;
    work.actor.motionSpeed = 21.0f;
    work.actor.active = 1;
    work.phase = 22;
    work.phaseTimer = 23;
    work.updateCounter = 24;
    work.computerPhase = 25;
    work.actionDelay = 26;
    work.randomDelay = 27;
    work.triggerHeight = 28.0f;
    work.motionActive = 1;
    work.motionTimer = 29;
    work.movementInProgress = 1;
    work.contactEvent = 30;
    work.contactCooldown = 31;
    work.contactPhase = 32;
    work.initializedCounter = 33;
    work.initializedScale = 34.0f;
    work.processPhase = 35;
    work.updateCount = 36;
    work.voiceState = 37;

    assert(memcmp(lbl_1_data_1A8, authored, sizeof(authored)) == 0);
    before = work;
    liveScale = 1.0f;
    mp6_m675_stage_player(&work);
    assert_vec_eq(work.actor.basePosition, authored[0]);
    assert_vec_eq(work.actor.position, authored[0]);
    assert_player_runtime_unchanged(&work, &before);
    assert(memcmp(lbl_1_data_1A8, authored, sizeof(authored)) == 0);

    before = work;
    liveScale = 1.6f;
    mp6_m675_stage_player(&work);
    assert_vec_eq(work.actor.basePosition, (Vec){-2080.0f, 200.0f, 660.0f});
    assert_vec_eq(work.actor.position, (Vec){-2080.0f, 200.0f, 660.0f});
    assert_player_runtime_unchanged(&work, &before);
    assert(memcmp(lbl_1_data_1A8, authored, sizeof(authored)) == 0);

    /* A live resize must always re-read the authored native point. */
    liveScale = 2.1f;
    mp6_m675_stage_player(&work);
    assert_vec_eq(work.actor.basePosition, (Vec){-2730.0f, 200.0f, 660.0f});
    liveScale = 1.6f;
    mp6_m675_stage_player(&work);
    assert_vec_eq(work.actor.basePosition, (Vec){-2080.0f, 200.0f, 660.0f});
    assert(memcmp(lbl_1_data_1A8, authored, sizeof(authored)) == 0);
}

static void test_initial_registration_uses_live_scale(void) {
    const Vec authored[4] = {
        {-1300.0f, 200.0f, 660.0f},
        {-1000.0f, 200.0f, 660.0f},
        {1000.0f, 200.0f, 660.0f},
        {1300.0f, 200.0f, 660.0f},
    };
    M675PlayerWork work = {0};

    memset(lbl_1_bss_2E4, 0, sizeof(lbl_1_bss_2E4));
    work.actor.formationIndex = 3;
    work.actor.velocity = (Vec){7.0f, 8.0f, 9.0f};
    work.actor.rotation = (Vec){1.0f, 2.0f, 3.0f};
    work.actor.baseYaw = 42.0f;
    liveScale = 2.1f;
    fn_1_5170(&work);
    assert(lbl_1_bss_2E4[0] == &work);
    assert_vec_eq(work.actor.basePosition, (Vec){2730.0f, 200.0f, 660.0f});
    assert_vec_eq(work.actor.position, (Vec){2730.0f, 200.0f, 660.0f});
    assert_vec_eq(work.actor.velocity, (Vec){0.0f, 0.0f, 0.0f});
    assert(nearf(work.actor.baseYaw, 0.0f));
    assert_vec_eq(work.actor.rotation, (Vec){1.0f, 0.0f, 3.0f});
    assert(work.contactPhase == 0 && work.initializedCounter == 0);
    assert(nearf(work.initializedScale, 1.0f));
    assert(memcmp(lbl_1_data_1A8, authored, sizeof(authored)) == 0);
}

static void test_all_slots_clear_the_projected_frame(void) {
    const float scales[] = {1.0f, 1.325f, 1.75f, 2.1f};
    M675PlayerWork work = {0};
    /* Measured opening camera: (y,z)=(644.288635,1882.66284),
     * forward=(0,-.090157986,-.995927453), vertical FOV=45, aspect=1.2*k.
     * Keep a conservative 150-world-unit character margin beyond the edge. */
    const float depth = (200.0f - 644.288635f) * -.090157986f +
                        (660.0f - 1882.66284f) * -.995927453f;
    for (unsigned k = 0; k < sizeof(scales) / sizeof(scales[0]); ++k) {
        liveScale = scales[k];
        for (int slot = 0; slot < 4; ++slot) {
            work.actor.formationIndex = slot;
            mp6_m675_stage_player(&work);
            assert(nearf(work.actor.position.x, lbl_1_data_1A8[slot].x * liveScale));
            assert(work.actor.position.y == 200.0f && work.actor.position.z == 660.0f);
            float edge = depth * .41421356237f * 1.2f * liveScale;
            assert(fabsf(work.actor.position.x + 3.77205992f) - 150.0f > edge);
        }
    }
}

static void run_update_case(M675PlayerWork *state, OMOBJ *object,
                            s32 processPhase, s32 phase, int shouldStage) {
    const Vec native = {-1000.0f, 200.0f, 660.0f};
    state->actor.formationIndex = 1;
    state->actor.basePosition = (Vec){91.0f, 92.0f, 93.0f};
    state->actor.position = (Vec){101.0f, 102.0f, 103.0f};
    state->actor.velocity = (Vec){0.0f, 0.0f, 0.0f};
    state->processPhase = processPhase;
    state->phase = phase;
    object->data = state;
    liveScale = 1.6f;
    fn_1_55F4(object);
    if (shouldStage) {
        assert_vec_eq(state->actor.basePosition,
                      (Vec){-1600.0f, 200.0f, 660.0f});
        assert_vec_eq(state->actor.position,
                      (Vec){-1600.0f, 200.0f, 660.0f});
    } else {
        assert_vec_eq(state->actor.basePosition, (Vec){91.0f, 92.0f, 93.0f});
        assert_vec_eq(state->actor.position, (Vec){101.0f, 102.0f, 103.0f});
    }
    (void)native;
}

static void test_waiting_gate_excludes_later_phases(void) {
    M675PlayerWork state = {0};
    OMOBJ object = {0};

    /* Only the two waiting subphases stage the widened entrance. */
    run_update_case(&state, &object, 0, 0, 1);
    run_update_case(&state, &object, 0, 1, 1);
    run_update_case(&state, &object, 0, 2, 0);
    /* Process phases 1/2/3 are gameplay, exit, and results; they must not
     * reset a player to its entrance target on a live update. */
    run_update_case(&state, &object, 1, 0, 0);
    run_update_case(&state, &object, 2, 0, 0);
    run_update_case(&state, &object, 3, 0, 0);
}

int main(void) {
    test_helper_is_native_baselined_and_side_effect_free();
    test_initial_registration_uses_live_scale();
    test_all_slots_clear_the_projected_frame();
    test_waiting_gate_excludes_later_phases();
    return 0;
}
