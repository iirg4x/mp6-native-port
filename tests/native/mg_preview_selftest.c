#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef int s32;
typedef unsigned int u32;
typedef float f32;
typedef int BOOL;
typedef f32 Mtx[3][4];
typedef struct {
    f32 x, y, z;
} Point3d;
typedef struct HU3D_MODEL_s {
    int unused;
} HU3D_MODEL;
typedef int HU3D_MODELID;
typedef int HUSPRID;
typedef int HUSPR_GROUPID;
typedef struct ANIMDATA_s {
    int tag;
} ANIMDATA;
typedef struct {
    int instPic[2][1];
    char *movie[2][1];
} MGDATA;

#define HUSPR_NONE (-1)
#define HU_MEMNUM_OVL 3
#define HEAP_MODEL 4
#define GX_TF_RGB565 5
#define GX_VA_POS 0
#define GX_VA_TEX0 1
#define GX_DIRECT 2
#define GX_VTXFMT0 0
#define GX_POS_XYZ 3
#define GX_F32 4
#define GX_TEX_ST 5
#define GX_CULL_NONE 6
#define GX_TRUE 1
#define GX_FALSE 0
#define GX_LEQUAL 7
#define GX_BM_BLEND 8
#define GX_BL_SRCALPHA 9
#define GX_BL_INVSRCALPHA 10
#define GX_LO_NOOP 11
#define GX_COLOR0A0 12
#define GX_COLOR1A1 13
#define GX_SRC_REG 14
#define GX_DF_CLAMP 15
#define GX_AF_SPOT 16
#define GX_TG_MTX2x4 17
#define GX_TG_TEX0 18
#define GX_IDENTITY 19
#define GX_PTIDENTITY 20
#define GX_TEVSTAGE0 21
#define GX_TEXMAP0 22
#define GX_TEXMAP1 23
#define GX_COLOR0 24
#define GX_CC_ZERO 25
#define GX_CC_TEXC 26
#define GX_TEV_ADD 27
#define GX_TB_ZERO 28
#define GX_CS_SCALE_1 29
#define GX_TEVPREV 30
#define GX_CA_ZERO 31
#define GX_CA_TEXA 32
#define GX_TEXCOORD0 33
#define GX_QUADS 34
#define GX_CLAMP 35

#define DATA_mgmbattle 0
#define DATA_mgmbingo 1
#define DATANUM(data, number) (number)

static MGDATA MgDataTbl[102];
static ANIMDATA frame_animation = {1};
static ANIMDATA still_animation = {2};
static ANIMDATA loaded_animations[512];
static ANIMDATA *roster_storage[260];
static unsigned char framebuffer[284 * 256 * 2];
static int asset_reads;
static int roster_allocations;
static int roster_frees;
static int animation_kills;
static int fail_roster_allocation;
static int movie_result;
static int movie_create_calls;
static int member_set_calls;
static int draw_no_calls;
static int pos_set_calls;
static int member_kill_calls;
static int thp_closes;
static int random_calls;
static int random_result = 1;
static int thp_started;
static int live_width;
static int thp_checks;
static int framebuffer_copy_calls;
static int framebuffer_copy_x;
static int framebuffer_copy_y;
static int framebuffer_copy_width;
static int framebuffer_copy_height;
static int framebuffer_copy_format;
static int framebuffer_copy_clear;
static void *framebuffer_copy_destination;
static int native_texture_loads;
static int still_texture_loads;
static int overlay_texture_loads;
static int primitive_open;
static int begin_calls;
static int end_calls;
static int positions;
static int texcoords;
static int positions_in_primitive;
static int texcoords_in_primitive;
static int state_while_open;

static void state_call(void)
{
    if (primitive_open) {
        ++state_while_open;
    }
}

static void GXBegin(int primitive, int format, int vertex_count)
{
    assert(!primitive_open);
    assert(primitive == GX_QUADS && format == GX_VTXFMT0 && vertex_count == 4);
    primitive_open = 1;
    positions_in_primitive = 0;
    texcoords_in_primitive = 0;
    ++begin_calls;
}

static void GXPosition3f32(f32 x, f32 y, f32 z)
{
    (void)x;
    (void)y;
    (void)z;
    assert(primitive_open);
    ++positions;
    ++positions_in_primitive;
}

static void GXTexCoord2f32(f32 s, f32 t)
{
    (void)s;
    (void)t;
    assert(primitive_open);
    ++texcoords;
    ++texcoords_in_primitive;
}

