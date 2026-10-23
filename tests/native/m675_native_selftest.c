#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "mp6_m675.h"
#include "mp6_widescreen.h"
#include "game/charman.h"
#include "game/data.h"
#include "game/gamework.h"
#include "game/memory.h"
#include "game/mg/score.h"
#include "game/mg/timer.h"
#include "m675_types.inc"

_Static_assert(offsetof(M675Collision, header) == 0, "sphere owns the shared header");
_Static_assert(offsetof(M675CollisionRegion, header) == 0, "region owns the shared header");
_Static_assert(offsetof(M675EllipseBoundary, header) == 0, "ellipse owns the shared header");

GW_PLAYER_CONF GwPlayerConf[GW_PLAYER_MAX];
static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
OMOBJ *lbl_1_bss_548, *lbl_1_bss_540, *lbl_1_bss_538;
OMOBJ *lbl_1_bss_2C8, *lbl_1_bss_2CC, *lbl_1_bss_2D0;
OMOBJ *emitters[3];
OMOBJ *lbl_1_bss_334[128];
OMOBJ **lbl_1_data_848[4] = {&emitters[0], &emitters[1], &emitters[2], NULL};
Vec lbl_1_data_E4 = {1, 2, 3}, lbl_1_data_D8 = {4, 5, 6};
u32 lbl_1_data_100[9];
M675PlayerConfig lbl_1_data_364;
static OMOBJ objects[16];
static s16 objectModels[16][1], objectMotions[16][9];
static unsigned objectCount, allocations;
static struct { unsigned char *raw; size_t size; } memory[16];
OMOBJ *omAddObjEx(OMOBJMAN *owner, s16 priority, u16 models, u16 motions,
                 s16 group, OMOBJ_FUNC callback) {
    assert(objectCount < 16);
    OMOBJ *object = &objects[objectCount];
    assert(models <= 1 && motions <= 9);
    object->mdlId = objectModels[objectCount];
    object->mtnId = objectMotions[objectCount++];
    object->objFunc = callback;
    return object;
}
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 tag) {
    assert(heap == HEAP_HEAP && tag == HU_MEMNUM_OVL && bytes > 0 && allocations < 16);
    unsigned char *raw = malloc(bytes + 32);
    assert(raw);
    memset(raw, 0xA5, bytes + 32);
    memory[allocations].raw = raw;
    memory[allocations++].size = bytes;
    return raw + 16;
}
static void check_guards(void) {
    for (unsigned n = 0; n < allocations; ++n)
        for (unsigned i = 0; i < 16; ++i) {
            assert(memory[n].raw[i] == 0xA5);
            assert(memory[n].raw[memory[n].size + 16 + i] == 0xA5);
        }
}
#define TRAP_CALLBACK(name) void name(OMOBJ *object) { __builtin_trap(); }
TRAP_CALLBACK(fn_1_F1A4)
TRAP_CALLBACK(fn_1_EA34)
TRAP_CALLBACK(fn_1_55F4)
TRAP_CALLBACK(fn_1_6198)
static unsigned fogLayers, objectCallbacks, shadowCalls, stoppedCameras;
void fn_1_11A0(OMOBJ *object);
void fn_1_11F8(s16 layer) { __builtin_trap(); }
void fn_1_2FD4(OMOBJ *object);
void fn_1_3094(OMOBJ *object) { assert(object == lbl_1_bss_2C8); }
void fn_1_5170(M675PlayerWork *work) { assert(work->actor.active == 1); }
void fn_1_6284(OMOBJ *object, s32 motion) { assert(motion == 0); }
void Hu3DFogSet(float start, float end, u8 r, u8 g, u8 b) { assert(start == 10 && end == 100); }
void Hu3DLayerHookSet(s16 layer, HU3D_LAYER_HOOK hook) {
    assert(layer == (s16)fogLayers++ && hook == fn_1_11F8);
}
void fn_1_1DD8(s32 camera, Vec *direction, Vec *target, float fov, float radius) {
    assert(camera == 5 && direction == &lbl_1_data_E4 && target == &lbl_1_data_D8);
    assert(fov == 24 && radius == 1000); ++shadowCalls;
}
void Hu3DShadowColSet(u8 r, u8 g, u8 b) { assert(r == 10 && g == 20 && b == 30); }
void fn_1_24CC(u32 mask) { assert(mask == 1); ++stoppedCameras; }
static void object_callback(OMOBJ *object) {
    assert(object == lbl_1_bss_2C8 || object == lbl_1_bss_2D0); ++objectCallbacks;
}
HU3D_MODELID CharModelCreate(s16 character, s16 model) { assert(character == 2 && model == 4); return 3; }
HU3D_MOTIONID CharMotionCreate(s16 character, u32 resource) { assert(character == 2); return 4; }
void CharMotionDataClose(s16 character) { assert(character == 2); }
void CharEffectLayerSet(s16 layer) { assert(layer == 6); }
void Hu3DModelLayerSet(HU3D_MODELID model, s16 layer) {
    assert((model == 3 && layer == 4) || (model == 6 && layer == 1));
}
void Hu3DModelShadowMapSet(HU3D_MODELID model) { assert(model == 3); }
void fn_1_7944(OMOBJ *object);
s32 fn_1_A38(s32 resource) { return resource; }
int fn_1_45C4(int resource) { __builtin_trap(); }
s32 fn_1_46CC(s32 model, s32 resource) { __builtin_trap(); }
void *HuDataSelHeapReadNum(s32 resource, s32 tag, HEAPID heap) {
    assert(resource >= 19 && resource <= 21 && tag == HU_MEMNUM_OVL && heap == HEAP_MODEL);
    return &models[6];
}
HU3D_MODELID Hu3DModelCreate(void *data) { assert(data == &models[6]); return 6; }
void Hu3DModelPosSet(HU3D_MODELID model, f32 x, f32 y, f32 z) {
    assert(model == 6 && x == 0 && y == 0 && z == 0);
}
void Hu3DModelRotSet(HU3D_MODELID model, f32 x, f32 y, f32 z) {
    assert(model == 6 && x == 0 && y == 0 && z == 0);
}
void Hu3DModelAttrSet(HU3D_MODELID model, u32 attr) { assert(model == 6 && attr == 0); }
void Hu3DModelShadowSet(HU3D_MODELID model) { __builtin_trap(); }
/* Resource 19 (sea side) widens four backdrop parts, resource 20 (town
 * side) two grass/edge parts; resource 21 none. */
