#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "REL/m646Dll/module_types.h"
#include "game/disp.h"
#include "game/init.h"
#include "mp6_widescreen.h"
#include "m646_types.inc"

HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
u16 Hu3DCameraExistF;
static GXRenderModeObj mode;
GXRenderModeObj *RenderMode = &mode;
float CZoomM[HU3D_CAM_MAX];
Vec CenterM[HU3D_CAM_MAX], CRotM[HU3D_CAM_MAX];
static float scale = 1;
float mp6_widescreen_scale_factor(void) { return scale; }
GW_PLAYER_CONF GwPlayerConf[GW_PLAYER_MAX];
OMOBJ *lbl_1_bss_30[4][3], *lbl_1_bss_70[4];
OMOBJ *lbl_1_bss_60, *lbl_1_bss_68, *lbl_1_bss_80, *lbl_1_bss_B8, *lbl_1_bss_C0;
s16 lbl_1_bss_28[4];
M646MainData lbl_1_bss_0, *lbl_1_data_0 = &lbl_1_bss_0;
M646MovingStageDescriptor lbl_1_data_1AB4[7];
M646AIProfile lbl_1_data_1DC8[4];
float lbl_1_data_108[5] = {100,75,55,35,100};
s32 lbl_1_data_4[2] = {10,11};
static OMOBJ objects[64];
static int objectCount, allocations;
static struct { unsigned char *raw; size_t size; } memory[64];
OMOBJ *omAddObjEx(OMOBJMAN *owner, s16 priority, u16 models, u16 motions,
                 s16 group, OMOBJ_FUNC callback) {
    assert(objectCount < 64);
    OMOBJ *object = &objects[objectCount++];
    object->objFunc = callback;
    return object;
}
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 tag) {
    assert(heap == HEAP_HEAP && tag == HU_MEMNUM_OVL && bytes > 0 && allocations < 64);
    unsigned char *p = malloc(bytes + 32); assert(p);
    memset(p, 0xA5, bytes + 32);
    memory[allocations].raw = p; memory[allocations++].size = bytes;
    return p + 16;
}
static void check_guards(void) {
    for (int a = 0; a < allocations; ++a)
        for (int i = 0; i < 16; ++i) {
            assert(memory[a].raw[i] == 0xA5);
            assert(memory[a].raw[memory[a].size + 16 + i] == 0xA5);
        }
}
#define UNUSED_CALLBACK(name) void name(OMOBJ *object) { __builtin_trap(); }
UNUSED_CALLBACK(fn_1_1630)
UNUSED_CALLBACK(fn_1_6E20)
UNUSED_CALLBACK(fn_1_2BDC)
UNUSED_CALLBACK(fn_1_52B4)
UNUSED_CALLBACK(fn_1_8ACC)
UNUSED_CALLBACK(fn_1_93E0)
UNUSED_CALLBACK(fn_1_9978)
UNUSED_CALLBACK(fn_1_310)
float PSVECMag(const Vec *v) { return sqrtf(v->x*v->x + v->y*v->y + v->z*v->z); }
void C_VECReflect(const Vec *v, const Vec *n, Vec *out) { __builtin_trap(); }
u32 frand(void) { return 3; }
void *HuDataSelHeapReadNum(s32 data, s32 tag, HEAPID heap) { return &lbl_1_data_4[data-10]; }
HU3D_MOTIONID Hu3DMotionCreate(void *data) { return data == &lbl_1_data_4[0] ? 10 : 11; }
static int detached, positioned, animated;
void Hu3DModelHookObjReset(HU3D_MODELID model, char *name) { ++detached; }
void Hu3DModelPosSetV(HU3D_MODELID model, HuVecF *pos) { ++positioned; }
void Hu3DMotionSpeedSet(HU3D_MODELID model, float speed) { assert(speed == 1); ++animated; }
void Hu3DModelLayerSet(HU3D_MODELID model, s16 layer) { assert(layer == 7); }
void Hu3DModelAttrSet(HU3D_MODELID model, u32 attr) { assert(attr == HU3D_ATTR_ZCMP_OFF); }
void fn_1_1290(OMOBJ *obj) { }
void fn_1_1570(OMOBJ *obj, s32 active) { assert(active == 1); }
void fn_1_15A4(OMOBJ *obj, s32 sound) { }
f32 fn_1_B2BC(M646PlayerTicket *ticket);
#include "m646_subject.inc"

static int contactCalls;
static s32 surface_contact(void *surface, void *body) {
    M646SurfaceCollider *s = surface;
    assert(s->contactEdge == s->current);
    assert(((M646BodyAnchor *)body)->position);
    ++contactCalls;
    return 1;
}

