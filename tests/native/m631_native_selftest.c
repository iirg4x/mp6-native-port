#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp6_m631_native.h"
#include "game/memory.h"
#include "game/object.h"

/* The recovered overlay's mutable storage, retained at native pointer width. */
HUPROCESS *lbl_1_bss_C;
OMOBJ *lbl_1_bss_8;
s32 lbl_1_bss_4;
s32 lbl_1_bss_0;
M631MotionPairNode *lbl_1_bss_10;
M631Job lbl_1_bss_14[256];
M631Jobs lbl_1_bss_1414;

MGSEQ_PARAM lbl_1_data_0;
s32 lbl_1_data_28[30];
s32 lbl_1_data_A0;
u32 lbl_1_data_100[8];
s16 GwMgNightF;

/* This callback is not part of the shared header because it is actor-private. */
void fn_1_A4FC(M631MotionPairNode *callback,
               M631MotionPairNode *context, f32 distance);
void fn_1_A0(void);

/* Engine seams used by the selected setup function. */
static HUPROCESS object_manager;
static OMOBJ object_storage[5];
static int object_count;
static int scene_init_calls;
static int actor_init_calls;
static int setup_scene_calls;
static int setup_count;
static int no_sync_value;

#define GUARD_BYTES 32
static _Alignas(16) unsigned char scene_allocation[
    GUARD_BYTES + sizeof(M631SceneWork) + GUARD_BYTES];
static _Alignas(16) unsigned char actor_allocations[4][
    GUARD_BYTES + sizeof(M631ActorWork) + GUARD_BYTES];

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}

static void fill_guards(unsigned char *storage, size_t size) {
    memset(storage, 0xA5, size);
}

static void check_guards(const unsigned char *storage, size_t size) {
    size_t index;
    for (index = 0; index < GUARD_BYTES; ++index) {
        assert(storage[index] == 0xA5);
        assert(storage[size - GUARD_BYTES + index] == 0xA5);
    }
}

OMOBJMAN *omInitObjMan(s16 objMax, s32 objManPrio) {
    assert(objMax == 10);
    assert(objManPrio == 4096);
    object_count = 0;
    memset(object_storage, 0, sizeof(object_storage));
    return &object_manager;
}

void omGameSysInit(OMOBJMAN *objMan) {
    assert(objMan == &object_manager);
}

OMOBJ *omAddObjEx(OMOBJMAN *objMan, s16 priority, u16 modelCount,
                  u16 motionCount, s16 groupNo, OMOBJ_FUNC objFunc) {
    OMOBJ *object;
    assert(objMan == &object_manager);
    assert(object_count < (int)(sizeof(object_storage) / sizeof(object_storage[0])));
    if (object_count == 0) {
        assert(priority == 101 && modelCount == 0 && motionCount == 0);
        assert(groupNo == -1);
    } else {
        assert(priority == 100 && modelCount == 1 && motionCount == 8);
        assert(groupNo == 0);
    }
    object = &object_storage[object_count++];
    object->objFunc = objFunc;
    return object;
}

void *HuMemDirectMallocNum(HEAPID heap, s32 size, u32 allocationNumber) {
    assert(heap == HEAP_HEAP);
    assert(allocationNumber == HU_MEMNUM_OVL);
    if (size == (s32)sizeof(M631SceneWork)) {
        assert(setup_count == 0);
        fill_guards(scene_allocation, sizeof(scene_allocation));
        setup_count += 1;
        return scene_allocation + GUARD_BYTES;
    }
    if (size == (s32)sizeof(M631ActorWork)) {
        int actorIndex = setup_count - 1;
        assert(actorIndex >= 0 && actorIndex < 4);
        fill_guards(actor_allocations[actorIndex],
                    sizeof(actor_allocations[actorIndex]));
        setup_count += 1;
        return actor_allocations[actorIndex] + GUARD_BYTES;
    }
    fail("unexpected native m631 allocation size");
    return NULL;
}

void Hu3DBGColorSet(u8 red, u8 green, u8 blue) {
    assert(red == 0 && green == 0 && blue == 0);
}

void Hu3DNoSyncSet(BOOL noSync) {
    no_sync_value = noSync;
}

void MgSeqCreate(MGSEQ_PARAM *parameter) {
    assert(parameter == &lbl_1_data_0);
}

void fn_1_174C(OMOBJ *object) {
    assert(object != NULL);
}

void fn_1_5284(OMOBJ *object) {
    assert(object != NULL);
}