static void GXEnd(void)
{
    assert(primitive_open);
    assert(positions_in_primitive == 4 && texcoords_in_primitive == 4);
    primitive_open = 0;
    ++end_calls;
}

#define GXInvalidateTexAll() state_call()
#define GXClearVtxDesc() state_call()
#define GXSetVtxDesc(...) state_call()
#define GXSetVtxAttrFmt(...) state_call()
#define GXSetCullMode(...) state_call()
#define GXSetZMode(...) state_call()
#define GXSetBlendMode(...) state_call()
#define GXSetChanCtrl(...) state_call()
#define GXSetNumChans(...) state_call()
#define GXLoadPosMtxImm(...) state_call()
#define GXSetTexCoordGen2(...) state_call()
#define GXSetTevOrder(...) state_call()
#define GXSetTevColorIn(...) state_call()
#define GXSetTevColorOp(...) state_call()
#define GXSetTevAlphaIn(...) state_call()
#define GXSetTevAlphaOp(...) state_call()
#define GXSetNumTexGens(...) state_call()
#define GXSetNumTevStages(...) state_call()

static HU3D_MODELID Hu3DHookFuncCreate(void (*hook)(HU3D_MODEL *, Mtx *));
static void Hu3DCameraLayerHookSet(s16 camera, s16 layer, void (*hook)(s16));
static void Hu3DModelCameraSet(HU3D_MODELID model, s16 camera);
static void Hu3DModelLayerSet(HU3D_MODELID model, s16 layer);
static void Hu3DModelPosSetV(HU3D_MODELID model, const Point3d *position);
static void Hu3DModelScaleSet(HU3D_MODELID model, f32 x, f32 y, f32 z);
static void *HuDataSelHeapReadNum(int data, s16 overlay, s16 heap);
static void *HuDataReadNumHeapShortForce(int data, s16 overlay, s16 heap);
static ANIMDATA *HuSprAnimRead(void *data);
static HUSPR_GROUPID HuSprGrpCreate(s16 member_count);
static void HuSprGrpMemberSet(HUSPR_GROUPID group, s16 member, HUSPRID sprite);
static void HuSprGrpDrawNoSet(HUSPR_GROUPID group, s32 draw_no);
static void HuSprPosSet(HUSPR_GROUPID group, s16 member, f32 x, f32 y);
static void HuSprGrpMemberKill(HUSPR_GROUPID group, s16 member);
static HUSPRID HuTHPSprCreateVol(char *path, s16 loop, s16 priority, f32 volume);
static void HuTHPClose(void);
static int HuTHPProcCheck(void);
static void HuPrcVSleep(void);
static int frandmod(int limit);
static void *HuMemDirectMallocNum(s16 alignment, int size, s16 heap);
static void HuMemDirectFree(void *pointer);
static void HuSprAnimKill(ANIMDATA *animation);
static int GXGetTexBufferSize(s16 width, s16 height, int format, s16 mipmap, s16 aa);
static int HuTHPStartCheck(void);
static void Hu3DFbCopyExec(s16 x, s16 y, s16 width, s16 height, int format,
                           s16 clear, void *destination);
static void Hu3DTexLoad(void *texture, s16 width, s16 height, int format,
                        int wrap_s, int wrap_t, int mipmap, int map);
static void HuSprTexLoad(ANIMDATA *animation, s16 bmp_no, s16 map, int wrap_s,
                         int wrap_t, int filter);

