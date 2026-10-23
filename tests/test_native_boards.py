"""Native inventory, strict compilation, and reset ownership for board RELs."""

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

from tools import apply_patches, build, native_boards
from tests.test_native_modes import strip_native_function_bodies


ROOT = Path(__file__).resolve().parents[1]
BOARD_SPECS = {
    "w03Dll": {"prefix": "w03", "units": ("board", "prolog")},
    "w04Dll": {"prefix": "w04", "units": ("board", "prolog")},
}


def patched_source(decomp, module, unit):
    """Return the actual pinned source after its authored compatibility patch."""
    relative = f"src/REL/{module}/{unit}.c"
    original = (Path(decomp) / relative).read_text(encoding="utf-8")
    patch = ROOT / "compat/decomp" / (relative + ".patch")
    if patch.exists():
        original = apply_patches.apply_unified_diff(original, patch.read_text(encoding="utf-8"), relative)
    return apply_patches.with_rel_state(original, relative)


def _reset_probe_source(source, prefix):
    """Keep the real board globals/reset and remove unrelated engine bodies."""
    upper = prefix.upper()
    reset_name = f"mp6_{prefix}_reset"
    data_macro = f"MP6_{upper}_BOARD_DATA"
    bss_macro = f"MP6_{upper}_BOARD_BSS"
    # W04's curve-function header has no storage declarations. Exclude its
    # unrelated bodies from this reset-only executable; strict compilation
    # above still checks the complete production board and included helpers.
    source = source.replace('#include "REL/w04Dll/common.h"', '')
    keep = [f"mp6_{prefix}_board_reset", f"mp6_{prefix}_reset"]
    if prefix == "w04":
        keep.append("fn_1_4ACC")
    source = strip_native_function_bodies(
        source,
        keep=keep,
    )
    teardown_stub = r'''
void *probe_freed_pointer;
int probe_free_calls;
void HuMemDirectFree(void *pointer)
{
    probe_freed_pointer = pointer;
    probe_free_calls++;
}
''' if prefix == "w04" else ""
    teardown_check = r'''
    {
        void *unrelated = (void *)(uintptr_t)0x4321;
        lbl_1_bss_7248 = (void *)(uintptr_t)0x1234;
        lbl_1_bss_7244 = 7;
        lbl_1_bss_726C = unrelated;
        probe_freed_pointer = NULL;
        probe_free_calls = 0;
        fn_1_4ACC();
        if (probe_free_calls != 1 || probe_freed_pointer != (void *)(uintptr_t)0x1234 ||
            lbl_1_bss_7248 != NULL || lbl_1_bss_7244 != 0 ||
            lbl_1_bss_726C != unrelated) {
            failures = 1;
        }
        fn_1_4ACC();
        if (probe_free_calls != 1 || lbl_1_bss_7248 != NULL || lbl_1_bss_7244 != 0 ||
            lbl_1_bss_726C != unrelated) {
            failures = 1;
        }
    }
''' if prefix == "w04" else ""
    probe = r'''
#include <stddef.h>
#include <stdint.h>
#include <string.h>

___TEARDOWN_STUB___
''' + source + r'''

#define MP6_JOIN_I(a, b) a##b
#define MP6_JOIN(a, b) MP6_JOIN_I(a, b)
#define MP6_DECLARE_DATA(n) unsigned char MP6_JOIN(saved_, n)[sizeof(n)];
#define MP6_SAVE_DATA(n) memcpy(MP6_JOIN(saved_, n), &(n), sizeof(n));
#define MP6_DIRTY(n) memset(&(n), 0xA5, sizeof(n));
#define MP6_CHECK_DATA(n) \
do { if (memcmp(MP6_JOIN(saved_, n), &(n), sizeof(n)) != 0) failures = 1; } while (0);
#define MP6_CHECK_BSS(n) \
do { const unsigned char *bytes = (const unsigned char *)&(n); \
     for (size_t i = 0; i < sizeof(n); ++i) if (bytes[i] != 0) failures = 1; } while (0);

int main(void)
{
    int failures = 0;
    ___RESET___();
    ___DATA___(MP6_DECLARE_DATA)
    ___DATA___(MP6_SAVE_DATA)
    ___DATA___(MP6_DIRTY)
    ___BSS___(MP6_DIRTY)
    ___RESET___();
    ___DATA___(MP6_CHECK_DATA)
    ___BSS___(MP6_CHECK_BSS)
___TEARDOWN_CHECK___
    return failures;
}
'''
    return probe.replace("___TEARDOWN_STUB___", teardown_stub).replace(
        "___TEARDOWN_CHECK___", teardown_check
    ).replace("___RESET___", reset_name).replace("___DATA___", data_macro).replace(
        "___BSS___", bss_macro
    )