static void assert_scene_pointer(M631SceneWork *scene) {
    assert((void *)scene == scene_allocation + GUARD_BYTES);
    scene_init_calls += 1;
}

void fn_1_162C(M631SceneWork *scene) {
    assert_scene_pointer(scene);
}

void fn_1_121C(M631SceneWork *scene) {
    assert_scene_pointer(scene);
}

void fn_1_1374(M631SceneWork *scene) {
    assert_scene_pointer(scene);
}

void fn_1_14D0(M631SceneWork *scene) {
    assert_scene_pointer(scene);
}

/* Contact owners deliberately carry a 64-bit cookie and are never integer-cast. */
typedef struct ContactOwner {
    uintptr_t cookie;
    int beforeCalls;
    int afterCalls;
} ContactOwner;

static ContactOwner contactOwners[2];
static int pairCalls;
static f32 lastPairDistance;

static void contact_before(void *owner) {
    ContactOwner *value = (ContactOwner *)owner;
    assert(owner == &contactOwners[0] || owner == &contactOwners[1]);
    assert(value->cookie > UINT32_MAX);
    value->beforeCalls += 1;
}

static s32 contact_after(void *owner) {
    ContactOwner *value = (ContactOwner *)owner;
    assert(owner == &contactOwners[0] || owner == &contactOwners[1]);
    assert(value->cookie > UINT32_MAX);
    value->afterCalls += 1;
    return 0;
}

static void contact_resolve(M631MotionPairNode *first,
                            M631MotionPairNode *second, f32 distance) {
    assert(first != second);
    assert(first->owner == &contactOwners[0] ||
           first->owner == &contactOwners[1]);
    assert(second->owner == &contactOwners[0] ||
           second->owner == &contactOwners[1]);
    assert(distance > 2.9f && distance < 3.1f);
    pairCalls += 1;
    lastPairDistance = distance;
}

typedef struct JobPayload {
    uintptr_t cookie;
    int calls;
    int complete;
} JobPayload;

static s32 job_callback(void *data) {
    JobPayload *payload = (JobPayload *)data;
    assert(payload->cookie > UINT32_MAX);
    payload->calls += 1;
    return payload->complete;
}

static void assert_zero(const void *memory, size_t size) {
    const unsigned char *bytes = (const unsigned char *)memory;
    size_t index;
    for (index = 0; index < size; ++index) {
        assert(bytes[index] == 0);
    }
}

