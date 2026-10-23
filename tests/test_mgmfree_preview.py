"""Bounded native probes for the recovered MGMfree preview helpers.

The probe is deliberately built from the current patched ``menu.c`` bodies,
not a hand-written copy of their control flow.  Its small host-side stubs
record the GX/THP/sprite calls so the native decisions remain observable
without linking the complete overlay.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build


ROOT = Path(__file__).resolve().parents[1]
MENU_REL = "src/REL/mgmfreedll/menu.c"


def _balanced_function(source: str, name: str) -> str:
    """Extract one complete C function from *source*, including its signature."""

    match = re.search(rf"\b{name}\s*\([^;{{]*\)\s*\{{", source)
    if match is None:
        raise AssertionError(f"patched menu.c is missing {name}")
    # The name-only match keeps the locator tolerant of recovered return-type
    # spelling; include the complete declaration line in the extracted probe.
    start = source.rfind("\n", 0, match.start()) + 1
    brace = source.index("{", match.start(), match.end())
    depth = 0
    quote: str | None = None
    escaped = False
    line_comment = False
    block_comment = False
    index = brace
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and next_char == "/":
                block_comment = False
                index += 1
        elif quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char == "/" and next_char == "/":
            line_comment = True
            index += 1
        elif char == "/" and next_char == "*":
            block_comment = True
            index += 1
        elif char in "\"'":
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
        index += 1
    raise AssertionError(f"unterminated production function {name}")


def _patched_menu() -> str:
    decomp = Path(apply_patches.require_pinned_decomp())
    original = (decomp / MENU_REL).read_text()
    patch = ROOT / "compat/decomp" / f"{MENU_REL}.patch"
    return apply_patches.apply_unified_diff(original, patch.read_text(), MENU_REL)


_PROBE_PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef float f32;

typedef int HUSPRID;
typedef int HUSPR_GROUPID;
typedef int HU3D_ANIMID;

typedef struct AnimBitmap {
    void *data;
} ANIMBMP;

typedef struct AnimData {
    ANIMBMP *bmp;
} ANIMDATA;

typedef struct Object {
    s32 work[2];
} OMOBJ;

typedef struct MgDataEntry {
    const char *movie[1][1];
    int instPic[1][1];
} MgDataEntry;

enum {
    GX_TF_RGBA8 = 7,
    HUSPR_ATTR_DISPOFF = 4,
    HU_MEMNUM_OVL = 11,
    HEAP_MODEL = 12,
};

static s16 lbl_1_data_3AE = 100;
static ANIMDATA *lbl_1_bss_4DC[7];
static ANIMDATA *lbl_1_bss_40;
static HUSPRID lbl_1_bss_4AA[2];
static HUSPR_GROUPID lbl_1_bss_4AE;
static s16 lbl_1_bss_4A8;
static s16 lbl_1_bss_580[8][32][2];
static s16 lbl_1_bss_9D0[4];
static HU3D_ANIMID lbl_1_bss_4B0[7];
static ANIMDATA *lbl_1_bss_554[11];
static MgDataEntry MgDataTbl[8];

static int g_render_width;
static int g_copy_src[4];
static int g_copy_dst[4];
static int g_copy_calls;
static int g_copy_format;
static void *g_copy_data;
static int g_native_frame_ready;
static int g_native_copy_calls;

static int g_thp_close_calls;
static int g_thp_proc_check;
static int g_thp_start_check;
static int g_thp_create_calls;
static int g_thp_create_result;
static const char *g_last_movie;
static int g_member_bind_calls;
static int g_group_draw_calls;
static int g_attr_set_calls;
static int g_anim_set_calls;
static ANIMDATA *g_last_anim;
static int g_asset_read_calls;
static int g_anim_read_calls;
static int g_anim_lock_calls;
static int g_sprite_create_calls;

static void reset_probe(void)
{
    memset(g_copy_src, 0, sizeof(g_copy_src));
    memset(g_copy_dst, 0, sizeof(g_copy_dst));
    g_copy_calls = 0;
    g_copy_format = 0;
    g_copy_data = NULL;
    g_native_frame_ready = 0;
    g_native_copy_calls = 0;
    g_thp_close_calls = 0;
    g_thp_proc_check = 0;
    g_thp_start_check = 0;
    g_thp_create_calls = 0;
    g_thp_create_result = -1;
    g_last_movie = NULL;
    g_member_bind_calls = 0;
    g_group_draw_calls = 0;
    g_attr_set_calls = 0;
    g_anim_set_calls = 0;
    g_last_anim = NULL;
    g_asset_read_calls = 0;
    g_anim_read_calls = 0;
    g_anim_lock_calls = 0;
    g_sprite_create_calls = 0;
    lbl_1_data_3AE = 100;
    lbl_1_bss_40 = NULL;
    memset(lbl_1_bss_4AA, 0, sizeof(lbl_1_bss_4AA));
    memset(lbl_1_bss_580, 0, sizeof(lbl_1_bss_580));
    memset(lbl_1_bss_9D0, 0, sizeof(lbl_1_bss_9D0));
    memset(lbl_1_bss_4B0, 0, sizeof(lbl_1_bss_4B0));
    memset(lbl_1_bss_554, 0, sizeof(lbl_1_bss_554));
    memset(MgDataTbl, 0, sizeof(MgDataTbl));
}

int mp6_widescreen_render_width(void)
{
    return g_render_width;
}

void GXSetTexCopySrc(int x, int y, int width, int height)
{
    g_copy_src[0] = x;
    g_copy_src[1] = y;
    g_copy_src[2] = width;
    g_copy_src[3] = height;
}

void GXSetTexCopyDst(int width, int height, int format, int mipmap)
{
    g_copy_dst[0] = width;
    g_copy_dst[1] = height;
    g_copy_dst[2] = mipmap;
    g_copy_dst[3] = 0;
    g_copy_format = format;
}

void GXCopyTex(void *data, int clear)
{
    (void)clear;
    g_copy_data = data;
    g_copy_calls++;
}

int mp6_thp_copy_frame_rgba8(void *data, int width, int height)
{
    assert(data != NULL && width == 128 && height == 128);
    g_native_copy_calls++;
    return g_native_frame_ready;
}

void HuTHPClose(void)
{
    g_thp_close_calls++;
}

int HuTHPProcCheck(void)
{
    return g_thp_proc_check;
}

HUSPRID HuTHPSprCreateVol(const char *movie, int volume, int priority, f32 pan)
{
    (void)volume;
    (void)priority;
    (void)pan;
    g_thp_create_calls++;
    g_last_movie = movie;
    return g_thp_create_result;
}

int HuTHPStartCheck(void)
{
    return g_thp_start_check;
}

void HuSprGrpMemberSet(HUSPR_GROUPID group, int member, HUSPRID sprite)
{
    (void)group;
    (void)member;
    (void)sprite;
    g_member_bind_calls++;
}

void HuSprGrpDrawNoSet(HUSPR_GROUPID group, int draw_no)
{
    (void)group;
    (void)draw_no;
    g_group_draw_calls++;
}

void HuSprAttrSet(HUSPR_GROUPID group, int member, int attr)
{
    (void)group;
    (void)member;
    (void)attr;
    g_attr_set_calls++;
}

void HuSprKill(HUSPRID sprite)
{
    (void)sprite;
}

void HuSprAnimKill(ANIMDATA *anim)
{
    (void)anim;
}

void Hu3DAnimAnimSet(HU3D_ANIMID anim_id, ANIMDATA *anim)
{
    (void)anim_id;
    g_last_anim = anim;
    g_anim_set_calls++;
}

int HuDataSelHeapReadNum(int number, int heap, int model)
{
    (void)number;
    (void)heap;
    (void)model;
    g_asset_read_calls++;
    return 4242;
}

ANIMDATA *HuSprAnimRead(int resource)
{
    static ANIMBMP bitmap = {0};
    static ANIMDATA anim = {&bitmap};
    (void)resource;
    g_anim_read_calls++;
    return &anim;
}

void HuSprAnimLock(ANIMDATA *anim)
{
    (void)anim;
    g_anim_lock_calls++;
}

HUSPRID HuSprCreate(ANIMDATA *anim, int priority, int bank)
{
    (void)anim;
    (void)priority;
    (void)bank;
    g_sprite_create_calls++;
    return 31337;
}

void HuSprAttrReset(HUSPR_GROUPID group, int member, int attr)
{
    (void)group;
    (void)member;
    (void)attr;
}

'''


_PROBE_SUFFIX = r'''
static void check_efb_copy(void)
{
    static ANIMBMP bitmap;
    static ANIMDATA anim;
    static unsigned char pixels[128 * 128 * 4];
    const int widths[] = {640, 848, 1120};
    const int expected_x[] = {0, 104, 240};
    int i;

    bitmap.data = pixels;
    anim.bmp = &bitmap;
    lbl_1_bss_4DC[5] = &anim;
    for (i = 0; i < 3; ++i) {
        reset_probe();
        lbl_1_bss_4DC[5] = &anim;
        g_render_width = widths[i];
        fn_1_16B4(0);
        assert(g_copy_src[0] == expected_x[i]);
        assert(g_copy_src[1] == 320);
        assert(g_copy_src[2] == 128 && g_copy_src[3] == 128);
        assert(g_copy_dst[0] == 128 && g_copy_dst[1] == 128);
        assert(g_copy_format == GX_TF_RGBA8);
        assert(g_copy_calls == 1 && g_copy_data == pixels);
        assert(g_native_copy_calls == 1);
    }
    reset_probe();
    lbl_1_bss_4DC[5] = &anim;
    g_native_frame_ready = 1;
    fn_1_16B4(0);
    assert(g_native_copy_calls == 1 && g_copy_calls == 0);
}

static void check_thp_failure_and_success(void)
{
    OMOBJ object = {{0, 0}};

    reset_probe();
    lbl_1_bss_4A8 = 0;
    MgDataTbl[0].movie[0][0] = "missing-decoder.thp";
    lbl_1_data_3AE = 0;
    fn_1_1218(&object);
    assert(lbl_1_data_3AE == 1 && g_thp_close_calls == 1);
    fn_1_1218(&object);
    assert(lbl_1_data_3AE == 2 && g_thp_close_calls == 2);
    g_thp_create_result = -1;
    fn_1_1218(&object);
    assert(lbl_1_data_3AE == 100);
    assert(g_thp_create_calls == 1 && g_last_movie == MgDataTbl[0].movie[0][0]);
    assert(g_member_bind_calls == 0 && g_group_draw_calls == 0);

    reset_probe();
    lbl_1_bss_4A8 = 0;
    MgDataTbl[0].movie[0][0] = "preview.thp";
    lbl_1_data_3AE = 0;
    g_thp_create_result = 77;
    fn_1_1218(&object);
    fn_1_1218(&object);
    fn_1_1218(&object);
    assert(lbl_1_data_3AE == 5);
    assert(g_member_bind_calls == 1 && g_group_draw_calls == 1);
    g_thp_start_check = 1;
    fn_1_1218(&object);
    assert(lbl_1_data_3AE == 6 && g_attr_set_calls == 2);
    for (int frame = 0; frame < 11; ++frame) {
        fn_1_1218(&object);
    }
    assert(lbl_1_data_3AE == 100);

    /* Open/preload failure can arrive after a valid sprite was returned. */
    for (int waiting_state = 3; waiting_state <= 5; waiting_state += 2) {
        reset_probe();
        lbl_1_data_3AE = waiting_state;
        fn_1_1218(&object);
        assert(lbl_1_data_3AE == 100 && g_attr_set_calls == 0);
    }
}

static void check_missing_and_locked_entries_skip_asset_reads(void)
{
    static ANIMDATA empty_anim;
    static ANIMDATA ready_anim;

    reset_probe();
    lbl_1_bss_4B0[5] = 9;
    lbl_1_bss_554[6] = &empty_anim;
    lbl_1_bss_554[7] = &ready_anim;
    lbl_1_bss_9D0[0] = 0;
    lbl_1_bss_9D0[1] = 0;
    lbl_1_bss_9D0[2] = 0;
    lbl_1_bss_9D0[3] = 0;
    lbl_1_bss_580[0][0][0] = -1;
    lbl_1_bss_580[0][0][1] = 1;
    fn_1_1400(1);
    assert(lbl_1_data_3AE == 100);
    assert(g_asset_read_calls == 0 && g_anim_read_calls == 0);
    assert(g_thp_close_calls == 1 && g_anim_set_calls == 1);
    assert(g_last_anim == &empty_anim);

    reset_probe();
    lbl_1_bss_4B0[5] = 9;
    lbl_1_bss_554[6] = &empty_anim;
    lbl_1_bss_554[7] = &ready_anim;
    lbl_1_bss_580[0][0][0] = 0;
    lbl_1_bss_580[0][0][1] = 0;
    MgDataTbl[0].instPic[0][0] = 1234;
    fn_1_1400(1);
    assert(lbl_1_data_3AE == 100);
    assert(g_asset_read_calls == 0 && g_anim_read_calls == 0);
    assert(g_thp_close_calls == 1 && g_anim_set_calls == 1);
    assert(g_last_anim == &empty_anim);
}

int main(void)
{
    check_efb_copy();
    check_thp_failure_and_success();
    check_missing_and_locked_entries_skip_asset_reads();
    puts("MGMfree preview native probe: PASS");
    return 0;
}
'''


class MgmfreePreviewNative(unittest.TestCase):
    def test_patched_preview_helpers_run_against_native_stubs(self):
        menu = _patched_menu()
        functions = {
            name: _balanced_function(menu, name)
            for name in ("fn_1_16B4", "fn_1_1218", "fn_1_1400")
        }
        probe = _PROBE_PREFIX + "\n".join(functions.values()) + _PROBE_SUFFIX

        with tempfile.TemporaryDirectory(
            prefix="mgmfree-preview-", dir=ROOT / "build"
        ) as temporary:
            folder = Path(temporary)
            source = folder / "preview_probe.c"
            executable = folder / "preview_probe.exe"
            source.write_text(probe)
            compile_result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    "-std=c11",
                    "-O2",
                    "-UNDEBUG",
                    "-Werror=implicit-function-declaration",
                    "-Werror=incompatible-pointer-types",
                    str(source),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stdout + compile_result.stderr,
            )
            run_result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(
                run_result.returncode,
                0,
                run_result.stdout + run_result.stderr,
            )
            self.assertIn("MGMfree preview native probe: PASS", run_result.stdout)


if __name__ == "__main__":
    unittest.main()
