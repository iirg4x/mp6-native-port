"""Native selection, storage-reset, and THP contracts for Endurance Alley."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build, native_modes, native_namespace


ROOT = Path(__file__).resolve().parents[1]
MODULE = "mgmrenshodll"
UNITS = ("camera", "course", "main", "messages", "player", "preview", "scenery")

RESET_OWNERS = {
    "camera": {
        "bss": ("lbl_1_bss_B0", "lbl_1_bss_A8"),
        "data": ("lbl_1_data_0",),
    },
    "course": {
        "bss": ("lbl_1_bss_104", "lbl_1_bss_100", "lbl_1_bss_FC",
                "lbl_1_bss_CE", "lbl_1_bss_CC", "lbl_1_bss_CA", "lbl_1_bss_C8"),
        "data": ("lbl_1_data_1C8", "lbl_1_data_224", "lbl_1_data_22D",
                 "lbl_1_data_237", "lbl_1_data_241", "lbl_1_data_24B",
                 "lbl_1_data_255", "lbl_1_data_25F", "lbl_1_data_269",
                 "lbl_1_data_273", "lbl_1_data_27D", "lbl_1_data_287"),
    },
    "main": {
        "bss": ("lbl_1_bss_158", "lbl_1_bss_154", "lbl_1_bss_152", "lbl_1_bss_150"),
        "data": ("lbl_1_data_500", "lbl_1_data_504", "lbl_1_data_508",
                 "lbl_1_data_50C", "lbl_1_data_510", "lbl_1_data_514",
                 "lbl_1_data_518", "lbl_1_data_51C"),
    },
    "messages": {
        "bss": ("lbl_1_bss_B8",),
        "data": ("lbl_1_data_10", "lbl_1_data_30", "lbl_1_data_58",
                 "lbl_1_data_5C", "lbl_1_data_94", "lbl_1_data_1C0"),
    },
    "player": {
        "bss": ("lbl_1_bss_140",),
        "data": ("lbl_1_data_338", "lbl_1_data_360", "lbl_1_data_364",
                 "lbl_1_data_368", "lbl_1_data_380", "lbl_1_data_45C"),
    },
    "preview": {
        "bss": ("lbl_1_bss_A4", "lbl_1_bss_A2", "lbl_1_bss_A0",
                "lbl_1_bss_9C", "lbl_1_bss_98", "lbl_1_bss_94",
                "lbl_1_bss_90", "lbl_1_bss_8C", "lbl_1_bss_88",
                "lbl_1_bss_84", "lbl_1_bss_2", "lbl_1_bss_0"),
        "data": (),
    },
    "scenery": {
        "bss": ("lbl_1_bss_138", "lbl_1_bss_12C", "lbl_1_bss_128",
                "lbl_1_bss_124", "lbl_1_bss_11C", "lbl_1_bss_118"),
        "data": ("lbl_1_data_298", "lbl_1_data_2D4", "lbl_1_data_2E4"),
    },
}


def patched(decomp, unit):
    rel = f"src/REL/{MODULE}/{unit}.c"
    original = (decomp / rel).read_text(encoding="utf-8")
    patch = ROOT / "compat/decomp" / (rel + ".patch")
    if patch.exists():
        original = apply_patches.apply_unified_diff(original, patch.read_text(encoding="utf-8"), rel)
    return apply_patches.with_rel_state(original, rel)


def function_body(source, name):
    match = re.search(r"(?m)^.*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", source)
    if match is None:
        raise AssertionError(f"missing function definition: {name}")
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():index + 1]
    raise AssertionError(f"unterminated function: {name}")


def strip_native_function_bodies(source, keep=()):
    """Keep real globals/macros while removing unrelated engine call sites."""
    keep = set(keep)
    definitions = list(re.finditer(
        r"(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_]\w*[ \t*]+"
        r"(\w+)\s*\([^;{}]*\)\s*\{", source))
    for match in reversed(definitions):
        if match.group(1) in keep:
            continue
        brace = source.index("{", match.start())
        depth = 0
        end = brace
        while end < len(source):
            if source[end] == "{":
                depth += 1
            elif source[end] == "}":
                depth -= 1
                if depth == 0:
                    end += 1
                    break
            end += 1
        source = source[:match.start()] + source[match.start():brace].rstrip() + ";" + source[end:]
    return source


def execute_reset_owners(testcase, decomp, module, units, entry_unit):
    """Execute real storage owners once per unit, with no engine/gameplay stubs."""
    prefix = module.removesuffix("dll")
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
        temporary = Path(temporary)
        patched_root = temporary / "patched"
        apply_patches.apply_all(out_dir=str(patched_root))
        flags_by_unit = {
            Path(rel).stem: flags
            for rel, flags in native_modes.sources(decomp)
            if Path(rel).parent.name == module
        }
        objects = []
        for unit in units:
            owner_prefix = f"MP6_{prefix.upper()}_{unit.upper()}"
            data_macro = f"{owner_prefix}_DATA"
            bss_macro = f"{owner_prefix}_BSS"
            reset = f"mp6_{prefix}_{unit}_reset"
            rel = f"src/REL/{module}/{unit}.c"
            # apply_all appends the unit's src/rel_state block, as the build does.
            source = (patched_root / rel).read_text(encoding="utf-8")
            source = strip_native_function_bodies(
                source,
                keep=(f"mp6_{prefix}_reset",) if unit == entry_unit else (),
            )
            probe = temporary / f"reset_{unit}.c"
            probe.write_text(f'''#include <stddef.h>
#include <string.h>

{source}

#define MP6_JOIN_I(a, b) a##b
#define MP6_JOIN(a, b) MP6_JOIN_I(a, b)
#define MP6_DECLARE_DATA(n) unsigned char MP6_JOIN(saved_, n)[sizeof(n)];
#define MP6_SAVE_DATA(n) memcpy(MP6_JOIN(saved_, n), &(n), sizeof(n));
#define MP6_DIRTY(n) memset(&(n), 0xA5, sizeof(n));
#define MP6_CHECK_DATA(n) \\
do {{ if (memcmp(MP6_JOIN(saved_, n), &(n), sizeof(n)) != 0) failures = 1; }} while (0);
#define MP6_CHECK_BSS(n) \\
do {{ const unsigned char *bytes = (const unsigned char *)&(n); \\
     for (size_t i = 0; i < sizeof(n); ++i) if (bytes[i] != 0) failures = 1; }} while (0);

int probe_{unit}(void)
{{
int failures = 0;
{data_macro}(MP6_DECLARE_DATA)
{reset}();
{data_macro}(MP6_SAVE_DATA)
{data_macro}(MP6_DIRTY)
{bss_macro}(MP6_DIRTY)
{reset}();
{data_macro}(MP6_CHECK_DATA)
{bss_macro}(MP6_CHECK_BSS)
return failures;
}}
''', encoding="utf-8")
            output = temporary / f"reset_{unit}.o"
            result = subprocess.run([
                build.ZIG, "cc", "-I", str(patched_root / "include"),
                "-I", str(ROOT / "include"), *build.COMMON_FLAGS,
                *flags_by_unit[unit],
                "-c", str(probe), "-o", str(output),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            testcase.assertEqual(result.returncode, 0,
                             f"{unit} reset probe compile failed:\n{result.stderr}")
            objects.append(output)

        entry = temporary / "reset_main.c"
        declarations = "\n".join(f"int probe_{unit}(void);" for unit in units)
        calls = "\n".join(f"    failures |= probe_{unit}();" for unit in units)
        entry.write_text(f'''{declarations}
void mp6_{prefix}_reset(void);
int main(void)
{{
int failures = 0;
{calls}
mp6_{prefix}_reset();
return failures;
}}
''', encoding="utf-8")
        executable = temporary / "reset_probe.exe"
        result = subprocess.run([
            build.ZIG, "cc", *build.COMMON_FLAGS, "-Wl,--gc-sections",
            *(str(obj) for obj in objects), str(entry), "-o", str(executable),
        ], cwd=ROOT, capture_output=True, text=True, timeout=60)
        testcase.assertEqual(result.returncode, 0,
                         f"reset probe link failed:\n{result.stderr}")
        result = subprocess.run([str(executable)], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        testcase.assertEqual(result.returncode, 0,
                         f"reset probe reported storage mismatch:\n{result.stdout}\n{result.stderr}")



def execute_preview_draw(testcase, preview):
    """Execute fn_1_898 with GX stubs and verify primitive ownership."""
    draw = function_body(preview, "fn_1_898")
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
        temporary = Path(temporary)
        source = temporary / "preview_draw_probe.c"
        executable = temporary / "preview_draw_probe.exe"
        source.write_text(r'''#include <assert.h>

typedef short s16;
typedef float f32;
typedef f32 Mtx[3][4];
typedef struct HU3D_MODEL_s { int unused; } HU3D_MODEL;
typedef struct ANIMDATA_s { int tag; } ANIMDATA;

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
#define GX_FALSE 0
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
#define GX_TF_RGB565 5
#define TOURNAMENT_PREVIEW_CAPTURE_WIDTH 284
#define TOURNAMENT_PREVIEW_CAPTURE_HEIGHT 256

static int primitive_open;
static int begin_calls;
static int end_calls;
static int positions;
static int texcoords;
static int positions_in_primitive;
static int texcoords_in_primitive;
static int state_calls;
static int state_while_open;
static int thp_checks;
static int thp_started;
static int native_texture_loads;
static int still_texture_loads;
static int overlay_texture_loads;

static void state_call(void) {
state_calls += 1;
if (primitive_open) state_while_open += 1;
}
static void GXInvalidateTexAll(void) { state_call(); }
static void GXClearVtxDesc(void) { state_call(); }
static void GXSetVtxDesc(int attr, int type) {
(void)attr; (void)type; state_call();
}
static void GXSetVtxAttrFmt(int format, int attr, int type, int comp, int shift) {
(void)format; (void)attr; (void)type; (void)comp; (void)shift; state_call();
}
static void GXSetCullMode(int mode) { (void)mode; state_call(); }
static void GXSetZMode(int enable, int compare, int update) {
(void)enable; (void)compare; (void)update; state_call();
}
static void GXSetBlendMode(int mode, int src, int dst, int logic) {
(void)mode; (void)src; (void)dst; (void)logic; state_call();
}
static void GXSetChanCtrl(int channel, int enable, int src, int amb, int light,
                      int diffuse, int alpha) {
(void)channel; (void)enable; (void)src; (void)amb; (void)light;
(void)diffuse; (void)alpha; state_call();
}
static void GXSetNumChans(int count) { (void)count; state_call(); }
static void GXLoadPosMtxImm(const void *matrix, int id) {
(void)matrix; (void)id; state_call();
}
static int HuTHPStartCheck(void) {
thp_checks += 1;
return thp_started;
}
static void Hu3DTexLoad(void *texture, int width, int height, int format,
                    int wrap_s, int wrap_t, int mipmap, int map) {
(void)texture; (void)width; (void)height; (void)format;
(void)wrap_s; (void)wrap_t; (void)mipmap; (void)map;
native_texture_loads += 1;
state_call();
}
static void HuSprTexLoad(ANIMDATA *animation, int a, int b, int c, int d, int e) {
(void)animation; (void)a; (void)b; (void)c; (void)d; (void)e;
if (b == 0) still_texture_loads += 1;
else overlay_texture_loads += 1;
state_call();
}
static void GXSetTexCoordGen2(int coord, int function, int source, int matrix,
                          int normalize, int post) {
(void)coord; (void)function; (void)source; (void)matrix;
(void)normalize; (void)post; state_call();
}
static void GXSetTevOrder(int stage, int coord, int map, int color) {
(void)stage; (void)coord; (void)map; (void)color; state_call();
}
static void GXSetTevColorIn(int stage, int a, int b, int c, int d) {
(void)stage; (void)a; (void)b; (void)c; (void)d; state_call();
}
static void GXSetTevColorOp(int stage, int op, int bias, int scale,
                        int clamp, int reg) {
(void)stage; (void)op; (void)bias; (void)scale; (void)clamp; (void)reg;
state_call();
}
static void GXSetTevAlphaIn(int stage, int a, int b, int c, int d) {
(void)stage; (void)a; (void)b; (void)c; (void)d; state_call();
}
static void GXSetTevAlphaOp(int stage, int op, int bias, int scale,
                        int clamp, int reg) {
(void)stage; (void)op; (void)bias; (void)scale; (void)clamp; (void)reg;
state_call();
}
static void GXSetNumTexGens(int count) { (void)count; state_call(); }
static void GXSetNumTevStages(int count) { (void)count; state_call(); }
static void GXBegin(int primitive, int format, int vertex_count) {
assert(!primitive_open);
assert(primitive == GX_QUADS && format == GX_VTXFMT0 && vertex_count == 4);
primitive_open = 1;
begin_calls += 1;
positions_in_primitive = 0;
texcoords_in_primitive = 0;
}
static void GXPosition3f32(f32 x, f32 y, f32 z) {
(void)x; (void)y; (void)z;
assert(primitive_open);
positions += 1;
positions_in_primitive += 1;
}
static void GXTexCoord2f32(f32 s, f32 t) {
(void)s; (void)t;
assert(primitive_open);
texcoords += 1;
texcoords_in_primitive += 1;
}
static void GXEnd(void) {
assert(primitive_open);
assert(positions_in_primitive == 4 && texcoords_in_primitive == 4);
primitive_open = 0;
end_calls += 1;
}

void *lbl_1_bss_94;
ANIMDATA *lbl_1_bss_98;
ANIMDATA *lbl_1_bss_9C;

''' + draw + r'''

int main(void) {
HU3D_MODEL model = { 0 };
Mtx matrix = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f, 0.0f }
};
int active;

for (active = 0; active <= 1; ++active) {
    primitive_open = 0;
    begin_calls = 0;
    end_calls = 0;
    positions = 0;
    texcoords = 0;
    positions_in_primitive = 0;
    texcoords_in_primitive = 0;
    state_calls = 0;
    state_while_open = 0;
    thp_checks = 0;
    native_texture_loads = 0;
    still_texture_loads = 0;
    overlay_texture_loads = 0;
    thp_started = active;
    fn_1_898(&model, &matrix);
    assert(!primitive_open);
    assert(begin_calls == 2 && end_calls == 2);
    assert(positions == 8 && texcoords == 8);
    assert(state_while_open == 0);
    assert(thp_checks == 1);
    assert(native_texture_loads == active);
    assert(still_texture_loads == !active);
    assert(overlay_texture_loads == 1);
}
return 0;
}
''', encoding="utf-8")
        result = subprocess.run([
            build.ZIG, "cc", *build.COMMON_FLAGS, "-UNDEBUG",
            str(source), "-o", str(executable),
        ], cwd=ROOT, capture_output=True, text=True, timeout=60)
        testcase.assertEqual(result.returncode, 0,
                         f"preview draw probe compile failed:\\n{result.stderr}")
        result = subprocess.run([str(executable)], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        testcase.assertEqual(result.returncode, 0,
                         f"preview draw probe failed:\\n{result.stdout}\\n{result.stderr}")



def execute_preview_control(testcase, preview):
    """Execute the patched preview entry points against bounded native stubs."""
    create = function_body(preview, "fn_1_0")
    start_movie = function_body(preview, "fn_1_6F4")
    capture = function_body(preview, "fn_1_854")
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
        temporary = Path(temporary)
        source = temporary / "preview_probe.c"
        executable = temporary / "preview_probe.exe"
        source.write_text(r'''#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef int s32;
typedef float f32;
typedef int HU3D_MODELID;
typedef int HUSPR_GROUPID;
typedef int HUSPRID;
typedef struct { f32 x, y, z; } Point3d;
typedef Point3d Vec;
typedef struct ANIMDATA_s { int tag; } ANIMDATA;
typedef struct {
int instPic[2][1];
int movie[2][1];
} MGDATA;

#define HUSPR_NONE (-1)
#define HU_MEMNUM_OVL 3
#define HEAP_MODEL 4
#define GX_TF_RGB565 5
#define DATA_mgmrensho 0
#define DATA_mgmtournament 0
#define HU3D_CAM0 1
#define TOURNAMENT_PREVIEW_CAPTURE_WIDTH 284
#define TOURNAMENT_PREVIEW_CAPTURE_HEIGHT 256
#define DATANUM(data, number) (number)

static MGDATA MgDataTbl[1] = {
{ { { 11 }, { 12 } }, { { 21 }, { 22 } } }
};
static ANIMDATA frame_animation = { 1 };
static ANIMDATA still_animation = { 2 };
static unsigned char framebuffer[284 * 256 * 2];

static int movie_create_result;
static int movie_create_calls;
static int member_set_calls;
static int draw_no_calls;
static int pos_set_calls;
static int last_member;
static int thp_started;
static int render_width;
static int framebuffer_copy_calls;
static int framebuffer_copy_x;
static int framebuffer_copy_y;
static int framebuffer_copy_width;
static int framebuffer_copy_height;
static void *framebuffer_copy_destination;

static void fn_1_898(void *model, void *matrix) {
(void)model;
(void)matrix;
}

static HU3D_MODELID Hu3DHookFuncCreate(void (*hook)(void *, void *)) {
assert(hook == fn_1_898);
return 9;
}
static void Hu3DModelCameraSet(HU3D_MODELID model, s16 camera) {
(void)model;
(void)camera;
}
static void Hu3DModelLayerSet(HU3D_MODELID model, s16 layer) {
(void)model;
(void)layer;
}
static void Hu3DModelPosSetV(HU3D_MODELID model, const Point3d *position) {
(void)model;
assert(position != NULL);
}
static void Hu3DModelScaleSet(HU3D_MODELID model, f32 x, f32 y, f32 z) {
(void)model;
assert(x == 1.0f && y == 1.0f && z == 1.0f);
}
static void *HuDataSelHeapReadNum(int data, s16 overlay, s16 heap) {
(void)data;
(void)overlay;
(void)heap;
return &frame_animation;
}
static void *HuDataReadNumHeapShortForce(int data, s16 overlay, s16 heap) {
(void)data;
(void)overlay;
(void)heap;
return &still_animation;
}
static ANIMDATA *HuSprAnimRead(void *data) {
return (ANIMDATA *)data;
}
static HUSPR_GROUPID HuSprGrpCreate(s16 member_count) {
assert(member_count == 1);
return 12;
}
static HUSPRID HuTHPSprCreateVol(int movie, s16 volume, s16 rate, f32 start) {
assert(movie == 21);
assert(volume == 1 && rate == 100 && start == 0.0f);
movie_create_calls += 1;
return movie_create_result;
}
static void HuSprGrpMemberSet(HUSPR_GROUPID group, s16 member, HUSPRID sprite) {
assert(group == 12 && member == 0);
assert(sprite != HUSPR_NONE);
member_set_calls += 1;
last_member = sprite;
}
static void HuSprGrpDrawNoSet(HUSPR_GROUPID group, s16 draw_no) {
assert(group == 12 && draw_no == 127);
draw_no_calls += 1;
}
static void HuSprPosSet(HUSPR_GROUPID group, s16 member, f32 x, f32 y) {
assert(group == 12 && member == 0 && x == 128.0f && y == 128.0f);
pos_set_calls += 1;
}
static void HuSprGrpMemberKill(HUSPR_GROUPID group, s16 member) {
assert(group == 12 && member == 0);
}
void fn_1_854(s16 previewLayer);
static int GXGetTexBufferSize(s16 width, s16 height, int format, s16 mipmap, s16 aa) {
assert(width == 284 && height == 256 && format == GX_TF_RGB565);
assert(mipmap == 0 && aa == 0);
return sizeof(framebuffer);
}
static void *HuMemDirectMallocNum(s16 alignment, int size, s16 heap) {
assert(alignment == 0 && size == (int)sizeof(framebuffer) && heap == HU_MEMNUM_OVL);
return framebuffer;
}
static void Hu3DCameraLayerHookSet(s16 camera, s16 layer, void (*hook)(s16)) {
assert(camera == 1 && layer == 0 && hook == fn_1_854);
}
static void HuTHPClose(void) {}
static int HuTHPProcCheck(void) { return 0; }
static void HuPrcVSleep(void) {}
static int HuTHPStartCheck(void) { return thp_started; }
static int mp6_widescreen_render_width(void) { return render_width; }
static void Hu3DFbCopyExec(s16 x, s16 y, s16 width, s16 height, int format,
                       s16 clear, void *destination) {
assert(format == GX_TF_RGB565 && clear == 0);
framebuffer_copy_calls += 1;
framebuffer_copy_x = x;
framebuffer_copy_y = y;
framebuffer_copy_width = width;
framebuffer_copy_height = height;
framebuffer_copy_destination = destination;
}

HU3D_MODELID lbl_1_bss_A4;
HUSPR_GROUPID lbl_1_bss_A2;
s16 lbl_1_bss_A0;
ANIMDATA *lbl_1_bss_9C;
ANIMDATA *lbl_1_bss_98;
void *lbl_1_bss_94;
MGDATA *lbl_1_bss_90;
ANIMDATA **lbl_1_bss_8C;
s16 lbl_1_bss_88;
ANIMDATA *lbl_1_bss_84;
u8 lbl_1_bss_2[128];
s16 lbl_1_bss_0;

''' + create + r'''

''' + start_movie + r'''

''' + capture + r'''

int main(void) {
Point3d position = { 0.0f, 0.0f, 0.0f };
int widths[] = { 640, 848, 1120 };
int i;

/* A missing movie must not bind HUSPR_NONE, while selection/still state survives. */
movie_create_result = HUSPR_NONE;
fn_1_0(0, 0, &position);
assert(lbl_1_bss_90 == &MgDataTbl[0]);
assert(lbl_1_bss_98 == &still_animation);
assert(movie_create_calls == 1);
assert(member_set_calls == 0);
assert(draw_no_calls == 0 && pos_set_calls == 0);
fn_1_6F4();
assert(lbl_1_bss_90 == &MgDataTbl[0]);
assert(lbl_1_bss_98 == &still_animation);
assert(member_set_calls == 0);
assert(draw_no_calls == 0 && pos_set_calls == 0);
assert(movie_create_calls == 2);

/* Both creation sites bind an actual sprite, and the follow-up failure is benign. */
movie_create_result = 17;
fn_1_0(0, 0, &position);
assert(member_set_calls == 1 && last_member == 17);
assert(draw_no_calls == 1 && pos_set_calls == 1);
movie_create_result = 23;
fn_1_6F4();
assert(member_set_calls == 2 && last_member == 23);
assert(draw_no_calls == 2 && pos_set_calls == 2);
movie_create_result = HUSPR_NONE;
fn_1_6F4();
assert(member_set_calls == 2);
assert(draw_no_calls == 2 && pos_set_calls == 2);
assert(lbl_1_bss_90 == &MgDataTbl[0]);

/* Inactive THP performs no copy; active THP centers the native 284x256 image. */
lbl_1_bss_94 = framebuffer;
thp_started = 0;
for (i = 0; i < 3; ++i) {
    render_width = widths[i];
    fn_1_854(0);
}
assert(framebuffer_copy_calls == 0);
thp_started = 1;
for (i = 0; i < 3; ++i) {
    render_width = widths[i];
    fn_1_854(0);
    assert(framebuffer_copy_x == (widths[i] - 640) / 2);
    assert(framebuffer_copy_y == 0);
    assert(framebuffer_copy_width == 284 && framebuffer_copy_height == 256);
    assert(framebuffer_copy_destination == framebuffer);
}
assert(framebuffer_copy_calls == 3);
return 0;
}
''', encoding="utf-8")
        result = subprocess.run([
            build.ZIG, "cc", *build.COMMON_FLAGS, "-UNDEBUG",
            str(source), "-o", str(executable),
        ], cwd=ROOT, capture_output=True, text=True, timeout=60)
        testcase.assertEqual(result.returncode, 0,
                         f"preview probe compile failed:\n{result.stderr}")
        result = subprocess.run([str(executable)], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        testcase.assertEqual(result.returncode, 0,
                         f"preview probe failed:\n{result.stdout}\n{result.stderr}")



class NativeModes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())

    def test_only_complete_units_are_selected_and_namespaced(self):
        self.assertEqual(native_modes.MODULES[MODULE], UNITS)
        units = [(rel, flags) for rel, flags in native_modes.sources(self.decomp)
                 if Path(rel).parent.name == MODULE]
        self.assertEqual([rel for rel, _ in units], [
            f"src/REL/{MODULE}/{unit}.c" for unit in UNITS
        ])
        self.assertNotIn("runtime.c", "\n".join(rel for rel, _ in units))

        files = [self.decomp / f"src/REL/{MODULE}/{unit}.c" for unit in UNITS]
        files += sorted((self.decomp / f"include/REL/{MODULE}").glob("*.h"))
        expected = native_namespace.namespace_flags(
            files, "mgmrensho",
            extra_names={"_prolog", "_epilog", "_ctors", "_dtors", "ObjectSetup"},
        )
        self.assertEqual(native_modes.namespace_flags(self.decomp, MODULE), expected)
        for rel, flags in units:
            self.assertEqual(flags[:len(expected)], expected, rel)
            source = (self.decomp / rel).read_text(encoding="utf-8")
            for label in set(re.findall(r"\b(?:fn_1_|lbl_1_)\w+", source)):
                self.assertIn(f"-D{label}=mgmrensho_{label}", flags, (rel, label))
        for name in ("_prolog", "_epilog", "_ctors", "_dtors", "ObjectSetup"):
            self.assertIn(f"-D{name}=mgmrensho_{name.lstrip('_')}", expected)

    def test_preview_and_startup_native_fallback_contracts(self):
        preview = patched(self.decomp, "preview")
        startup = patched(self.decomp, "main")
        self.assertEqual(len(re.findall(r"if \((?:movieSpriteId|movieSprite) != HUSPR_NONE\)", preview)), 2)
        self.assertEqual(preview.count("HuSprGrpMemberSet(lbl_1_bss_A2, 0"), 2)
        capture = function_body(preview, "fn_1_854")
        self.assertIn("if (HuTHPStartCheck() != 0)", capture)
        self.assertIn("(s16)((mp6_widescreen_render_width() - 640) / 2)", capture)
        self.assertEqual(capture.count("Hu3DFbCopyExec("), 1)
        self.assertIn("#include \"mp6_widescreen.h\"", preview)
        loop = function_body(startup, "fn_1_EAD8")
        self.assertRegex(loop, r"s16 stop;\s+\n\s*stop = 1;")
        self.assertIn("while (stop != 0)", loop)

    def test_preview_entry_signature_matches_main_declaration(self):
        preview = patched(self.decomp, "preview")
        startup = patched(self.decomp, "main")
        declaration = re.search(
            r"extern HU3D_MODELID fn_1_0\(([^)]*)\);", startup)
        definition = re.search(
            r"HU3D_MODELID fn_1_0\(([^)]*)\)\s*\{", preview)
        self.assertIsNotNone(declaration)
        self.assertIsNotNone(definition)

        def abi_types(signature):
            signature = re.sub(
                r"\b(?:minigameIndex|stillIndex|position|previewPosition)\b",
                "",
                signature.group(1),
            ).replace("Point3d", "Vec")
            return re.sub(r"\s+", " ", signature).strip()

        self.assertEqual(abi_types(declaration), abi_types(definition))
        self.assertIn("s16", declaration.group(1))
        self.assertIn("s16", definition.group(1))

    def test_preview_draw_hook_executes_balanced_quads(self):
        execute_preview_draw(self, patched(self.decomp, "preview"))

    def test_preview_thp_fallback_and_native_capture_execute(self):
        execute_preview_control(self, patched(self.decomp, "preview"))

    def test_each_tu_has_complete_explicit_reload_owner(self):
        for unit, owners in RESET_OWNERS.items():
            source = patched(self.decomp, unit)
            for kind, names in owners.items():
                for name in names:
                    self.assertEqual(source.count(f"X({name})"), 1, (unit, kind, name))
            self.assertIn(f"MP6_REL_RESET_DEFINE(mp6_mgmrensho_{unit}_reset", source)
        main = patched(self.decomp, "main")
        for unit in UNITS:
            if unit != "main":
                self.assertIn(f"mp6_mgmrensho_{unit}_reset();", main)
        self.assertIn("void mp6_mgmrensho_reset(void)", main)

    def test_units_compile_with_strict_native_prototypes(self):
        """Compile one source at a time; this is intentionally not a full link/build."""
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            patched_root = temporary / "patched"
            apply_patches.apply_all(out_dir=str(patched_root))
            for index, (rel, flags) in enumerate(native_modes.sources(self.decomp)):
                source = patched_root / rel
                if not source.is_file():
                    source = self.decomp / rel
                output = temporary / f"mgmrensho_{index}.o"
                pointer_checks = (["-Werror=pointer-to-int-cast", "-Werror=int-to-pointer-cast"]
                                  if Path(rel).parent.name in ("mgmtournamentdll", "ttwarsdll") else [])
                result = subprocess.run([
                    build.ZIG, "cc", "-I", str(patched_root / "include"),
                    "-I", str(ROOT / "include"), *build.COMMON_FLAGS, *flags,
                    "-Werror=implicit-function-declaration",
                    "-Werror=incompatible-pointer-types", "-Werror=int-conversion",
                    "-Werror=incompatible-function-pointer-types",
                    *pointer_checks,
                    "-c", str(source), "-o", str(output),
                ], cwd=ROOT, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0,
                                 f"{rel} failed strict native compile:\n{result.stderr}")
                self.assertTrue(output.is_file())

    def test_real_native_reset_owners_restore_storage(self):
        execute_reset_owners(self, self.decomp, MODULE, UNITS, "main")


if __name__ == "__main__":
    unittest.main()