int main(void) {
    /* The real constructors must allocate the same native records all consumers use. */
    for (int p = 0; p < 4; ++p) {
        GwPlayerConf[p].type = p != 0; GwPlayerConf[p].comDif = p % 4;
        fn_1_AFC(NULL, p);
    }
    fn_1_543C(NULL); fn_1_2B50(NULL); fn_1_8E78(NULL); fn_1_98E0(NULL); fn_1_4E28(NULL);
    assert(allocations == 21);
    for (int i = 0; i < 12; ++i) assert(memory[i].size == sizeof(M646ActorState));
    for (int i = 12; i < 16; ++i) assert(memory[i].size == sizeof(M646PlayerFullWork));
    assert(memory[16].size == 160 * sizeof(void *));
    assert(memory[17].size == sizeof(M646ScoreboardWork));
    assert(memory[18].size == sizeof(M646CoordinatorWork));
    assert(memory[19].size == sizeof(M646StageWork));
    assert(memory[20].size == sizeof(M646MovingStageWork));
    for (int p = 0; p < 4; ++p) {
        M646PlayerFullWork *w = lbl_1_bss_70[p]->data;
        assert(w->aiming.motion.playerIndex == p && w->comDifficulty == (p ? p : -1));
    }
    /* Fill every pointer slot, including the final attachment slot; no PPC stride. */
    M646BodyAnchor bodies[31] = {0};
    M646SurfaceCollider surfaces[131] = {0};
    for (int i = 0; i < 30; ++i) assert(fn_1_3E80(&bodies[i]) == i);
    for (int i = 0; i < 130; ++i) assert(fn_1_3EF8(&surfaces[i]) == 30 + i);
    assert(fn_1_3E80(&bodies[30]) == -1 && fn_1_3EF8(&surfaces[130]) == -1);
    assert(fn_1_3FD4(159) == &surfaces[129]);
    fn_1_3F70(159); assert(fn_1_3FD4(159) == NULL);
    assert(offsetof(M646BodyAnchor, owner) == offsetof(M646CollisionHeader, owner));
    assert(offsetof(M646ModelCollider, contact) == offsetof(M646CollisionHeader, contact));
    /* Native surface contact retains pointers and correct post-header edge fields. */
    M646WorldEdge edge = {.first={0,0,-5}, .second={0,0,5}, .normal={1,0,0}};
    Vec position = {-1,0,0}, direction = {-1,0,0};
    M646BodyAnchor body = {.position=&position, .direction=&direction, .savedPosition={1,0,0}, .radius=1};
    M646SurfaceCollider surface = {.edgeCount=1, .current=&edge, .contact=surface_contact};
    assert(fn_1_3350(&body, &surface) == 1 && contactCalls == 1 && position.x > 0);
    /* Stage owner is an OMOBJ, never an attachment: its previous-index field may be nonzero. */
    OMOBJ *projectile = lbl_1_bss_30[0][0];
    M646ActorState *shot = projectile->data;
    HU3D_MODELID projectileModel = 1; projectile->mdlId = &projectileModel;
    shot->linkedObject = lbl_1_bss_70[0];
    shot->anchor.owner = projectile;
    lbl_1_bss_80->prev = 37;
    surface.owner = lbl_1_bss_80; surface.callbackMode = 10; surface.callbackType = 12;
    assert(fn_1_1F80(&shot->anchor, &surface) == 1 && shot->callbackType12Seen == 1);
    assert(shot->activeAttachmentContact == 0 && lbl_1_bss_80->prev == 37);
    /* Model collision writes a full contact-edge pointer without touching category or model. */
    M646ModelAttachment target = {.model=7, .parentModel=8};
    target.collider = (M646ModelCollider){.kind=1, .owner=&target, .callbackType=3,
        .current=&edge, .contact=fn_1_8DAC, .category=2, .model=9};
    position = (Vec){0,0,2}; body.savedPosition = (Vec){1,0,0}; body.radius = 1;
    fn_1_3894(&body, &target.collider);
    assert(target.collider.contactEdge == &edge && target.collider.category == 2 && target.collider.model == 9);
    assert(target.state == 1 && target.parentModel == -1 && detached == 1 && animated == 1 && positioned == 1);
    fn_1_8F04(0, 10); fn_1_8F04(0, 10000); assert(fn_1_8FA4(0) == 9999);
    fn_1_8F5C(0); assert(fn_1_8FA4(0) == 0);
    /* Stress all five visible layouts. Capacity must be checked before any candidate write. */
    M646MovingStageWork *moving = lbl_1_bss_80->data;
    M646CoordinatorWork *work = lbl_1_bss_C0->data;
    for (int s = 1; s < 6; ++s)
        for (int c = 0; c < 5; ++c) {
            moving->stages[s].jointIndex[c] = 12;
            for (int j = 0; j < 12; ++j) moving->stages[s].attachments[c][j].collider.pose.pos.y = 300;
        }
    fn_1_A124(); fn_1_A158();
    assert(work->candidateCount == 17 && work->categoryCount[0] == 12 && work->categoryCount[1] == 5);
    for (int c = 0; c < 5; ++c) assert(work->categoryCount[c] <= 12);
    /* All candidates outside the initial distance threshold must not accumulate into the next filter. */
    M646PlayerFullWork *player = lbl_1_bss_70[1]->data;
    M646PlayerTicket ticket = {lbl_1_bss_70[1], 1};
    for (int i = 0; i < work->candidateCount; ++i) work->candidates[i].attachment->collider.pose.pos.x = 20000;
    assert(fn_1_A8E0(&ticket) == NULL && fn_1_ABB0(&ticket) == NULL && fn_1_B07C(&ticket) == NULL);
    for (int i = 0; i < work->candidateCount; ++i) work->candidates[i].attachment->collider.pose.pos.x = 0;
    player->targetDelayTicks = 0; assert(fn_1_A8E0(&ticket));
    player->targetDelayTicks = 0; assert(fn_1_ABB0(&ticket));
    assert(fn_1_B07C(&ticket));
    for (int r = 0; r < 3; ++r) {
        mode.fbWidth = (int[]){640,848,1120}[r]; mode.efbHeight = mode.xfbHeight = 480;
        scale = (float)mode.fbWidth / 640;
        fn_1_E0(NULL);
        assert(Hu3DCamera[0].viewportW == mode.fbWidth && Hu3DCamera[0].viewportH == 480);
        assert(fabsf(Hu3DCamera[0].aspect - 1.2f * scale) < .0001f);
        assert(Hu3DCamera[0].near == 20 && Hu3DCamera[0].far == 10000);
        Hu3DCameraAllKill();
    }
    check_guards();
    for (int a = 0; a < allocations; ++a) free(memory[a].raw);
    return 0;
}
