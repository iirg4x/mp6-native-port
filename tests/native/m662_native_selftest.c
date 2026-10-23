#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp6_m662_native.h"
#include "game/object.h"
#include "game/printfunc.h"

#define DATA_m662 0
#define DATANUM(data, number) (number)

/* The selected functions use these scene globals directly. */
u16 lbl_1_bss_C60;
s16 lbl_1_bss_C58[4];
HUPROCESS *lbl_1_bss_C50[2];
M662CameraState lbl_1_bss_B08;
M662Light lbl_1_bss_ACC;
M662Shadow lbl_1_bss_A7C;
M662ParticleModule lbl_1_bss_A74;
M662Model lbl_1_bss_4B4[46];
M662Motion lbl_1_bss_3F4[2];
M662Player lbl_1_bss_2F4[2];
M662Sequence lbl_1_bss_210;
M662Puzzle lbl_1_bss_D0[4];
M662Bounds lbl_1_bss_0[13];
MGSEQ_PARAM lbl_1_data_0;
s16 lbl_1_data_F6[5];
M662SinePath lbl_1_data_150[17];
M662LinearPath lbl_1_data_32C[10];
s32 lbl_1_data_448[40];
char lbl_1_data_4E8[4][32];

HU3D_MODEL hu3d_storage[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = hu3d_storage;
HuVecF Center, CRot;
float CZoom;
int fontcolor;
s16 GwMgNightF;

typedef struct MotionIds { u32 resourceIds[6]; } MotionIds;
typedef struct M662CameraIds { s16 cameraMasks[4]; } M662CameraIds;
typedef struct M662CameraPresetTable { f32 parameters[4][13]; } M662CameraPresetTable;

static int setup_camera_calls;
static int setup_light_calls;
static int setup_shadow_calls;
static int setup_model_calls;
static int setup_audio_calls;
static int setup_next_calls;
static int camera_create_mask;
static int camera_mapper_calls;
static float camera_viewport[6];
static float camera_perspective[4];
static int path_sound_calls;

/* fn_1_130 deliberately delegates these engine-owned setup stages. */
void fn_1_1918(s16 cameraIndex) {
    assert(cameraIndex == 0);
    setup_camera_calls += 1;
}
void fn_1_1DFC(void) { setup_light_calls += 1; }
void fn_1_223C(void) { setup_shadow_calls += 1; }
int fn_1_8B34(void) { setup_model_calls += 1; return 0; }
void fn_1_4D04(void) { setup_audio_calls += 1; }
u32 MgSeqModeNext(void) { setup_next_calls += 1; return 0; }
void Hu3DBGColorSet(u8 r, u8 g, u8 b) {
    assert(r == 0 && g == 0 && b == 0);
}

/* The real preset helper below calls this shared mapping seam. */
void fn_1_1DC8(s16 cameraIndex) {
    assert(cameraIndex >= 0 && cameraIndex < 4);
    camera_mapper_calls += 1;
}
void Hu3DCameraCreate(int cameraBit) { camera_create_mask = cameraBit; }
void Hu3DCameraViewportSet(int cameraBit, float x, float y, float w,
                           float h, float nearZ, float farZ) {
    (void)cameraBit; (void)nearZ; (void)farZ;
    camera_viewport[0] = x; camera_viewport[1] = y;
    camera_viewport[2] = w; camera_viewport[3] = h;
    camera_viewport[4] = nearZ; camera_viewport[5] = farZ;
}
void Hu3DCameraPerspectiveSet(int cameraBit, float fov, float nearZ,
                              float farZ, float aspect) {
    (void)cameraBit;
    camera_perspective[0] = fov; camera_perspective[1] = nearZ;
    camera_perspective[2] = farZ; camera_perspective[3] = aspect;
}

/* Timer seam: retain every argument so the recovered wrappers are executable. */
static MGTIMER timer_storage;
static int timer_create_calls;
static int timer_mode_calls;
static int timer_disp_off_calls;
static int timer_kill_calls;
static BOOL timer_done;
MGTIMER *MgTimerCreate(int type) {
    memset(&timer_storage, 0, sizeof(timer_storage));
    timer_storage.type = type;
    timer_create_calls += 1;
    return &timer_storage;
}
void MgTimerParamSet(MGTIMER *timer, int maxTime, int endTime, int recordTime) {
    timer->maxTime = maxTime; timer->endTime = endTime;
    timer->recordTime = recordTime;
}
void MgTimerPosSet(MGTIMER *timer, float x, float y) {
    timer->pos.x = x; timer->pos.y = y;
}
void MgTimerModeOnSet(MGTIMER *timer, int offType) {
    timer->mode = MGTIMER_MODE_ON; timer->offType = offType;
    timer_mode_calls += 1;
}
BOOL MgTimerDoneCheck(MGTIMER *timer) {
    (void)timer;
    return timer_done;
}
void MgTimerDispOff(MGTIMER *timer) {
    timer->dispOnF = FALSE; timer_disp_off_calls += 1;
}
void MgTimerKill(MGTIMER *timer) {
    assert(timer == &timer_storage);
    timer_kill_calls += 1;
}

/* Particle seam with guarded native allocations and a deliberately short live pool. */
#define PARTICLE_MODEL 4
#define PARTICLE_LIVE_COUNT 3
#define PARTICLE_MAX_COUNT 100
#define GUARD_BYTES 16
static ANIMDATA animation_storage;
static HU3D_PARTICLE particle_storage;
static _Alignas(16) unsigned char particle_raw[
    GUARD_BYTES + PARTICLE_LIVE_COUNT * sizeof(HU3D_PARTICLE_DATA) + GUARD_BYTES];
static _Alignas(16) unsigned char parameter_raw[
    GUARD_BYTES + PARTICLE_MAX_COUNT * sizeof(M662ParticleParameters) + GUARD_BYTES];
static int particle_hook_calls;
static int particle_layer_calls;
static int particle_camera_calls;
static int particle_blend_calls;
static int parameter_alloc_calls;
static u32 cache_store_bytes;
static void *cache_store_address;

static void fill_guard(unsigned char *raw, size_t bytes) {
    memset(raw, 0xA5, bytes);
}
static void assert_guard(const unsigned char *raw, size_t bytes) {
    size_t i;
    for (i = 0; i < GUARD_BYTES; ++i) {
        assert(raw[i] == 0xA5);
        assert(raw[bytes - GUARD_BYTES + i] == 0xA5);
    }
}
void *HuDataSelHeapReadNum(int dataNum, s32 num, HEAPID heap) {
    assert(dataNum == 37 && num == HU_MEMNUM_OVL && heap == HEAP_MODEL);
    return &animation_storage;
}
ANIMDATA *HuSprAnimRead(void *data) {
    assert(data == &animation_storage);
    return &animation_storage;
}
void *HuMemDirectMallocNum(HEAPID heap, s32 size, u32 num) {
    assert(heap == HEAP_HEAP && num == HU_MEMNUM_OVL);
    assert(size == PARTICLE_MAX_COUNT * (s32)sizeof(M662ParticleParameters));
    fill_guard(parameter_raw, sizeof(parameter_raw));
    parameter_alloc_calls += 1;
    return parameter_raw + GUARD_BYTES;
}
HU3D_MODELID Hu3DParticleCreate(ANIMDATA *animation, s16 maxCount) {
    assert(animation == &animation_storage && maxCount == PARTICLE_MAX_COUNT);
    memset(&particle_storage, 0, sizeof(particle_storage));
    fill_guard(particle_raw, sizeof(particle_raw));
    particle_storage.maxCnt = PARTICLE_LIVE_COUNT;
    particle_storage.data = (HU3D_PARTICLE_DATA *)(particle_raw + GUARD_BYTES);
    Hu3DData[PARTICLE_MODEL].hookData = &particle_storage;
    return PARTICLE_MODEL;
}
void Hu3DParticleHookSet(HU3D_MODELID modelId, HU3D_PARTICLE_HOOK hook) {
    assert(modelId == PARTICLE_MODEL && hook == fn_1_4470);
    particle_hook_calls += 1;
}
void Hu3DModelLayerSet(HU3D_MODELID modelId, s16 layerNo) {
    assert(modelId == PARTICLE_MODEL && layerNo == 0);
    particle_layer_calls += 1;
}
void Hu3DModelCameraSet(HU3D_MODELID modelId, u16 cameraBit) {
    assert(modelId == PARTICLE_MODEL && cameraBit == 1);
    particle_camera_calls += 1;
}
void Hu3DParticleBlendModeSet(HU3D_MODELID modelId, u8 blendMode) {
    assert(modelId == PARTICLE_MODEL && blendMode == 1);
    particle_blend_calls += 1;
}
void PSVECAdd(const Vec *a, const Vec *b, Vec *out) {
    out->x = a->x + b->x; out->y = a->y + b->y; out->z = a->z + b->z;
}
void DCStoreRangeNoSync(void *address, u32 bytes) {
    cache_store_address = address; cache_store_bytes = bytes;
}

/* Cross-TU ABI checks retain the corrected pointer/return contracts at link time. */
void fn_1_48E8(M662Player *player, s16 spatialMode) {
    (void)player; (void)spatialMode;
}
void fn_1_5008(s16 soundSlot, s16 spatialMode, HuVecF *position) {
    assert(soundSlot == 1 && spatialMode == 5 && position == NULL);
    path_sound_calls += 1;
}
void fn_1_4EFC(s16 streamSlot, s16 fadeSpeed) {
    (void)streamSlot; (void)fadeSpeed;
}
void fn_1_57A8(void) {}

/* M662_NATIVE_SUBJECT */
static void assert_zero(const void *memory, size_t size) {
    const unsigned char *bytes = (const unsigned char *)memory;
    size_t index;
    for (index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            fprintf(stderr, "nonzero reset byte size=%zu index=%zu value=%u\\n",
                    size, index, (unsigned)bytes[index]);
            abort();
        }
    }
}
static void assert_data_equal(const void *a, const void *b, size_t size) {
    assert(memcmp(a, b, size) == 0);
}