static HU3D_MODELID Hu3DHookFuncCreate(void (*hook)(HU3D_MODEL *, Mtx *))
{
    (void)hook;
    return 9;
}
static void Hu3DCameraLayerHookSet(s16 camera, s16 layer, void (*hook)(s16))
{
    (void)camera;
    (void)layer;
    (void)hook;
}
static void Hu3DModelCameraSet(HU3D_MODELID model, s16 camera)
{
    (void)model;
    (void)camera;
}
static void Hu3DModelLayerSet(HU3D_MODELID model, s16 layer)
{
    (void)model;
    (void)layer;
}
static void Hu3DModelPosSetV(HU3D_MODELID model, const Point3d *position)
{
    (void)model;
    assert(position != NULL);
}
static void Hu3DModelScaleSet(HU3D_MODELID model, f32 x, f32 y, f32 z)
{
    (void)model;
    (void)x;
    (void)y;
    (void)z;
}
static void *HuDataSelHeapReadNum(int data, s16 overlay, s16 heap)
{
    (void)data;
    assert(overlay == HU_MEMNUM_OVL && heap == HEAP_MODEL);
    return &frame_animation;
}
static void *HuDataReadNumHeapShortForce(int data, s16 overlay, s16 heap)
{
    (void)data;
    assert(overlay == HU_MEMNUM_OVL && heap == HEAP_MODEL);
    assert(asset_reads < (int)(sizeof(loaded_animations) / sizeof(loaded_animations[0])));
    return &loaded_animations[asset_reads++];
}
static ANIMDATA *HuSprAnimRead(void *data)
{
    return (ANIMDATA *)data;
}
static HUSPR_GROUPID HuSprGrpCreate(s16 member_count)
{
    assert(member_count == 1);
    return 12;
}
static void HuSprGrpMemberSet(HUSPR_GROUPID group, s16 member, HUSPRID sprite)
{
    assert(group == 12 && member == 0 && sprite != HUSPR_NONE);
    ++member_set_calls;
}
static void HuSprGrpDrawNoSet(HUSPR_GROUPID group, s32 draw_no)
{
    assert(group == 12 && draw_no == 127);
    ++draw_no_calls;
}
static void HuSprPosSet(HUSPR_GROUPID group, s16 member, f32 x, f32 y)
{
    assert(group == 12 && member == 0 && x == 128.0f && y == 128.0f);
    ++pos_set_calls;
}
static void HuSprGrpMemberKill(HUSPR_GROUPID group, s16 member)
{
    assert(group == 12 && member == 0);
    ++member_kill_calls;
}
static HUSPRID HuTHPSprCreateVol(char *path, s16 loop, s16 priority, f32 volume)
{
    (void)path;
    assert(loop == 1 && priority == 100 && volume == 0.0f);
    ++movie_create_calls;
    return movie_result;
}
static void HuTHPClose(void)
{
    ++thp_closes;
}
static int HuTHPProcCheck(void)
{
    return 0;
}
static void HuPrcVSleep(void) {}
static int frandmod(int limit)
{
    assert(limit == 2);
    ++random_calls;
    return random_result;
}
static void *HuMemDirectMallocNum(s16 alignment, int size, s16 heap)
{
    assert(alignment == 0 && heap == HU_MEMNUM_OVL);
    if (size == (int)sizeof(framebuffer)) {
        return framebuffer;
    }
    ++roster_allocations;
    return fail_roster_allocation ? NULL : roster_storage;
}
static void HuMemDirectFree(void *pointer)
{
    assert(pointer == roster_storage);
    ++roster_frees;
}
static void HuSprAnimKill(ANIMDATA *animation)
{
    assert(animation >= loaded_animations &&
           animation < loaded_animations + (int)(sizeof(loaded_animations) / sizeof(loaded_animations[0])));
    ++animation_kills;
}
static int GXGetTexBufferSize(s16 width, s16 height, int format, s16 mipmap, s16 aa)
{
    assert(width == 284 && height == 256 && format == GX_TF_RGB565 && mipmap == 0 && aa == 0);
    return sizeof(framebuffer);
}
static int HuTHPStartCheck(void)
{
    ++thp_checks;
    return thp_started;
}
static int mp6_widescreen_render_width(void)
{
    return live_width;
}
static void Hu3DFbCopyExec(s16 x, s16 y, s16 width, s16 height, int format,
                           s16 clear, void *destination)
{
    ++framebuffer_copy_calls;
    framebuffer_copy_x = x;
    framebuffer_copy_y = y;
    framebuffer_copy_width = width;
    framebuffer_copy_height = height;
    framebuffer_copy_format = format;
    framebuffer_copy_clear = clear;
    framebuffer_copy_destination = destination;
}
static void Hu3DTexLoad(void *texture, s16 width, s16 height, int format,
                        int wrap_s, int wrap_t, int mipmap, int map)
{
    assert(texture == framebuffer && width == 284 && height == 256 && format == 4);
    assert(wrap_s == GX_CLAMP && wrap_t == GX_CLAMP && mipmap == GX_TRUE && map == GX_TEXMAP0);
    ++native_texture_loads;
    state_call();
}
static void HuSprTexLoad(ANIMDATA *animation, s16 bmp_no, s16 map, int wrap_s,
                         int wrap_t, int filter)
{
    (void)animation;
    (void)bmp_no;
    (void)wrap_s;
    (void)wrap_t;
    (void)filter;
    if (map == 0) {
        ++still_texture_loads;
    } else {
        ++overlay_texture_loads;
    }
    state_call();
}