static unsigned seaExtensions, townExtensions;
void mp6_widescreen_extend_sides(int16_t model, const char *name, int mirror) {
    assert(model == 6 && strcmp(name, "sora") == 0 && mirror == TRUE);
    ++seaExtensions;
}
void mp6_widescreen_extend_billboard_mirror(int16_t model, const char *name, int side) {
    assert(model == 6 && strcmp(name, "yama") == 0 && side == -1);
    ++seaExtensions;
}
void mp6_widescreen_extend_tiled_sloped_rim_at(int16_t model, const char *name, int occurrence, int axis,
                                               float boundary, float reach, float tolerance) {
    assert(model == 6 && strcmp(name, "obj24") == 0 && occurrence == 1 && axis == 2);
    (void)boundary; (void)reach; (void)tolerance;
    ++seaExtensions;
}
void mp6_widescreen_extend_shape_mirror(int16_t model, const char *name) {
    assert(model == 6 && strcmp(name, "umi") == 0);
    ++seaExtensions;
}
void mp6_widescreen_extend_terrain_outer_sides(int16_t model, const char *name, float centerX,
                                               float reach, int mirror) {
    assert(model == 6 && strcmp(name, "siba3") == 0 && centerX == 0 && mirror == TRUE);
    (void)reach;
    ++townExtensions;
}
void mp6_widescreen_extend_tiled_rim_sides(int16_t model, const char *name, float reach, float tolerance) {
    assert(model == 6 && strcmp(name, "bouha") == 0);
    (void)reach; (void)tolerance;
    ++townExtensions;
}
void omVibrate(s16 player, s16 time, s16 on, s16 off) { __builtin_trap(); }
s32 CharFXPlayPan(s16 character, s16 effect, s16 pan) { __builtin_trap(); }
s32 fn_1_32EC(Vec *position) { __builtin_trap(); }
void fn_1_4424(int effect, int handle, Vec *position, int pan, int volume) { __builtin_trap(); }
s16 MgSeqTimerValueGet(void) { return 47; }
static unsigned matrixCalls;
void C_MTXMultVecSR(const Mtx m, const Vec *src, Vec *dst) {
    Vec v = *src;
    for (unsigned i = 0; i < 3; ++i)
        ((float *)dst)[i] = m[i][0]*v.x + m[i][1]*v.y + m[i][2]*v.z;
    ++matrixCalls;
}
#include "m675_subject.inc"