static void check_abi_types(void) {
    s32 (*p_2da0)(f32 *, f32, f32, f32) = fn_1_2DA0;
    MGTIMER *(*p_2ec0)(int, int, int, float, float) = fn_1_2EC0;
    void (*p_2f34)(MGTIMER *) = fn_1_2F34;
    s32 (*p_2f60)(MGTIMER *) = fn_1_2F60;
    void (*p_2f88)(MGTIMER *) = fn_1_2F88;
    void (*p_48e8)(M662Player *, s16) = fn_1_48E8;
    void (*p_5008)(s16, s16, HuVecF *) = fn_1_5008;
    void (*p_4efc)(s16, s16) = fn_1_4EFC;
    void (*p_57a8)(void) = fn_1_57A8;
    (void)p_2da0; (void)p_2ec0; (void)p_2f34; (void)p_2f60; (void)p_2f88;
    (void)p_48e8; (void)p_5008; (void)p_4efc; (void)p_57a8;
}

int main(void) {
    unsigned char data0_saved[sizeof(lbl_1_data_0)];
    unsigned char sine_saved[sizeof(lbl_1_data_150)];
    unsigned char linear_saved[sizeof(lbl_1_data_32C)];
    unsigned char asset_saved[sizeof(lbl_1_data_448)];
    unsigned char names_saved[sizeof(lbl_1_data_4E8)];
    M662ParticleParameters parameters;
    M662ParticleParameters *parameter_table;
    HU3D_PARTICLE_DATA *entry;
    Vec position, velocity;
    f32 scalar = 0.0f;
    Mtx matrix = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

    _Static_assert(sizeof(M662PadInput) == 20, "native pad ABI");
    _Static_assert(sizeof(M662Player) == 128, "native player ABI");
    _Static_assert(sizeof(M662Model) == 32, "native model ABI");
    _Static_assert(sizeof(M662Motion) == 96, "native motion ABI");
    _Static_assert(sizeof(M662Sequence) == 232, "native sequence ABI");
    _Static_assert(offsetof(M662Sequence, timerObject) == 104, "timer pointer ABI");
    _Static_assert(sizeof(M662Puzzle) == 88, "native puzzle ABI");
    _Static_assert(offsetof(M662Puzzle, objName) == 16, "object pointer ABI");
    _Static_assert(sizeof(M662ParticleParameters) == 60, "particle parameter ABI");
    _Static_assert(sizeof(HU3D_PARTICLE_DATA) == 72, "particle data ABI");
    check_abi_types();

    /* The generated reset owns every BSS object and snapshots each writable table once. */
    memset(&lbl_1_data_0, 0x11, sizeof(lbl_1_data_0));
    memset(&lbl_1_data_F6, 0x22, sizeof(lbl_1_data_F6));
    memset(&lbl_1_data_150, 0x33, sizeof(lbl_1_data_150));
    memset(&lbl_1_data_32C, 0x44, sizeof(lbl_1_data_32C));
    memset(&lbl_1_data_448, 0x55, sizeof(lbl_1_data_448));
    memset(&lbl_1_data_4E8, 0x66, sizeof(lbl_1_data_4E8));
    mp6_m662_reset();
    memcpy(data0_saved, &lbl_1_data_0, sizeof(data0_saved));
    memcpy(sine_saved, &lbl_1_data_150, sizeof(sine_saved));
    memcpy(linear_saved, &lbl_1_data_32C, sizeof(linear_saved));
    memcpy(asset_saved, &lbl_1_data_448, sizeof(asset_saved));
    memcpy(names_saved, &lbl_1_data_4E8, sizeof(names_saved));
    assert_zero(&lbl_1_bss_C60, sizeof(lbl_1_bss_C60));
    assert_zero(&lbl_1_bss_C58, sizeof(lbl_1_bss_C58));
    assert_zero(&lbl_1_bss_C50, sizeof(lbl_1_bss_C50));
    assert_zero(&lbl_1_bss_B08, sizeof(lbl_1_bss_B08));
    assert_zero(&lbl_1_bss_ACC, sizeof(lbl_1_bss_ACC));
    assert_zero(&lbl_1_bss_A7C, sizeof(lbl_1_bss_A7C));
    assert_zero(&lbl_1_bss_A74, sizeof(lbl_1_bss_A74));
    assert_zero(&lbl_1_bss_4B4, sizeof(lbl_1_bss_4B4));
    assert_zero(&lbl_1_bss_3F4, sizeof(lbl_1_bss_3F4));
    assert_zero(&lbl_1_bss_2F4, sizeof(lbl_1_bss_2F4));
    assert_zero(&lbl_1_bss_210, sizeof(lbl_1_bss_210));
    assert_zero(&lbl_1_bss_D0, sizeof(lbl_1_bss_D0));
    assert_zero(&lbl_1_bss_0, sizeof(lbl_1_bss_0));

    memset(&lbl_1_data_0, 0xA5, sizeof(lbl_1_data_0));
    memset(&lbl_1_data_F6, 0xA5, sizeof(lbl_1_data_F6));
    memset(&lbl_1_data_150, 0xA5, sizeof(lbl_1_data_150));
    memset(&lbl_1_data_32C, 0xA5, sizeof(lbl_1_data_32C));
    memset(&lbl_1_data_448, 0xA5, sizeof(lbl_1_data_448));
    memset(&lbl_1_data_4E8, 0xA5, sizeof(lbl_1_data_4E8));
    memset(&lbl_1_bss_C60, 0xA5, sizeof(lbl_1_bss_C60));
    memset(&lbl_1_bss_C58, 0xA5, sizeof(lbl_1_bss_C58));
    memset(&lbl_1_bss_C50, 0xA5, sizeof(lbl_1_bss_C50));
    memset(&lbl_1_bss_B08, 0xA5, sizeof(lbl_1_bss_B08));
    memset(&lbl_1_bss_ACC, 0xA5, sizeof(lbl_1_bss_ACC));
    memset(&lbl_1_bss_A7C, 0xA5, sizeof(lbl_1_bss_A7C));
    memset(&lbl_1_bss_A74, 0xA5, sizeof(lbl_1_bss_A74));
    memset(&lbl_1_bss_4B4, 0xA5, sizeof(lbl_1_bss_4B4));
    memset(&lbl_1_bss_3F4, 0xA5, sizeof(lbl_1_bss_3F4));
    memset(&lbl_1_bss_2F4, 0xA5, sizeof(lbl_1_bss_2F4));
    memset(&lbl_1_bss_210, 0xA5, sizeof(lbl_1_bss_210));
    memset(&lbl_1_bss_D0, 0xA5, sizeof(lbl_1_bss_D0));
    memset(&lbl_1_bss_0, 0xA5, sizeof(lbl_1_bss_0));
    mp6_m662_reset();
    assert_data_equal(&lbl_1_data_0, data0_saved, sizeof(data0_saved));
    assert_data_equal(&lbl_1_data_150, sine_saved, sizeof(sine_saved));
    assert_data_equal(&lbl_1_data_32C, linear_saved, sizeof(linear_saved));
    assert_data_equal(&lbl_1_data_448, asset_saved, sizeof(asset_saved));
    assert_data_equal(&lbl_1_data_4E8, names_saved, sizeof(names_saved));
    assert_zero(&lbl_1_bss_C50, sizeof(lbl_1_bss_C50));

    /* The real setup callback clears full native objects before delegating setup. */
    memset(&lbl_1_bss_B08, 0xA5, sizeof(lbl_1_bss_B08));
    memset(&lbl_1_bss_ACC, 0xA5, sizeof(lbl_1_bss_ACC));
    memset(&lbl_1_bss_A7C, 0xA5, sizeof(lbl_1_bss_A7C));
    memset(&lbl_1_bss_A74, 0xA5, sizeof(lbl_1_bss_A74));
    memset(&lbl_1_bss_4B4, 0xA5, sizeof(lbl_1_bss_4B4));
    memset(&lbl_1_bss_3F4, 0xA5, sizeof(lbl_1_bss_3F4));
    memset(&lbl_1_bss_2F4, 0xA5, sizeof(lbl_1_bss_2F4));
    memset(&lbl_1_bss_210, 0xA5, sizeof(lbl_1_bss_210));
    memset(&lbl_1_bss_D0, 0xA5, sizeof(lbl_1_bss_D0));
    memset(&lbl_1_bss_0, 0xA5, sizeof(lbl_1_bss_0));
    GwMgNightF = -7;
    fn_1_130(0, 0);
    assert(setup_camera_calls == 1 && setup_light_calls == 1);
    assert(setup_shadow_calls == 1 && setup_model_calls == 1);
    assert(setup_audio_calls == 1 && setup_next_calls == 1);
    assert(lbl_1_bss_210.nightMode == -7);
    assert_zero(&lbl_1_bss_B08.cameras[1], sizeof(lbl_1_bss_B08.cameras[1]) * 3 +
                sizeof(lbl_1_bss_B08.unknownTail));
    assert_zero(&lbl_1_bss_A74, sizeof(lbl_1_bss_A74));
    assert_zero(&lbl_1_bss_4B4, sizeof(lbl_1_bss_4B4));
    assert_zero(&lbl_1_bss_3F4, sizeof(lbl_1_bss_3F4));
    assert_zero(&lbl_1_bss_2F4, sizeof(lbl_1_bss_2F4));
    assert_zero(&lbl_1_bss_210, offsetof(M662Sequence, nightMode));
    assert_zero((unsigned char *)&lbl_1_bss_210 +
                    offsetof(M662Sequence, nightMode) + sizeof(lbl_1_bss_210.nightMode),
                sizeof(lbl_1_bss_210) - offsetof(M662Sequence, nightMode) -
                    sizeof(lbl_1_bss_210.nightMode));
    assert_zero(&lbl_1_bss_D0, sizeof(lbl_1_bss_D0));
    assert_zero(&lbl_1_bss_0, sizeof(lbl_1_bss_0));

    /* Valid camera presets flow through the shared camera-mask mapper. */
    memset(&lbl_1_bss_B08, 0, sizeof(lbl_1_bss_B08));
    fn_1_1918_real(2);
    assert(lbl_1_bss_B08.cameras[2].cameraIndex == 2);
    assert(fabsf(lbl_1_bss_B08.cameras[2].position.y - 208.0f) < .001f);
    assert(fabsf(lbl_1_bss_B08.cameras[2].fieldOfView - 45.0f) < .001f);
    assert(camera_create_mask == 4 && camera_mapper_calls == 1);
    assert(fabsf(camera_viewport[2] - 640.0f) < .001f &&
           fabsf(camera_viewport[3] - 480.0f) < .001f);
    assert(fabsf(camera_perspective[0] - 45.0f) < .001f &&
           fabsf(camera_perspective[1] - 20.0f) < .001f &&
           fabsf(camera_perspective[2] - 40000.0f) < .001f);

    /* Timer wrappers preserve native pointer identity and all engine calls. */
    MGTIMER *timer = fn_1_2EC0(2, 10, 3, 12.5f, 34.5f);
    assert(timer == &timer_storage && timer_create_calls == 1);
    assert(timer->type == 2 && timer->maxTime == 600 && timer->endTime == 180);
    assert(timer->recordTime == 0 && timer->pos.x == 12.5f && timer->pos.y == 34.5f);
    fn_1_2F34(timer);
    assert(timer->mode == MGTIMER_MODE_ON && timer_mode_calls == 1);
    timer_done = FALSE; assert(fn_1_2F60(timer) == FALSE);
    timer_done = TRUE; assert(fn_1_2F60(timer) == TRUE);
    fn_1_2F88(timer);
    assert(timer_disp_off_calls == 1 && timer_kill_calls == 1);

    /* Particle setup uses exact native record sizes and keeps allocator canaries intact. */
    memset(&lbl_1_bss_A74, 0, sizeof(lbl_1_bss_A74));
    fn_1_3CEC();
    assert(parameter_alloc_calls == 1 && particle_hook_calls == 1);
    assert(particle_layer_calls == 1 && particle_camera_calls == 1);
    assert(particle_blend_calls == 1);
    assert(lbl_1_bss_A74.maximumCount[0] == PARTICLE_MAX_COUNT);
    assert(lbl_1_bss_A74.modelId[0] == PARTICLE_MODEL);
    assert(lbl_1_bss_A74.allocation[0] == parameter_raw + GUARD_BYTES);
    parameter_table = (M662ParticleParameters *)lbl_1_bss_A74.allocation[0];
    entry = particle_storage.data;
    for (int i = 0; i < PARTICLE_LIVE_COUNT; ++i)
        assert(entry[i].time == 0 && entry[i].scale == 0.0f);
    assert_guard(parameter_raw, sizeof(parameter_raw));
    assert_guard(particle_raw, sizeof(particle_raw));

    memset(&parameters, 0, sizeof(parameters));
    parameters.initialColor = (GXColor){ 10, 20, 30, 40 };
    parameters.finalColor[0] = 50;
    parameters.finalColor[1] = 60;
    parameters.finalColor[2] = 70;
    parameters.velocityScale[0] = 2.0f;
    parameters.velocityScale[1] = 3.0f;
    parameters.velocityScale[2] = 4.0f;
    parameters.gravity = 1.0f;
    parameters.rotationStep = 0.5f;
    parameters.scaleStep = 0.25f;
    parameters.scaleStepMultiplier = 1.5f;
    parameters.alphaStep = 0.0f;
    parameters.colorBlendStep = 0.1f;
    position = (Vec){ 4.0f, 5.0f, 6.0f };
    velocity = (Vec){ 1.0f, 2.0f, 3.0f };
    assert(fn_1_4270(0, 7, position, velocity, 1.0f, 20, &parameters) == 0);
    assert(parameter_table[0].resetValues[0] == 0.0f &&
           parameter_table[0].resetValues[1] == 0.0f &&
           parameter_table[0].resetValues[2] == 0.0f);
    assert(entry[0].parManId == 7 && entry[0].time == 20);
    assert(entry[0].pos.x == 4.0f && entry[0].pos.y == 5.0f && entry[0].pos.z == 6.0f);
    for (int i = 0; i < PARTICLE_LIVE_COUNT; ++i) entry[i].time = 1;
    assert(fn_1_4270(0, 7, position, velocity, 1.0f, 20, &parameters) == -1);
    assert_guard(parameter_raw, sizeof(parameter_raw));

    entry[0].time = 2;
    entry[0].scale = 1.0f;
    entry[0].vel = (HuVecF){ 1.0f, 2.0f, 3.0f };
    entry[0].pos = (HuVecF){ 4.0f, 5.0f, 6.0f };
    entry[0].color = parameters.initialColor;
    cache_store_address = NULL; cache_store_bytes = 0;
    fn_1_4470(&hu3d_storage[PARTICLE_MODEL], &particle_storage, matrix);
    assert(entry[0].time == 1);
    assert(entry[0].vel.x == 2.0f && entry[0].vel.y == 7.0f && entry[0].vel.z == 12.0f);
    assert(entry[0].pos.x == 6.0f && entry[0].pos.y == 11.0f && entry[0].pos.z == 18.0f);
    assert(fabsf(entry[0].zRot - 0.5f) < .001f);
    assert(fabsf(entry[0].scale - 1.375f) < .001f);
    assert(cache_store_address == particle_storage.data);
    assert(cache_store_bytes == PARTICLE_LIVE_COUNT * sizeof(HU3D_PARTICLE_DATA));
    assert_guard(parameter_raw, sizeof(parameter_raw));
    assert_guard(particle_raw, sizeof(particle_raw));

    /* The real path-step driver must leave an inactive (-1) segment untouched. */
    memset(&lbl_1_data_150[0], 0, sizeof(lbl_1_data_150[0]));
    lbl_1_data_150[0].duration = 10.0f;
    lbl_1_data_150[0].centerX = 100.0f;
    lbl_1_data_150[0].centerZ = -20.0f;
    lbl_1_data_150[0].start = 0.0f;
    lbl_1_data_150[0].end = 90.0f;
    lbl_1_data_150[0].amplitude = 25.0f;
    memset(&lbl_1_bss_D0, 0, sizeof(lbl_1_bss_D0));
    memset(&lbl_1_bss_4B4, 0, sizeof(lbl_1_bss_4B4));
    lbl_1_bss_210.pathStep = 0;
    for (int i = 0; i < 4; ++i) {
        lbl_1_bss_D0[i].secondModel = (s16)(36 + i);
        lbl_1_bss_D0[i].paths[0].index = -1;
        lbl_1_bss_D0[i].paths[0].mode = 0;
        lbl_1_bss_D0[i].progress = 1234.5f;
    }
    path_sound_calls = 0;
    assert(fn_1_7AF8() == 0);
    assert(path_sound_calls == 0);
    assert(lbl_1_bss_210.pathStep == 1);
    for (int i = 0; i < 4; ++i) {
        assert(lbl_1_bss_D0[i].status == 0);
        assert(lbl_1_bss_D0[i].pathIndex == -1);
        assert(lbl_1_bss_D0[i].progress == 1234.5f);
    }

    /* A valid curved path keeps the original update and emits its audio cue. */
    memset(&lbl_1_bss_D0, 0, sizeof(lbl_1_bss_D0));
    memset(&lbl_1_bss_4B4, 0, sizeof(lbl_1_bss_4B4));
    lbl_1_bss_210.pathStep = 0;
    for (int i = 0; i < 4; ++i) {
        lbl_1_bss_D0[i].status = 1;
        lbl_1_bss_D0[i].secondModel = (s16)(36 + i);
        lbl_1_bss_D0[i].pathIndex = 0;
        lbl_1_bss_D0[i].pathMode = 0;
        lbl_1_bss_D0[i].progress = 0.0f;
    }
    lbl_1_bss_D0[0].status = 0;
    lbl_1_bss_D0[0].paths[0].index = 0;
    lbl_1_bss_D0[0].paths[0].mode = 0;
    path_sound_calls = 0;
    assert(fn_1_7AF8() == 0);
    assert(path_sound_calls == 1);
    assert(lbl_1_bss_210.pathStep == 0);
    assert(lbl_1_bss_D0[0].status == 1);
    assert(lbl_1_bss_D0[0].progress == 9.0f);
    assert(lbl_1_bss_4B4[36].position.x == 100.0f);
    assert(lbl_1_bss_4B4[36].position.z == 80.0f);

    /* Mixed inactive/active selection still skips the inactive data table entry. */
    memset(&lbl_1_bss_D0, 0, sizeof(lbl_1_bss_D0));
    memset(&lbl_1_bss_4B4, 0, sizeof(lbl_1_bss_4B4));
    lbl_1_bss_210.pathStep = 0;
    for (int i = 0; i < 4; ++i) {
        lbl_1_bss_D0[i].secondModel = (s16)(36 + i);
        lbl_1_bss_D0[i].pathIndex = -1;
        lbl_1_bss_D0[i].pathMode = 0;
        lbl_1_bss_D0[i].paths[0].index = -1;
        lbl_1_bss_D0[i].paths[0].mode = 0;
        lbl_1_bss_D0[i].progress = 4321.5f;
    }
    lbl_1_bss_D0[0].status = 0;
    lbl_1_bss_D0[0].paths[0].index = -1;
    lbl_1_bss_D0[0].paths[0].mode = 0;
    lbl_1_bss_D0[1].status = 0;
    lbl_1_bss_D0[1].paths[0].index = 0;
    lbl_1_bss_D0[1].paths[0].mode = 0;
    path_sound_calls = 0;
    assert(fn_1_7AF8() == 0);
    assert(path_sound_calls == 1);
    assert(lbl_1_bss_210.pathStep == 0);
    assert(lbl_1_bss_D0[0].progress == 4321.5f);
    assert(lbl_1_bss_D0[1].progress == 9.0f);
    return 0;
}