HU3D_MODELID battle_lbl_1_bss_A4;
HUSPR_GROUPID battle_lbl_1_bss_A2;
s16 battle_lbl_1_bss_A0;
ANIMDATA *battle_lbl_1_bss_9C;
ANIMDATA *battle_lbl_1_bss_98;
void *battle_lbl_1_bss_94;
MGDATA *battle_lbl_1_bss_90;
ANIMDATA **battle_lbl_1_bss_8C;
s16 battle_lbl_1_bss_88;
ANIMDATA *battle_lbl_1_bss_84;
u8 battle_lbl_1_bss_2[128];
s16 battle_lbl_1_bss_0;

HU3D_MODELID bingo_lbl_1_bss_A4;
HUSPR_GROUPID bingo_lbl_1_bss_A2;
s16 bingo_lbl_1_bss_A0;
ANIMDATA *bingo_lbl_1_bss_9C;
ANIMDATA *bingo_lbl_1_bss_98;
void *bingo_lbl_1_bss_94;
MGDATA *bingo_lbl_1_bss_90;
ANIMDATA **bingo_lbl_1_bss_8C;
s16 bingo_lbl_1_bss_88;
ANIMDATA *bingo_lbl_1_bss_84;
u8 bingo_lbl_1_bss_2[130];
s16 bingo_lbl_1_bss_0;

/* The Python test inserts the two namespaced sets of actual patched bodies. */
/* MG_PREVIEW_NATIVE_SUBJECT */

typedef HU3D_MODELID (*PreviewCreate)(s16, s16, Point3d *);
typedef void (*PreviewRoster)(s16, u8 *);
typedef void (*PreviewSimple)(void);
typedef void (*PreviewCapture)(s16);
typedef void (*PreviewDraw)(HU3D_MODEL *, Mtx *);

typedef struct {
    PreviewCreate create;
    PreviewRoster prepare;
    PreviewSimple advance;
    PreviewSimple cleanup;
    PreviewSimple start_movie;
    PreviewCapture capture;
    PreviewDraw draw;
    HU3D_MODELID *model;
    HUSPR_GROUPID *group;
    s16 *time;
    ANIMDATA **overlay;
    ANIMDATA **current;
    ANIMDATA **saved;
    void **texture;
    MGDATA **data;
    ANIMDATA ***array;
    s16 *index;
    s16 *count;
    u8 *order_storage;
    size_t order_size;
} PreviewApi;

static void reset_runtime_counters(void)
{
    asset_reads = 0;
    roster_allocations = 0;
    roster_frees = 0;
    animation_kills = 0;
    fail_roster_allocation = 0;
    random_calls = 0;
    thp_closes = 0;
    member_set_calls = 0;
    draw_no_calls = 0;
    pos_set_calls = 0;
    member_kill_calls = 0;
    movie_create_calls = 0;
}

static void reset_draw_counters(void)
{
    primitive_open = 0;
    begin_calls = 0;
    end_calls = 0;
    positions = 0;
    texcoords = 0;
    positions_in_primitive = 0;
    texcoords_in_primitive = 0;
    state_while_open = 0;
    thp_checks = 0;
    native_texture_loads = 0;
    still_texture_loads = 0;
    overlay_texture_loads = 0;
}

