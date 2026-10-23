#include <assert.h>
#include <math.h>
#include <string.h>

#include "game/hu3d.h"
#include "dolphin/mtx.h"
#include "mp6_ambient_occlusion.h"

/* The extracted callbacks are tested against the real GX/AO call sequence,
 * while these tiny shims keep the fixture independent of the renderer. */
static int test_width;
static unsigned char test_texture;
static void *test_texture_ptr = &test_texture;
static int trace_events[64];
static int trace_event_count;
static int source_width;
static int source_height;
static int destination_width;
static int destination_height;
static int destination_format;
static int destination_mipmap;
static int copy_count;
static int begin_vertices;
static int end_count;
static int init_count;
static int init_width;
static int init_height;
static int init_format;
static int lod_count;
static int projection_right;
static int position_count;
static float positions[16][3];
static int ao_end_count;
static int ao_exclude_count;
static int clear_count;

enum {
    TEST_EVENT_BEGIN = 1,
    TEST_EVENT_END,
    TEST_EVENT_SOURCE,
    TEST_EVENT_DESTINATION,
    TEST_EVENT_COPY,
    TEST_EVENT_CLEAR,
};

s16 Hu3DCameraNo;
void *lbl_1_bss_A70;
s16 lbl_1_bss_A74;
void *lbl_1_bss_13C8;
s16 lbl_1_bss_13CC;

static void test_event(int event)
{
    assert(trace_event_count < (int)(sizeof(trace_events) / sizeof(trace_events[0])));
    trace_events[trace_event_count++] = event;
}

static int mp6_widescreen_render_width(void)
{
    return test_width;
}

void mp6_ao_camera_end(int camera)
{
    assert(camera == Hu3DCameraNo);
    ++ao_end_count;
}

void mp6_ao_camera_exclude(int camera)
{
    assert(camera == Hu3DCameraNo);
    ++ao_exclude_count;
}

static void test_GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height)
{
    assert(left == 0 && top == 0);
    source_width = width;
    source_height = height;
    test_event(TEST_EVENT_SOURCE);
}

static void test_GXSetTexCopyDst(u16 width, u16 height, GXTexFmt format, GXBool mipmap)
{
    destination_width = width;
    destination_height = height;
    destination_format = format;
    destination_mipmap = mipmap;
    test_event(TEST_EVENT_DESTINATION);
}

static void test_GXCopyTex(void *destination, GXBool clear)
{
    assert(destination == test_texture_ptr && clear == GX_FALSE);
    ++copy_count;
    test_event(TEST_EVENT_COPY);
}

static void test_GXInitTexObj(GXTexObj *obj, void *data, u16 width, u16 height,
                              GXTexFmt format, GXTexWrapMode wrap_s,
                              GXTexWrapMode wrap_t, u8 mipmap)
{
    (void)obj;
    (void)data;
    (void)wrap_s;
    (void)wrap_t;
    (void)mipmap;
    ++init_count;
    init_width = width;
    init_height = height;
    init_format = format;
}

static void test_GXInitTexObjLOD(GXTexObj *obj, GXTexFilter min_filter,
                                 GXTexFilter mag_filter, f32 min_lod,
                                 f32 max_lod, f32 lod_bias, GXBool bias_clamp,
                                 GXBool edge_lod, GXAnisotropy max_aniso)
{
    (void)obj;
    (void)min_filter;
    (void)mag_filter;
    (void)min_lod;
    (void)max_lod;
    (void)lod_bias;
    (void)bias_clamp;
    (void)edge_lod;
    (void)max_aniso;
    ++lod_count;
}

static void test_MTXOrtho(Mtx44 matrix, f32 top, f32 bottom, f32 left, f32 right,
                          f32 near_plane, f32 far_plane)
{
    (void)matrix;
    (void)top;
    (void)bottom;
    (void)left;
    (void)near_plane;
    (void)far_plane;
    projection_right = (int)right;
}

static void test_GXBegin(GXPrimitive primitive, GXVtxFmt format, u16 vertices)
{
    (void)primitive;
    (void)format;
    begin_vertices = vertices;
    test_event(TEST_EVENT_BEGIN);
}

static void test_GXEnd(void)
{
    ++end_count;
    test_event(TEST_EVENT_END);
}

static void test_GXPosition3f32(f32 x, f32 y, f32 z)
{
    assert(position_count < (int)(sizeof(positions) / sizeof(positions[0])));
    positions[position_count][0] = x;
    positions[position_count][1] = y;
    positions[position_count][2] = z;
    ++position_count;
}

static void test_Hu3DZClear(void)
{
    ++clear_count;
    test_event(TEST_EVENT_CLEAR);
}

static float fn_1_35AF8(float start_value, float end_value, float time, float duration)
{
    if (time <= 0.0f) {
        return start_value;
    }
    if (time >= duration) {
        return end_value;
    }
    return start_value + ((time / duration) * (end_value - start_value));
}

static float fn_1_2D0FC(float start_value, float end_value, float time, float duration)
{
    if (time <= 0.0f) {
        return start_value;
    }
    if (time >= duration) {
        return end_value;
    }
    return start_value + ((time / duration) * (end_value - start_value));
}