class NativeBoards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())

    def test_import_is_lazy_and_board_inventories_are_exact(self):
        """Importing the selector must not touch the dependency checkout."""
        env = os.environ.copy()
        env["PYTHONPATH"] = str(ROOT)
        result = subprocess.run(
            [
                sys.executable,
                "-c",
                "from pathlib import Path; "
                "reject = lambda *args, **kwargs: (_ for _ in ()).throw("
                "AssertionError('dependency read during selector import')); "
                "Path.read_text = reject; Path.read_bytes = reject; "
                "Path.glob = reject; Path.rglob = reject; "
                "from tools import native_boards; "
                "assert native_boards.MODULES['w03Dll'] == ('board', 'prolog'); "
                "assert native_boards.MODULES['w04Dll'] == ('board', 'prolog')",
            ],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for module, spec in BOARD_SPECS.items():
            self.assertEqual(native_boards.MODULES[module], spec["units"])

        bridge = (ROOT / "src/os/dll_bridge.c").read_text(encoding="utf-8")
        all_sources = list(native_boards.sources(self.decomp))
        for module, spec in BOARD_SPECS.items():
            with self.subTest(module=module):
                units = spec["units"]
                selected = [
                    (path, flags)
                    for path, flags in all_sources
                    if Path(path).parent.name == module
                ]
                self.assertEqual(
                    [Path(path).stem for path, _ in selected], list(units)
                )
                self.assertTrue(
                    all(Path(path).parent.name == module for path, _ in selected)
                )
                flags = native_boards.namespace_flags(self.decomp, module)
                prefix = spec["prefix"]
                self.assertIn(f"-D_prolog={prefix}_prolog", flags)
                self.assertIn(f"-D_epilog={prefix}_epilog", flags)
                self.assertIn(f"-D_ctors={prefix}_ctors", flags)
                self.assertIn(f"-D_dtors={prefix}_dtors", flags)
                self.assertIn(f"{prefix}_prolog", bridge)
                self.assertIn(f"mp6_{prefix}_reset", bridge)

    def test_real_patched_storage_macros_cover_board_globals(self):
        for module, spec in BOARD_SPECS.items():
            with self.subTest(module=module):
                source = patched_source(self.decomp, module, "board")
                self.assertTrue(
                    (ROOT / f"compat/decomp/src/REL/{module}/board.c.patch").exists()
                )
                globals_ = set(
                    re.findall(
                        r"(?m)^[A-Za-z_]\w*[ \t*]+(lbl_1_(?:bss|data)_\w+)",
                        source,
                    )
                )
                listed = re.findall(
                    r"\bX\((lbl_1_(?:bss|data)_\w+)\)", source
                )
                self.assertEqual(globals_, set(listed))
                self.assertEqual(len(listed), len(set(listed)))
                prefix = spec["prefix"]
                upper = prefix.upper()
                self.assertIn(
                    f"MP6_REL_RESET_DEFINE(mp6_{prefix}_board_reset", source
                )
                self.assertIn(f"void mp6_{prefix}_reset(void)", source)

    def test_each_real_patched_board_unit_compiles_with_strict_native_abi(self):
        """Compile units one at a time; this test intentionally does not link/run."""
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            patched_root = temporary / "patched"
            apply_patches.apply_all(out_dir=str(patched_root))
            selected = list(native_boards.sources(self.decomp))
            for relative, flags in selected:
                module = Path(relative).parent.name
                if module not in BOARD_SPECS:
                    continue
                source = patched_root / relative
                if not source.is_file():
                    source = self.decomp / relative
                output = temporary / (
                    f"{BOARD_SPECS[module]['prefix']}_{Path(relative).stem}.o"
                )
                result = subprocess.run(
                    [
                        build.ZIG,
                        "cc",
                        "-I", str(patched_root / "include"),
                        "-I", str(ROOT / "include"),
                        *[flag for flag in build.COMMON_FLAGS if flag != "-w"],
                        "-Werror=implicit-function-declaration",
                        "-Werror=incompatible-function-pointer-types",
                        "-Werror=incompatible-pointer-types",
                        "-Werror=int-conversion",
                        "-Werror=pointer-to-int-cast",
                        "-Werror=int-to-pointer-cast",
                        "-I",
                        str(patched_root / "include"),
                        "-I",
                        str(ROOT / "include"),
                        *flags,
                        "-c",
                        str(source),
                        "-o",
                        str(output),
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=60,
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    f"{relative} strict native compile failed:\n"
                    f"{result.stdout}\n{result.stderr}",
                )

    def test_real_board_resets_restore_data_and_clear_bss(self):
        """Execute only the actual patched reset owner, with no gameplay stubs."""
        all_sources = list(native_boards.sources(self.decomp))
        for module, spec in BOARD_SPECS.items():
            prefix = spec["prefix"]
            with self.subTest(module=module):
                source = _reset_probe_source(
                    patched_source(self.decomp, module, "board"), prefix
                )
                with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
                    temporary = Path(temporary)
                    probe = temporary / f"{prefix}_reset_probe.c"
                    executable = temporary / f"{prefix}_reset_probe.exe"
                    probe.write_text(source, encoding="utf-8")
                    flags_by_unit = {
                        Path(relative).stem: flags
                        for relative, flags in all_sources
                        if Path(relative).parent.name == module
                    }
                    result = subprocess.run(
                        [
                            build.ZIG,
                            "cc",
                            *[flag for flag in build.COMMON_FLAGS if flag != "-w"],
                            "-I",
                            str(ROOT / "include"),
                            "-I",
                            str(self.decomp / "include"),
                            *flags_by_unit["board"],
                            "-UNDEBUG",
                            "-fsanitize=undefined",
                            "-fno-sanitize-recover=all",
                            "-Werror=incompatible-pointer-types",
                            "-Werror=int-conversion",
                            "-Werror=pointer-to-int-cast",
                            "-Werror=int-to-pointer-cast",
                            str(probe),
                            "-o",
                            str(executable),
                        ],
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                        timeout=60,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run(
                        [str(executable)],
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_w02_native_views_share_work_and_sprite_pointer_layouts(self):
        """Execute real recovered types, not a Python model of PPC offsets."""
        source = patched_source(self.decomp, "w02Dll", "text_A0")
        source = source.split("\ns16 lbl_1_bss_5C;", 1)[0]
        source += r'''
#include <stddef.h>
#include <stdint.h>
#define SAME_FIELD(A, B, field) \
    _Static_assert(offsetof(A, field) == offsetof(B, field), "native pointer offset")
SAME_FIELD(SpriteRenderHook, EventSpriteHook, particles);
SAME_FIELD(SpriteRenderHook, SpriteHook, particles);
SAME_FIELD(SpriteRenderHook, spriteinitializeSpriteHook, particles);
SAME_FIELD(SpriteRenderHook, MarkerHook, particles);
SAME_FIELD(SpriteRenderHook, textSpriteHook, particles);
SAME_FIELD(SpriteRenderHook, TextureGridHook, tileCoordinates);
_Static_assert(offsetof(SpriteRenderHook, update) ==
               offsetof(EventSpriteHook, callbackAddress), "native callback width");
_Static_assert(offsetof(SpriteRenderHook, linkedModel) ==
               offsetof(EventSpriteHook, linkedModelAddress), "native model pointer");
int main(void)
{
    PositionModel work = {0};
    BoardEffects *effects = &work;
    StationWork *station = &work;
    StarDisplayWork *star = &work;
    effects->modelObject = (OMOBJ *)(uintptr_t)0x12345678;
    station->machineModels[2] = 42;
    star->starModel = 99;
    return work.modelObject != (OMOBJ *)(uintptr_t)0x12345678 ||
           work.eyeModel != 42 || work.starModel != 99;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            directory = Path(directory)
            probe, executable = directory / "layout.c", directory / "layout.exe"
            probe.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "cc", *[f for f in build.COMMON_FLAGS if f != "-w"],
                 "-I", str(ROOT / "include"), "-I", str(self.decomp / "include"),
                 *native_boards.namespace_flags(self.decomp, "w02Dll"),
                 "-Werror=incompatible-pointer-types", "-fsanitize=undefined",
                 "-fno-sanitize-recover=all", str(probe), "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