static void test_preview(const PreviewApi *api)
{
    Point3d position = {0.0f, 0.0f, 0.0f};
    Mtx matrix = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    u8 order[130] = {0};
    ANIMDATA original_still = {77};
    HU3D_MODEL model = {0};
    int widths[] = {640, 848, 1120, 2560, 640};
    int expected_capacity = (int)api->order_size - 1;

    memset(api->order_storage, 0, api->order_size);
    *api->current = &original_still;
    reset_runtime_counters();
    api->prepare(0, order);
    api->advance();
    assert(roster_allocations == 0 && random_calls == 0 && thp_closes == 0);
    assert(*api->count == 0 && *api->array == NULL);
    assert(api->order_storage[0] == 255 && *api->current == &original_still);
    api->prepare(-1, order);
    api->prepare(5, NULL);
    assert(roster_allocations == 0);

    asset_reads = 0;
    *api->current = &original_still;
    api->prepare((s16)api->order_size, order);
    assert(*api->count == expected_capacity);
    assert(api->order_storage[expected_capacity] == 255);
    assert(asset_reads == expected_capacity * 2 && roster_allocations == 1);
    for (int player = 0; player < expected_capacity; ++player) {
        api->advance();
        assert(*api->current == &loaded_animations[player * 2 + 1]);
    }
    assert(*api->index == 0 && random_calls == expected_capacity);
    api->cleanup();
    assert(animation_kills == expected_capacity * 2 && roster_frees == 1);
    assert(*api->current == &original_still && *api->array == NULL);
    api->cleanup();
    assert(animation_kills == expected_capacity * 2 && roster_frees == 1);

    fail_roster_allocation = 1;
    asset_reads = 0;
    *api->current = &original_still;
    api->prepare(5, order);
    api->advance();
    assert(*api->count == 0 && *api->array == NULL);
    assert(asset_reads == 0 && random_calls == expected_capacity);
    assert(*api->current == &original_still);
    fail_roster_allocation = 0;

    movie_result = HUSPR_NONE;
    reset_runtime_counters();
    api->create(0, 0, &position);
    assert(movie_create_calls == 1 && member_set_calls == 0);
    api->start_movie();
    assert(movie_create_calls == 2 && member_set_calls == 0);
    movie_result = 17;
    api->create(0, 0, &position);
    assert(member_set_calls == 1 && draw_no_calls == 1 && pos_set_calls == 1);
    movie_result = 23;
    api->start_movie();
    assert(member_set_calls == 2 && draw_no_calls == 2 && pos_set_calls == 2);
    movie_result = HUSPR_NONE;
    api->start_movie();
    assert(member_set_calls == 2 && draw_no_calls == 2 && pos_set_calls == 2);

    *api->texture = framebuffer;
    framebuffer_copy_calls = 0;
    thp_started = 0;
    for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        live_width = widths[i];
        api->capture(0);
    }
    assert(framebuffer_copy_calls == 0);
    thp_started = 1;
    for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        live_width = widths[i];
        api->capture(0);
        assert(framebuffer_copy_x == (widths[i] - 640) / 2);
        assert(framebuffer_copy_y == 0);
        assert(framebuffer_copy_width == 284 && framebuffer_copy_height == 256);
        assert(framebuffer_copy_format == GX_TF_RGB565 && framebuffer_copy_clear == 0);
        assert(framebuffer_copy_destination == framebuffer);
    }
    assert(framebuffer_copy_calls == (int)(sizeof(widths) / sizeof(widths[0])));

    for (int active = 0; active <= 1; ++active) {
        reset_draw_counters();
        thp_started = active;
        api->draw(&model, &matrix);
        assert(!primitive_open);
        assert(begin_calls == 2 && end_calls == 2);
        assert(positions == 8 && texcoords == 8);
        assert(state_while_open == 0);
        assert(thp_checks == 1);
        assert(native_texture_loads == active);
        assert(still_texture_loads == !active);
        assert(overlay_texture_loads == 1);
    }
}

int main(void)
{
    PreviewApi battle = {
        battle_fn_1_0,
        battle_fn_1_2A4,
        battle_fn_1_43C,
        battle_fn_1_4F8,
        battle_fn_1_6F4,
        battle_fn_1_854,
        battle_fn_1_898,
        &battle_lbl_1_bss_A4,
        &battle_lbl_1_bss_A2,
        &battle_lbl_1_bss_A0,
        &battle_lbl_1_bss_9C,
        &battle_lbl_1_bss_98,
        &battle_lbl_1_bss_84,
        &battle_lbl_1_bss_94,
        &battle_lbl_1_bss_90,
        &battle_lbl_1_bss_8C,
        &battle_lbl_1_bss_88,
        &battle_lbl_1_bss_0,
        battle_lbl_1_bss_2,
        sizeof(battle_lbl_1_bss_2),
    };
    PreviewApi bingo = {
        bingo_fn_1_0,
        bingo_fn_1_2A4,
        bingo_fn_1_43C,
        bingo_fn_1_4F8,
        bingo_fn_1_6F4,
        bingo_fn_1_854,
        bingo_fn_1_898,
        &bingo_lbl_1_bss_A4,
        &bingo_lbl_1_bss_A2,
        &bingo_lbl_1_bss_A0,
        &bingo_lbl_1_bss_9C,
        &bingo_lbl_1_bss_98,
        &bingo_lbl_1_bss_84,
        &bingo_lbl_1_bss_94,
        &bingo_lbl_1_bss_90,
        &bingo_lbl_1_bss_8C,
        &bingo_lbl_1_bss_88,
        &bingo_lbl_1_bss_0,
        bingo_lbl_1_bss_2,
        sizeof(bingo_lbl_1_bss_2),
    };
    live_width = 640;
    test_preview(&battle);
    test_preview(&bingo);
    return 0;
}