#define GXSetTexCopySrc test_GXSetTexCopySrc
#define GXSetTexCopyDst test_GXSetTexCopyDst
#define GXCopyTex test_GXCopyTex
#define GXInitTexObj test_GXInitTexObj
#define GXInitTexObjLOD test_GXInitTexObjLOD
#define GXBegin test_GXBegin
#define GXEnd test_GXEnd
#define GXPosition3f32 test_GXPosition3f32
#define MTXOrtho test_MTXOrtho
#define Hu3DZClear test_Hu3DZClear
#define GXSetProjection(...) ((void)0)
#define GXLoadTexObj(...) ((void)0)
#define GXClearVtxDesc(...) ((void)0)
#define GXSetVtxDesc(...) ((void)0)
#define GXSetVtxAttrFmt(...) ((void)0)
#define GXSetNumTexGens(...) ((void)0)
#define GXSetTexCoordGen2(...) ((void)0)
#define GXSetNumChans(...) ((void)0)
#define GXSetChanCtrl(...) ((void)0)
#define GXSetNumTevStages(...) ((void)0)
#define GXSetBlendMode(...) ((void)0)
#define GXSetTevOrder(...) ((void)0)
#define GXSetTevColorIn(...) ((void)0)
#define GXSetTevColorOp(...) ((void)0)
#define GXSetTevAlphaIn(...) ((void)0)
#define GXSetTevAlphaOp(...) ((void)0)
#define GXColor4u8(...) ((void)0)
#define GXTexCoord2f32(...) ((void)0)
#define PSMTXIdentity(...) ((void)0)
#define GXLoadPosMtxImm(...) ((void)0)

#include "feedback_subject.inc"

static void trace_reset(void)
{
    memset(trace_events, 0, sizeof(trace_events));
    memset(positions, 0, sizeof(positions));
    trace_event_count = 0;
    source_width = source_height = -1;
    destination_width = destination_height = -1;
    destination_format = -1;
    destination_mipmap = -1;
    copy_count = begin_vertices = end_count = init_count = lod_count = 0;
    init_width = init_height = init_format = -1;
    projection_right = -1;
    position_count = 0;
    ao_end_count = ao_exclude_count = clear_count = 0;
}

static void assert_copy_contract(void)
{
    assert(source_width == test_width && source_height == 480);
    assert(destination_width == 320 && destination_height == 240);
    assert(destination_format == GX_TF_RGB565 && destination_mipmap == GX_TRUE);
    assert(copy_count == 1);
}

static void assert_feedback_contract(void)
{
    int copy_event = -1;
    int end_event = -1;
    assert_copy_contract();
    assert(init_count == 1 && init_width == 320 && init_height == 240);
    assert(init_format == GX_TF_RGB565 && lod_count == 1);
    assert(begin_vertices == 16 && position_count == 16 && end_count == 1);
    assert(projection_right == test_width);
    assert(ao_end_count == 1 && ao_exclude_count == 1);
    assert(clear_count == 1);
    for (int event = 0; event < trace_event_count; ++event) {
        if (trace_events[event] == TEST_EVENT_END && end_event < 0) {
            end_event = event;
        }
        if (trace_events[event] == TEST_EVENT_SOURCE && copy_event < 0) {
            copy_event = event;
        }
    }
    assert(end_event >= 0 && copy_event > end_event);
    for (int copy_no = 0; copy_no < 4; ++copy_no) {
        int base = copy_no * 4;
        assert(positions[base + 0][0] == -copy_no);
        assert(positions[base + 0][1] == -copy_no);
        assert(positions[base + 1][0] == test_width + copy_no);
        assert(positions[base + 1][1] == -copy_no);
        assert(positions[base + 2][0] == test_width + copy_no);
        assert(positions[base + 2][1] == 480 + copy_no);
        assert(positions[base + 3][0] == -copy_no);
        assert(positions[base + 3][1] == 480 + copy_no);
    }
}

static void assert_initial_contract(void (*capture)(s16))
{
    trace_reset();
    capture(0);
    assert_copy_contract();
    assert(init_count == 0 && lod_count == 0 && begin_vertices == 0);
    assert(position_count == 0 && end_count == 0);
    assert(ao_end_count == 0 && ao_exclude_count == 0 && clear_count == 0);
}

static void assert_feedback_sequence(void (*feedback)(s16))
{
    trace_reset();
    feedback(0);
    assert_feedback_contract();
}

int main(void)
{
    static const int widths[] = {640, 848, 1120, 2560, 640};
    Hu3DCameraNo = 3;
    lbl_1_bss_A70 = test_texture_ptr;
    lbl_1_bss_13C8 = test_texture_ptr;

    for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        test_width = widths[i];
        lbl_1_bss_A74 = 0;
        assert_initial_contract(fn_1_35B6C);
        lbl_1_bss_13CC = 0;
        assert_initial_contract(fn_1_2D170);
        lbl_1_bss_A74 = 0;
        assert_feedback_sequence(fn_1_366CC);
        lbl_1_bss_13CC = 0;
        assert_feedback_sequence(fn_1_2DCD0);
    }
    return 0;
}