static void test_reset_dispatch(void) {
    unsigned char savedData0[sizeof(lbl_1_data_0)];
    unsigned char savedData28[sizeof(lbl_1_data_28)];
    unsigned char savedData100[sizeof(lbl_1_data_100)];
    unsigned char savedDataA0[sizeof(lbl_1_data_A0)];

    memset(&lbl_1_data_0, 0x11, sizeof(lbl_1_data_0));
    memset(&lbl_1_data_28, 0x22, sizeof(lbl_1_data_28));
    memset(&lbl_1_data_A0, 0x33, sizeof(lbl_1_data_A0));
    memset(&lbl_1_data_100, 0x44, sizeof(lbl_1_data_100));
    memset(&lbl_1_bss_C, 0xA5, sizeof(lbl_1_bss_C));
    memset(&lbl_1_bss_8, 0xA5, sizeof(lbl_1_bss_8));
    memset(&lbl_1_bss_4, 0xA5, sizeof(lbl_1_bss_4));
    memset(&lbl_1_bss_0, 0xA5, sizeof(lbl_1_bss_0));
    memset(&lbl_1_bss_10, 0xA5, sizeof(lbl_1_bss_10));
    memset(&lbl_1_bss_14, 0xA5, sizeof(lbl_1_bss_14));
    memset(&lbl_1_bss_1414, 0xA5, sizeof(lbl_1_bss_1414));

    mp6_m631_reset();
    memcpy(savedData0, &lbl_1_data_0, sizeof(savedData0));
    memcpy(savedData28, &lbl_1_data_28, sizeof(savedData28));
    memcpy(savedDataA0, &lbl_1_data_A0, sizeof(savedDataA0));
    memcpy(savedData100, &lbl_1_data_100, sizeof(savedData100));
    assert_zero(&lbl_1_bss_C, sizeof(lbl_1_bss_C));
    assert_zero(&lbl_1_bss_8, sizeof(lbl_1_bss_8));
    assert_zero(&lbl_1_bss_4, sizeof(lbl_1_bss_4));
    assert_zero(&lbl_1_bss_0, sizeof(lbl_1_bss_0));
    assert_zero(&lbl_1_bss_10, sizeof(lbl_1_bss_10));
    assert_zero(&lbl_1_bss_14, sizeof(lbl_1_bss_14));
    assert_zero(&lbl_1_bss_1414, sizeof(lbl_1_bss_1414));

    memset(&lbl_1_data_0, 0xA5, sizeof(lbl_1_data_0));
    memset(&lbl_1_data_28, 0xA5, sizeof(lbl_1_data_28));
    memset(&lbl_1_data_A0, 0xA5, sizeof(lbl_1_data_A0));
    memset(&lbl_1_data_100, 0xA5, sizeof(lbl_1_data_100));
    memset(&lbl_1_bss_C, 0xA5, sizeof(lbl_1_bss_C));
    memset(&lbl_1_bss_8, 0xA5, sizeof(lbl_1_bss_8));
    memset(&lbl_1_bss_4, 0xA5, sizeof(lbl_1_bss_4));
    memset(&lbl_1_bss_0, 0xA5, sizeof(lbl_1_bss_0));
    memset(&lbl_1_bss_10, 0xA5, sizeof(lbl_1_bss_10));
    memset(&lbl_1_bss_14, 0xA5, sizeof(lbl_1_bss_14));
    memset(&lbl_1_bss_1414, 0xA5, sizeof(lbl_1_bss_1414));
    mp6_m631_reset();
    assert(memcmp(&lbl_1_data_0, savedData0, sizeof(savedData0)) == 0);
    assert(memcmp(&lbl_1_data_28, savedData28, sizeof(savedData28)) == 0);
    assert(memcmp(&lbl_1_data_A0, savedDataA0, sizeof(savedDataA0)) == 0);
    assert(memcmp(&lbl_1_data_100, savedData100, sizeof(savedData100)) == 0);
    assert_zero(&lbl_1_bss_C, sizeof(lbl_1_bss_C));
    assert_zero(&lbl_1_bss_8, sizeof(lbl_1_bss_8));
    assert_zero(&lbl_1_bss_4, sizeof(lbl_1_bss_4));
    assert_zero(&lbl_1_bss_0, sizeof(lbl_1_bss_0));
    assert_zero(&lbl_1_bss_10, sizeof(lbl_1_bss_10));
    assert_zero(&lbl_1_bss_14, sizeof(lbl_1_bss_14));
    assert_zero(&lbl_1_bss_1414, sizeof(lbl_1_bss_1414));
}

static void test_contact_list(void) {
    M631MotionPairNode first;
    M631MotionPairNode second;
    HuVecF firstPosition = {0.0f, 1.0f, 0.0f};
    HuVecF secondPosition = {1.0f, 2.0f, 0.0f};

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    contactOwners[0].cookie = UINT64_C(0x100000001);
    contactOwners[1].cookie = UINT64_C(0x200000002);
    contactOwners[0].beforeCalls = contactOwners[0].afterCalls = 0;
    contactOwners[1].beforeCalls = contactOwners[1].afterCalls = 0;
    pairCalls = 0;
    lastPairDistance = 0.0f;

    fn_1_AB4C();
    fn_1_AFF8(&first, 2.0f, 3);
    fn_1_AFF8(&second, 2.0f, 4);
    assert(first.radius == 2.0f && first.state == 3);
    assert(second.radius == 2.0f && second.state == 4);
    fn_1_AF2C(&first, &contactOwners[0], contact_before, contact_after,
               contact_resolve, &firstPosition);
    fn_1_AF2C(&second, &contactOwners[1], contact_before, contact_after,
               contact_resolve, &secondPosition);
    assert(fn_1_B004() == &second);
    assert(second.next == &first && first.previous == &second);

    fn_1_AB60();
    assert(contactOwners[0].beforeCalls == 1);
    assert(contactOwners[1].beforeCalls == 1);
    assert(contactOwners[0].afterCalls == 1);
    assert(contactOwners[1].afterCalls == 1);
    assert(pairCalls == 2 && lastPairDistance > 2.9f && lastPairDistance < 3.1f);
    assert(firstPosition.x == 0.0f && firstPosition.y == 1.0f);
    assert(secondPosition.x == 1.0f && secondPosition.y == 2.0f);

    fn_1_AF94(&first);
    assert(fn_1_B004() == &second && second.previous == NULL);
    fn_1_AF94(&second);
    assert(fn_1_B004() == NULL);
}