static unsigned contacts;
static s32 contact(void *first, void *second) { assert(first && second); ++contacts; return 1; }
static void test_collision(void) {
    fn_1_F118(NULL);
    assert(memory[allocations-1].size == sizeof(M675CollisionWork));
    M675Collision moving[85] = {0};
    M675CollisionRegion regions[11] = {0};
    for (unsigned i = 0; i < 84; ++i) assert(fn_1_10170(&moving[i]) == (int)i);
    assert(fn_1_10170(&moving[84]) == -1);
    for (unsigned i = 0; i < 10; ++i) {
        regions[i].type = 100 + i; regions[i].index = i;
        assert(fn_1_10200(&regions[i]) == (int)i + 84);
        assert(fn_1_1039C(100 + i, i) == &regions[i]);
    }
    assert(fn_1_10200(&regions[10]) == -1);
    regions[9].type = 0x80000005u;
    assert(fn_1_1039C(0x80000005u, 9) == &regions[9]);
    assert(!fn_1_1039C(5, 9));
    fn_1_10290(0); fn_1_10290(83); fn_1_10290(84); fn_1_10290(93);
    assert(!fn_1_10310(0) && !fn_1_10310(83) && !fn_1_10310(84) && !fn_1_10310(93));
    assert(fn_1_10310(1) == &moving[1] && fn_1_10310(85) == &regions[1]);
    for (int i = -2; i < 0; ++i) { fn_1_10290(i); assert(!fn_1_10310(i)); }
    fn_1_10290(94); fn_1_10290(999); assert(!fn_1_10310(94));
    Vec p = {0, 0, 0}, q = {1, 0, 0};
    moving[0].position = &p; moving[0].radius = 1; moving[0].contact = contact;
    moving[1].position = &q; moving[1].radius = 1; moving[1].contact = contact;
    assert(fn_1_F4FC(&moving[0], &moving[1]) && contacts == 2);
    assert(p.x == -1 && q.x == 2);
    M675EllipseBoundary boundary = {0};
    boundary.shapeType = 11; boundary.radiusX = 10; boundary.radiusZ = 20; boundary.direction = 1;
    p = (Vec){20, 0, 0};
    assert(fn_1_FE84(&moving[0], &boundary));
    assert(fabsf(p.x-10) < .001f && contacts == 3);
    /* Region records deliberately have no sphere-only tail. */
    assert(fn_1_66EC(&moving[0], &boundary) == 1);
}
static void test_audio_fog_camera(void) {
    M675AudioConfigView config[2] = {{.cameras=1}, {0}};
    fn_1_2F38(NULL, config);
    M675AudioWork *audio = lbl_1_bss_2C8->data;
    assert(memory[allocations-1].size == sizeof(*audio) && audio->config == config);
    Vec positions[65] = {0};
    for (unsigned i = 0; i < 64; ++i) {
        assert(audio->entries[i].handle == -1);
        fn_1_4470(5, i+10, i, &positions[i], 1, 0);
        assert(audio->entries[i].position == &positions[i]);
    }
    fn_1_4470(1, 100, 100, &positions[64], 0, 0);
    assert(audio->entries[63].handle == 63 && audio->entries[63].position == &positions[63]);
    fn_1_3164(object_callback); fn_1_2FD4(lbl_1_bss_2C8);
    M675FogParam fog[3] = {{.layer=0,.start=10,.end=100}, {.layer=1}, {.layer=-1}};
    fn_1_10A0(NULL, fog);
    M675FogWork *fogWork = lbl_1_bss_2D0->data;
    assert(fogWork->parameters == fog && fogLayers == 2);
    fn_1_132C(.25f); assert(fogWork->factor == .25f && fogWork->parameters == fog);
    fn_1_12EC(object_callback); fn_1_11A0(lbl_1_bss_2D0); assert(objectCallbacks == 2);
    M675CameraWork camera = {.cameras=5, .shadowColor={10,20,30,255}};
    OMOBJ cameraObject = {.data=&camera}; lbl_1_bss_2CC = &cameraObject;
    fn_1_232C(24); Vec direction = {7,8,9}; fn_1_235C(&direction); fn_1_1FC8(fn_1_95C);
    assert(camera.shadowFov == 24 && camera.shadowDirection.z == 9);
    camera.callback(&cameraObject, 1, 0); camera.callback(&cameraObject, 4, 2);
    assert(shadowCalls == 1 && stoppedCameras == 2);
    assert(camera.entries[0].setupState == 1 && camera.entries[2].setupState == 1);
    assert(camera.entries[1].setupState == 0 && camera.initializationCount == 1);
    Vec a = {0,4,8}, b = {4,8,12}, result;
    fn_1_4BB4(&a, &b, .25f, &result); assert(result.x == 1 && result.y == 5 && result.z == 9);
    assert(fn_1_A60() == 47);
}
static void test_effects_and_players(void) {
    M675PropWork prop = {0}; M675EmitterRecord records[4] = {0};
    OMOBJ emitter = {.data=&prop}; emitter.work[0] = 4; prop.records = records; emitters[1] = &emitter;
    for (unsigned i = 0; i < 4; ++i) {
        records[i].state = records[i].enabled = 1; records[i].type = 1;
        records[i].playerIndex = 2; records[i].position.y = 20-i;
    }
    records[3].enabled = 0;
    assert(fn_1_D550(1,2) == &records[2].position);
    assert(!fn_1_D550(1,1) && !fn_1_D550(-1,0) && !fn_1_D550(3,0));
    M675EffectCollection effects = {0}; s16 effectModels[3] = {0,0,0};
    OMOBJ effectObject = {.mdlId=effectModels}; lbl_1_bss_538 = &effectObject; Hu3DData[0].hookData = &effects;
    s16 models[1], motions[9]; OMOBJ player = {.mdlId=models, .mtnId=motions};
    player.work[0] = 3; player.work[1] = 2; player.work[2] = 1; GwPlayerConf[3].charNo = 2;
    fn_1_527C(&player); M675PlayerWork *work = player.data;
    assert(memory[allocations-1].size == sizeof(*work));
    assert(work->actor.collision.position == &work->actor.position && work->actor.collision.owner == &player);
    assert(work->config.movementScale == lbl_1_data_364.movementScale && work->playerIndex == 3);
    assert(effects.records[0] == &work->effect && work->effect.position == &work->actor.position);
    M675EffectRegistration more[5] = {0};
    for (unsigned i = 0; i < 5; ++i) fn_1_E918(&more[i]);
    assert(effects.records[4] == &more[3]);
    fn_1_E9A8(NULL); ScoreDisplayWork *score = lbl_1_bss_540->data;
    assert(memory[allocations-1].size == sizeof(*score)); score->displayState = 1;
    for (int i = 0; i < 4; ++i) { fn_1_EF88(i, i+7); assert(fn_1_F058(i) == i+7); }
    fn_1_EF88(3, 100); assert(fn_1_F058(3) == 99);
}
static void test_stage_initialization(void) {
    Vec position = {10, 20, 30};
    M675PropParameter parameter = {.count=1, .layer=1, .position=&position, .rotationY=45};
    for (int resource = 19; resource <= 21; ++resource) {
        parameter.modelResource = resource;
        fn_1_7684(NULL, &parameter);
        OMOBJ *object = objects + objectCount - 1;
        M675PropWork *work = object->data;
        assert(memory[allocations-1].size == sizeof(*work));
        assert(lbl_1_bss_334[resource-19] == object && work->parameters == &parameter);
        assert(work->initialPosition.z == 30 && work->state.actor.position.x == 10);
        assert(work->state.actor.rotation.y == 45 && object->mdlId[0] == 6);
        /* No viewport branch: registration must also happen on a cold 4:3 load.
         * The production helper's geometry and toggle behavior has its own tests. */
        assert(seaExtensions == 4 && townExtensions == (resource >= 20 ? 2u : 0u));
    }
}
int main(void) {
    test_collision(); test_audio_fog_camera(); test_effects_and_players(); test_stage_initialization();
    Mtx m = {{2,0,0,999},{0,3,0,999},{0,0,4,999}};
    Vec v[2] = {{1,2,3},{4,5,6}}, out[2];
    PSMTXMultVecArraySR(m, v, out, 2);
    assert(out[0].x == 2 && out[0].y == 6 && out[1].z == 24);
    PSMTXMultVecArraySR(m, v, v, 2); assert(v[1].x == 8 && v[0].z == 12);
    PSMTXMultVecArraySR(m, NULL, NULL, 0); assert(matrixCalls == 4);
    check_guards();
    for (unsigned i = 0; i < allocations; ++i) free(memory[i].raw);
    return 0;
}
