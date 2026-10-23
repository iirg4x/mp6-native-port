"""Native ABI, lifecycle, and fallback contracts for the recovered instdll."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

from tools import apply_patches, build, native_inst, native_namespace


ROOT = Path(__file__).resolve().parents[1]


def recovered_bridge_inventory(source):
    body = source.split('#define MP6_RECOVERED_DLLS(X)', 1)[1]
    body = body.split('#define MP6_ENUM_RECOVERED_DLL', 1)[0]
    rows = {}
    for raw in re.findall(r'^\s+X\(([^)]*)\)', body, re.M):
        fields = [field.strip() for field in raw.split(',')]
        fields[1] = fields[1].strip('"')
        fields[4] = fields[4].strip('"')
        rows[fields[0]] = tuple(fields[1:])
    return rows


def function_body(source, name):
    start = source.index(name + "(")
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError("Unterminated function: " + name)


def function_definition_body(source, name):
    """Extract a definition when the source also has a forward declaration."""
    match = re.search(
        r"(?m)^[ \t]*(?:[A-Za-z_]\w*[ \t]+)+[* \t]*" + re.escape(name) +
        r"\s*\([^;{}]*\)\s*\{", source)
    if match is None:
        raise AssertionError("Missing function definition: " + name)
    brace = match.end() - 1
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():index + 1]
    raise AssertionError("Unterminated function: " + name)


def reset_owner_names(source, unit):
    """The unit's generated src/rel_state lists (tools/gen_rel_state.py)."""
    def listed(kind):
        match = re.search(r"#define MP6_INST_" + unit.upper() + "_" + kind
                          + r"\(X\)((?:[^\n]*\\\n)*[^\n]*)", source)
        return sorted(re.findall(r"X\((\w+)\)", match.group(1))) if match else []
    return listed("DATA"), listed("BSS")


def reset_block(unit):
    return (ROOT / "src/rel_state/REL/instDll" / f"{unit}.c.inc").read_text(encoding="utf-8")


class InstIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())

    def patched(self, rel):
        return apply_patches.generated_source(self.decomp, rel)

    def test_import_is_lazy_before_a_decomp_checkout_exists(self):
        """Importing the closure helper must not read or enumerate decomp data."""
        with tempfile.TemporaryDirectory() as temporary:
            env = os.environ.copy()
            env["PYTHONPATH"] = str(ROOT) + os.pathsep + env.get("PYTHONPATH", "")
            missing = Path(temporary) / "does-not-exist"
            env["MP6_DECOMP_DIR"] = str(missing)
            result = subprocess.run([
                sys.executable, "-c",
                "from pathlib import Path; from tools import build, native_inst; "
                "assert Path(build.DECOMP) == Path(r'" + str(missing) + "'); "
                "assert not Path(build.DECOMP).exists(); "
                "assert native_inst.UNITS == ('prolog', 'setup', 'stage', 'display', 'name')",
            ], cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_only_complete_units_are_selected_and_fully_namespaced(self):
        units = list(native_inst.sources(self.decomp))
        expected = [
            "src/REL/instDll/prolog.c",
            "src/REL/instDll/setup.c",
            "src/REL/instDll/stage.c",
            "src/REL/instDll/display.c",
            "src/REL/instDll/name.c",
        ]
        self.assertEqual([rel for rel, _ in units], expected)
        self.assertNotIn("runtime.c", "\n".join(rel for rel, _ in units))

        source_paths = [self.decomp / rel for rel in expected]
        namespace = native_namespace.namespace_flags(
            source_paths, "inst",
            extra_names={"_prolog", "_epilog", "_ctors", "_dtors"},
        )
        for rel, flags in units:
            self.assertEqual(flags[:len(namespace)], namespace, rel)
            source = (self.decomp / rel).read_text()
            for name in set(re.findall(r"\b(?:fn_1_|lbl_1_)\w+", source)):
                self.assertIn(f"-D{name}=inst_{name}", flags, (rel, name))
        all_flags = set(units[0][1])
        for name in ("_prolog", "_epilog", "_ctors", "_dtors"):
            self.assertIn(f"-D{name}=inst_{name.lstrip('_')}", all_flags)

    def test_bridge_exposes_inst_before_unavailable_overlay_fallback(self):
        bridge = (ROOT / "src/os/dll_bridge.c").read_text()
        self.assertEqual(recovered_bridge_inventory(bridge)['inst'], (
            'dll/instdll.rel', 'inst_prolog', 'inst_epilog',
            'instdll', 'INST', 'MP6_INST_CTORS'))
        self.assertIn('mp6_recovered_dll_by_path(path)', bridge)
        self.assertIn('mp6_recovered_dll_by_id(dll)', bridge)
        self.assertIn('mp6_dll_bridge_report_bound(recovered->name);', bridge)

    def test_movie_capture_uses_owned_decoded_pixels(self):
        display = self.patched("src/REL/instDll/display.c")
        callback = function_definition_body(display, "fn_1_9CB0")
        probe = r'''#include <assert.h>
#include <stdint.h>
typedef int16_t s16;
enum { INST_CAPTURE_WIDTH = 256, INST_CAPTURE_HEIGHT = 256 };
static unsigned char pixels[256 * 256 * 4];
static void *lbl_1_bss_4C = pixels;
static int started, copies;
static int HuTHPStartCheck(void) { return started; }
static int mp6_thp_copy_frame_rgba8(void *out, unsigned w, unsigned h) {
    assert(out == pixels && w == 256 && h == 256);
    pixels[0] = (unsigned char)++copies;
    return 1;
}
''' + callback + r'''
int main(void) {
    fn_1_9CB0(0);
    assert(copies == 0);
    started = 1;
    fn_1_9CB0(0);
    assert(copies == 1 && pixels[0] == 1);
    fn_1_9CB0(0);
    assert(copies == 2 && pixels[0] == 2);
    started = 0;
    fn_1_9CB0(0);
    assert(copies == 2 && pixels[0] == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="inst-movie-", dir=ROOT / "build") as tmp:
            source = Path(tmp) / "probe.c"
            source.write_text(probe)
            executable = Path(tmp) / "probe.exe"
            result = subprocess.run(
                [build.ZIG, "cc", "-UNDEBUG", "-O2", str(source), "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pointer_and_thp_failure_contracts(self):
        setup = self.patched("src/REL/instDll/setup.c")
        display = self.patched("src/REL/instDll/display.c")
        name = self.patched("src/REL/instDll/name.c")
        for source in (setup, display):
            self.assertNotIn("(s32)process->property", source)
            self.assertIn("(s32)(intptr_t)process->property", source)
        self.assertIn("(void *)(intptr_t)processState", display)
        # The glyph table stores 32-bit DATA numbers/flags; the native-width
        # objects are the separately allocated animation-pointer array.
        self.assertRegex(name, r"\bu32\s+lbl_1_data_F8\s*\[")
        self.assertRegex(name, r"\bu32\s+\*glyphDataByCode")
        self.assertRegex(name, r"\bu32\s+glyphDataAndOffset")
        self.assertIn("ANIMDATA **glyphAnimation", name)
        self.assertIn("100 * sizeof(*glyphAnimation)", name)
        self.assertIn("100 * sizeof(*glyphXOffset)", name)
        self.assertIn("100 * sizeof(*glyphYOffset)", name)
        self.assertNotRegex(name, r"HuMemDirectMallocNum\([^\n]*\b(?:400|800)\b")

        display_guards = re.findall(
            r"if \(movie != HUSPR_NONE\)\s*\{([^}]*)\}", display, re.S)
        self.assertEqual(len(display_guards), 2)
        for guard in display_guards:
            self.assertIn("HuSprGrpMemberSet", guard)
            self.assertIn("HuSprPosSet", guard)
        framebuffer_guard = re.search(
            r"if \(HuTHPStartCheck\(\)\)\s*\{([^}]*)\}", display, re.S)
        self.assertIsNotNone(framebuffer_guard)
        self.assertIn("mp6_thp_copy_frame_rgba8", framebuffer_guard.group(1))
        self.assertNotIn("Hu3DFbCopyExec", framebuffer_guard.group(1))

        thp = self.patched("src/game/thpmain.c")
        for function_name, sentinel in (
            ("HuTHPSprCreateVol", "return HUSPR_NONE;"),
            ("HuTHP3DCreateVol", "return HU3D_MODELID_NONE;"),
        ):
            body = function_body(thp, function_name)
            self.assertEqual(body.count("THPSimpleInit(2)"), 1, function_name)
            self.assertIn("if (!THPSimpleInit(2))", body)
            self.assertIn(sentinel, body)
            self.assertLess(body.index("if (!THPSimpleInit(2))"),
                            body.index("HuPrcCreate"), function_name)

    def test_each_inst_unit_has_reload_reset_run_by_constructors(self):
        for unit in ("setup", "stage", "display", "name"):
            source = self.patched(f"src/REL/instDll/{unit}.c")
            self.assertIn(f"MP6_REL_RESET_DEFINE(mp6_inst_{unit}_reset,", source)
        aggregate = function_body(self.patched("src/REL/instDll/setup.c"), "mp6_inst_reset")
        for unit in ("setup", "stage", "display", "name"):
            self.assertIn(f"mp6_inst_{unit}_reset();", aggregate)
        # The module's own prolog runs its constructors, which are the reset.
        prolog = self.patched("src/REL/instDll/prolog.c")
        self.assertIsNotNone(re.search(r"while \(\*\w+ != 0\)", prolog))
        bridge = (ROOT / "src/os/dll_bridge.c").read_text()
        self.assertIn("#define MP6_INST_CTORS { mp6_inst_reset, 0 }", bridge)

    def test_five_inst_units_compile_as_native_objects(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            patched = Path(temporary) / "patched"
            apply_patches.apply_all(out_dir=str(patched))
            for rel, flags in native_inst.sources(self.decomp):
                # A unit with no patch or reset block compiles from the pin.
                source = patched / rel if (patched / rel).exists() else self.decomp / rel
                obj = Path(temporary) / (Path(rel).stem + ".o")
                result = subprocess.run([
                    build.ZIG, "cc", "-I", str(patched / "include"),
                    *build.COMMON_FLAGS, *flags,
                    "-Werror=implicit-function-declaration",
                    "-Werror=incompatible-pointer-types", "-Werror=int-conversion",
                    "-c", str(source), "-o", str(obj),
                ], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0,
                                 f"{rel} failed native compilation:\n{result.stderr}")
                self.assertTrue(obj.is_file())

    def test_actual_reset_code_restores_data_and_zeros_bss(self):
        """Execute every recovered reset against its real TU-owned storage."""
        units = ("setup", "stage", "display", "name")
        rels = {
            unit: f"src/REL/instDll/{unit}.c"
            for unit in units
        }
        flags_by_unit = {
            Path(rel).stem: flags
            for rel, flags in native_inst.sources(self.decomp)
        }

        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            patched = temporary / "patched"
            apply_patches.apply_all(out_dir=str(patched))
            objects = []
            owner_sets = {}
            for unit in units:
                rel = rels[unit]
                source = (patched / rel).read_text()
                data, bss = reset_owner_names(source, unit)
                owner_sets[unit] = (data, bss)
                self.assertTrue(data or bss, unit)
            self.assertIn("lbl_1_data_4", owner_sets["setup"][0])
            self.assertIn("lbl_1_data_798", owner_sets["display"][0])

            for unit in units:
                rel = rels[unit]
                source_path = patched / rel
                source = source_path.read_text()
                first_function = re.search(
                    r"(?m)^[^\n;{}]*\bfn_1_[A-Za-z0-9_]+\([^;{}]*\)\s*\{",
                    source)
                self.assertIsNotNone(first_function, unit)
                data, bss = owner_sets[unit]
                lines = [
                    "#include <stddef.h>",
                    "#include <string.h>",
                    source[:first_function.start()],
                    reset_block(unit).replace("void mp6_inst_reset(void)", "static void unused_aggregate(void)"),
                    "",
                    "static int bytes_zero(const void *ptr, size_t size)",
                    "{",
                    "    const unsigned char *bytes = (const unsigned char *)ptr;",
                    "    while (size != 0) {",
                    "        if (*bytes++ != 0) return 0;",
                    "        --size;",
                    "    }",
                    "    return 1;",
                    "}",
                    "",
                ]
                for name in data:
                    lines.append(
                        f"static unsigned char baseline_{name}[sizeof({name})];")
                lines.extend((
                    "",
                    f"int mp6_inst_{unit}_reset_probe(void)",
                    "{",
                    "    int failures = 0;",
                    f"    mp6_inst_{unit}_reset();",
                ))
                for name in data:
                    lines.append(
                        f"    memcpy(baseline_{name}, &{name}, sizeof({name}));")
                for name in data + bss:
                    lines.append(f"    memset(&{name}, 0xA5, sizeof({name}));")
                lines.append(f"    mp6_inst_{unit}_reset();")
                for name in data:
                    lines.append(
                        f"    failures |= memcmp(baseline_{name}, &{name}, "
                        f"sizeof({name})) != 0;")
                for name in bss:
                    lines.append(
                        f"    failures |= !bytes_zero(&{name}, sizeof({name}));")
                lines.extend((
                    "    return failures;",
                    "}",
                    "",
                ))
                wrapper = temporary / f"{unit}_reset_probe.c"
                wrapper.write_text("\n".join(lines), encoding="utf-8")
                obj = temporary / f"{unit}_reset_probe.o"
                result = subprocess.run([
                    build.ZIG, "cc", "-I", str(patched / "include"),
                    *build.COMMON_FLAGS, *flags_by_unit[unit],
                    "-ffunction-sections", "-fdata-sections",
                    "-c", str(wrapper), "-o", str(obj),
                ], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0,
                                 f"{unit} reset harness failed to compile:\n{result.stderr}")
                objects.append(obj)

            main = temporary / "reset_probe_main.c"
            main.write_text(
                "#include <stdio.h>\n"
                "int mp6_inst_setup_reset_probe(void);\n"
                "int mp6_inst_stage_reset_probe(void);\n"
                "int mp6_inst_display_reset_probe(void);\n"
                "int mp6_inst_name_reset_probe(void);\n"
                "int main(void) {\n"
                "    int failures = 0;\n"
                "    failures |= mp6_inst_setup_reset_probe();\n"
                "    failures |= mp6_inst_stage_reset_probe();\n"
                "    failures |= mp6_inst_display_reset_probe();\n"
                "    failures |= mp6_inst_name_reset_probe();\n"
                "    if (failures) printf(\"reset failures: %d\\n\", failures);\n"
                "    return failures != 0;\n"
                "}\n",
                encoding="utf-8",
            )
            executable = temporary / "reset_probe.exe"
            result = subprocess.run([
                build.ZIG, "cc", *build.COMMON_FLAGS,
                "-Wl,--gc-sections", *(str(obj) for obj in objects),
                str(main), "-o", str(executable),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0,
                             f"reset harness failed to link:\n{result.stderr}")
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0,
                             f"reset harness reported a mismatch:\n{result.stdout}\n{result.stderr}")

    def test_display_hook_executes_live_wide_geometry_and_jitter(self):
        """Execute the patched backdrop hook against recorded GX calls."""
        source = self.patched("src/REL/instDll/display.c")
        body = function_definition_body(source, "fn_1_902C")
        call_names = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body))
        call_names -= {"if", "for", "while", "switch", "fn_1_902C"}
        typed = {
            "C_MTXOrtho", "GXSetProjection", "GXSetViewport",
            "GXSetViewportJitter", "GXSetScissor", "GXBegin",
            "GXPosition2f32", "GXTexCoord2f32", "GXEnd",
            "PSMTXIdentity", "PSMTXTrans", "GXLoadPosMtxImm",
            "GXLoadTexMtxImm", "VIGetNextField",
            "mp6_widescreen_half_width_delta", "mp6_widescreen_render_width",
        }
        generic_names = sorted(call_names - typed)
        macro_names = sorted(set(re.findall(r"\b[A-Z][A-Z0-9_]*\b", body)))
        macro_definitions = "\n".join(
            f"#ifndef {name}\n#define {name} 0\n#endif"
            for name in macro_names
        )
        generic_declarations = "\n".join(
            f"void {name}();" for name in generic_names
        )
        generic_definitions = "\n".join(
            f"void {name}() {{ }}" for name in generic_names
        )
        harness = r'''#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef float f32;
typedef unsigned int u32;
typedef short s16;
typedef unsigned char u8;
typedef float Mtx[3][4];
typedef float Mtx44[4][4];
typedef struct { u8 r, g, b, a; } GXColor;
typedef struct { int field_rendering; } RenderModeStub;
static RenderModeStub render_mode;
RenderModeStub *RenderMode = &render_mode;
static f32 lbl_1_bss_48;
static int lbl_1_bss_6E;
static int lbl_1_bss_8;
static void *lbl_1_bss_70[2];
static u32 test_render_width;
static int test_field;
static int ortho_calls, projection_calls;
static f32 ortho_top, ortho_bottom, ortho_left, ortho_right;
static int projection_type;
static int viewport_calls, jitter_calls, jitter_field;
static f32 viewport_width, viewport_height;
static u32 scissor_width, scissor_height;
static int begin_calls, begin_vertices, end_calls;
static int position_count, texcoord_count;
static f32 position_x[4], position_y[4];
static f32 texcoord_s[4], texcoord_t[4];
static f32 texture_translation_x, texture_translation_y;
static int texture_translation_calls;
static f32 phase_translation;
static int phase_captured;

f32 mp6_widescreen_half_width_delta(void);
u32 mp6_widescreen_render_width(void);
int VIGetNextField(void);
void C_MTXOrtho(Mtx44, f32, f32, f32, f32, f32, f32);
void GXSetProjection(Mtx44, int);
void GXSetViewport(f32, f32, f32, f32, f32, f32);
void GXSetViewportJitter(f32, f32, f32, f32, f32, f32, int);
void GXSetScissor(int, int, u32, u32);
void GXBegin(int, int, int);
void GXPosition2f32(f32, f32);
void GXTexCoord2f32(f32, f32);
void GXEnd(void);
void PSMTXIdentity(Mtx);
void PSMTXTrans(Mtx, f32, f32, f32);
void GXLoadPosMtxImm(Mtx, int);
void GXLoadTexMtxImm(Mtx, int, int);
''' + macro_definitions + r'''
#ifndef GX_MTX2x4
#define GX_MTX2x4 0
#endif
#ifndef GX_TG_MTX2x4
#define GX_TG_MTX2x4 0
#endif
''' + generic_declarations + "\n" + body + r'''

f32 mp6_widescreen_half_width_delta(void) {
    return ((f32)test_render_width - 640.0f) * 0.5f;
}
u32 mp6_widescreen_render_width(void) { return test_render_width; }
int VIGetNextField(void) { return test_field; }
void C_MTXOrtho(Mtx44 matrix, f32 top, f32 bottom, f32 left, f32 right,
                f32 nearz, f32 farz) {
    (void)matrix; (void)nearz; (void)farz;
    ++ortho_calls;
    ortho_top = top; ortho_bottom = bottom;
    ortho_left = left; ortho_right = right;
}
void GXSetProjection(Mtx44 matrix, int type) {
    (void)matrix; ++projection_calls; projection_type = type;
}
void GXSetViewport(f32 x, f32 y, f32 width, f32 height, f32 nearz, f32 farz) {
    (void)x; (void)y; (void)nearz; (void)farz;
    ++viewport_calls; viewport_width = width; viewport_height = height;
}
void GXSetViewportJitter(f32 x, f32 y, f32 width, f32 height, f32 nearz,
                         f32 farz, int field) {
    (void)x; (void)y; (void)nearz; (void)farz;
    ++jitter_calls; jitter_field = field;
    viewport_width = width; viewport_height = height;
}
void GXSetScissor(int x, int y, u32 width, u32 height) {
    (void)x; (void)y; scissor_width = width; scissor_height = height;
}
void GXBegin(int primitive, int format, int vertices) {
    (void)primitive; (void)format; ++begin_calls; begin_vertices = vertices;
    position_count = 0; texcoord_count = 0;
}
void GXPosition2f32(f32 x, f32 y) {
    if (position_count < 4) {
        position_x[position_count] = x; position_y[position_count] = y;
    }
    ++position_count;
}
void GXTexCoord2f32(f32 s, f32 t) {
    if (texcoord_count < 4) {
        texcoord_s[texcoord_count] = s; texcoord_t[texcoord_count] = t;
    }
    ++texcoord_count;
}
void GXEnd(void) { ++end_calls; }
void PSMTXIdentity(Mtx matrix) { memset(matrix, 0, sizeof(Mtx)); }
void PSMTXTrans(Mtx matrix, f32 x, f32 y, f32 z) {
    (void)matrix; (void)z;
    texture_translation_x = x; texture_translation_y = y;
    ++texture_translation_calls;
}
void GXLoadPosMtxImm(Mtx matrix, int id) { (void)matrix; (void)id; }
void GXLoadTexMtxImm(Mtx matrix, int id, int type) {
    (void)matrix; (void)id; (void)type;
}
''' + generic_definitions + r'''

static int near(f32 a, f32 b) { return fabsf(a - b) < 0.0001f; }
static int fail(const char *message) {
    printf("FAIL: %s\n", message);
    return 1;
}
static int run_case(u32 width, int field_rendering) {
    const f32 margin = ((f32)width - 640.0f) * 0.5f;
    const f32 left = -margin;
    const f32 right = 576.0f + margin;
    const f32 expected_x[4] = { left, right, right, left };
    const f32 expected_y[4] = { 0.0f, 0.0f, 480.0f, 480.0f };
    const f32 expected_s[4] = { left / 64.0f, right / 64.0f,
                                right / 64.0f, left / 64.0f };
    const f32 expected_t[4] = { 0.0f, 0.0f, 7.5f, 7.5f };
    int i;

    test_render_width = width;
    test_field = field_rendering ? 7 : 0;
    render_mode.field_rendering = field_rendering;
    lbl_1_bss_48 = 0.37f;
    lbl_1_bss_6E = 0;
    lbl_1_bss_8 = 0;
    lbl_1_bss_70[0] = (void *)(uintptr_t)1;
    lbl_1_bss_70[1] = (void *)(uintptr_t)2;
    ortho_calls = projection_calls = 0;
    viewport_calls = jitter_calls = 0;
    begin_calls = end_calls = 0;
    texture_translation_calls = 0;
    fn_1_902C(0);

    if (ortho_calls != 1 || projection_calls != 1) return fail("projection call count");
    if (!near(ortho_top, 0.0f) || !near(ortho_bottom, 480.0f) ||
        !near(ortho_left, left) || !near(ortho_right, right))
        return fail("ortho bounds");
    if (viewport_calls != (field_rendering ? 0 : 1) ||
        jitter_calls != (field_rendering ? 1 : 0) ||
        !near(viewport_width, (f32)width) || !near(viewport_height, 480.0f))
        return fail("viewport or jitter path");
    if (field_rendering && jitter_field != 7)
        return fail("field jitter input");
    if (scissor_width != width || scissor_height != 480)
        return fail("scissor bounds");
    if (begin_calls != 1 || begin_vertices != 4 || end_calls != 1 ||
        position_count != 4 || texcoord_count != 4)
        return fail("quad submission count");
    for (i = 0; i < 4; ++i) {
        if (!near(position_x[i], expected_x[i]) ||
            !near(position_y[i], expected_y[i]) ||
            !near(texcoord_s[i], expected_s[i]) ||
            !near(texcoord_t[i], expected_t[i]))
            return fail("quad or UV bounds");
    }
    if (!near((right - left) / (expected_s[1] - expected_s[0]), 64.0f))
        return fail("logical tile density");
    if (texture_translation_calls != 1 ||
        !near(texture_translation_x, -0.37f) ||
        !near(texture_translation_y, -0.37f))
        return fail("center phase");
    if (!phase_captured) {
        phase_translation = texture_translation_x;
        phase_captured = 1;
    } else if (!near(phase_translation, texture_translation_x)) {
        return fail("center phase changed with width");
    }
    if (!near(lbl_1_bss_48, 0.375f)) return fail("animation phase update");
    return 0;
}
int main(void) {
    int failures = 0;
    failures |= run_case(640, 0);
    failures |= run_case(640, 1);
    failures |= run_case(848, 0);
    failures |= run_case(848, 1);
    if (failures) return 1;
    puts("PASS: live backdrop geometry, tile density, phase, and jitter");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source_path = temporary / "display_hook_probe.c"
            executable = temporary / "display_hook_probe.exe"
            source_path.write_text(harness, encoding="utf-8")
            result = subprocess.run([
                build.ZIG, "cc", *build.COMMON_FLAGS,
                "-Wno-unused-parameter", str(source_path), "-o", str(executable),
            ], cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0,
                             f"display hook harness failed to compile:\n{result.stderr}")
            result = subprocess.run([str(executable)], cwd=ROOT,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0,
                             f"display hook probe failed:\n{result.stdout}\n{result.stderr}")
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