static void test_job_pool(void) {
    JobPayload single = {UINT64_C(0x300000003), 0, 0};
    JobPayload payloads[256];
    M631Job *jobs[256];
    uint64_t beforeGuard = UINT64_C(0xA5A5A5A5A5A5A5A5);
    uint64_t afterGuard = UINT64_C(0x5A5A5A5A5A5A5A5A);
    int index;

    fn_1_B014();
    assert(lbl_1_bss_1414.storage == &lbl_1_bss_14[0]);
    assert(lbl_1_bss_1414.freeHead == &lbl_1_bss_14[0]);
    assert(lbl_1_bss_1414.activeHead == NULL);

    jobs[0] = fn_1_B0B8(job_callback, &single);
    assert(jobs[0] != NULL && jobs[0]->data == &single);
    fn_1_B120(&jobs[0]);
    assert(jobs[0] == NULL && lbl_1_bss_1414.activeHead == NULL);
    assert(beforeGuard == UINT64_C(0xA5A5A5A5A5A5A5A5));
    assert(afterGuard == UINT64_C(0x5A5A5A5A5A5A5A5A));

    for (index = 0; index < 256; ++index) {
        payloads[index].cookie = UINT64_C(0x400000000) + (uintptr_t)index;
        payloads[index].calls = 0;
        payloads[index].complete = 1;
        jobs[index] = fn_1_B0B8(job_callback, &payloads[index]);
        assert(jobs[index] != NULL);
        assert(jobs[index]->owner == &lbl_1_bss_1414);
        assert(jobs[index]->data == &payloads[index]);
        assert(jobs[index]->callback == job_callback);
    }
    assert(fn_1_B0B8(job_callback, &single) == NULL);
    fn_1_B1AC();
    assert(lbl_1_bss_1414.activeHead == NULL);
    assert(lbl_1_bss_1414.freeHead != NULL);
    for (index = 0; index < 256; ++index) {
        assert(payloads[index].calls == 1);
    }
    assert(beforeGuard == UINT64_C(0xA5A5A5A5A5A5A5A5));
    assert(afterGuard == UINT64_C(0x5A5A5A5A5A5A5A5A));
}

static void test_typed_pair_resolver(void) {
    M631MotionController controller;
    M631MotionPairNode callback;
    M631MotionPairNode context;
    HuVecF callbackPosition = {0.0f, 0.0f, 0.0f};
    HuVecF contextPosition = {0.0f, 0.0f, 10.0f};
    void (*resolver)(M631MotionPairNode *, M631MotionPairNode *, f32);
    f32 oldX;
    f32 oldZ;

    memset(&controller, 0, sizeof(controller));
    memset(&callback, 0, sizeof(callback));
    memset(&context, 0, sizeof(context));
    callback.owner = &controller;
    callback.position = &callbackPosition;
    context.position = &contextPosition;
    context.state = 3;
    context.radius = 10.0f;
    resolver = fn_1_A4FC;
    oldX = controller.avoidanceDirection.x;
    oldZ = controller.avoidanceDirection.z;
    resolver(&callback, &context, 2.5f);
    assert(controller.avoidanceDirection.x != oldX ||
           controller.avoidanceDirection.z != oldZ);
}

static void test_native_setup_allocations(void) {
    int index;

    object_count = 0;
    setup_count = 0;
    scene_init_calls = 0;
    no_sync_value = 0;
    fn_1_A0();
    assert(object_count == 5);
    assert(setup_count == 5);
    assert(scene_init_calls == 4);
    assert(no_sync_value == 1);
    assert(lbl_1_bss_C == &object_manager);
    assert(lbl_1_bss_8 == &object_storage[0]);
    assert(object_storage[0].objFunc == fn_1_174C);
    for (index = 1; index < 5; ++index) {
        assert(object_storage[index].objFunc == fn_1_5284);
        assert(object_storage[index].work[0] == (u32)(index - 1));
        assert(object_storage[index].data == actor_allocations[index - 1] + GUARD_BYTES);
    }
    assert(object_storage[0].data == scene_allocation + GUARD_BYTES);
    check_guards(scene_allocation, sizeof(scene_allocation));
    for (index = 0; index < 4; ++index) {
        check_guards(actor_allocations[index], sizeof(actor_allocations[index]));
    }
}

/* M631_NATIVE_SUBJECT */

int main(void) {
    _Static_assert(sizeof(void *) >= 8, "native probe requires host pointers");
    _Static_assert(offsetof(M631MotionPairNode, owner) > sizeof(void *),
                   "native owner field must remain a pointer");
    test_reset_dispatch();
    test_contact_list();
    test_job_pool();
    test_typed_pair_resolver();
    test_native_setup_allocations();
    puts("PASS");
    return 0;
}
