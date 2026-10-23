"""Exercise Mount Duel's actual patched native contracts, not a Python model."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build, native_modes
from tests.test_native_modes import (
    execute_preview_control, execute_preview_draw, execute_reset_owners,
    function_body,
)

ROOT = Path(__file__).resolve().parents[1]
MODULE = "mgmtournamentdll"
UNITS = ("bracket", "camera", "characters", "host", "math", "preview", "scene", "window")
OWNERS = tuple(unit for unit in UNITS if unit != "math")


def patched(decomp, unit):
    relative = f"src/REL/{MODULE}/{unit}.c"
    return apply_patches.generated_source(decomp, relative)


class NativeTournament(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())

    def test_explicit_inventory_and_complete_storage_owners(self):
        self.assertEqual(native_modes.MODULES[MODULE], UNITS)
        selected = [(path, flags) for path, flags in native_modes.sources(self.decomp)
                    if Path(path).parent.name == MODULE]
        self.assertEqual([Path(path).stem for path, _ in selected], list(UNITS))
        flags = native_modes.namespace_flags(self.decomp, MODULE)
        self.assertIn("-D_prolog=mgmtournament_prolog", flags)
        self.assertIn("-D_dtors=mgmtournament_dtors", flags)
        for unit in OWNERS:
            source = patched(self.decomp, unit)
            globals_ = set(re.findall(
                r"(?m)^[A-Za-z_]\w*[ \t*]+(lbl_1_(?:bss|data)_\w+)", source))
            listed = re.findall(r"\bX\((lbl_1_(?:bss|data)_\w+)\)", source)
            self.assertEqual(globals_, set(listed), unit)
            self.assertEqual(len(listed), len(set(listed)), unit)
        bridge = (ROOT / "src/os/dll_bridge.c").read_text(encoding="utf-8")
        self.assertIn('X(mgmtournament, "dll/mgmtournamentdll.rel", mgmtournament_prolog, '
                      'mgmtournament_epilog, "mgmtournamentdll", MGMTOURNAMENT, '
                      'MP6_MGMTOURNAMENT_CTORS)', bridge)

    def test_real_reset_owners_and_aggregate_dispatch(self):
        execute_reset_owners(self, self.decomp, MODULE, OWNERS, "scene")

    def test_preview_draw_executes_balanced_native_primitives(self):
        execute_preview_draw(self, patched(self.decomp, "preview"))

    def test_preview_fallback_and_centered_capture_execute(self):
        execute_preview_control(self, patched(self.decomp, "preview"))

    def test_native_text_pointers_and_preview_cycle_bounds(self):
        preview = patched(self.decomp, "preview")
        window = patched(self.decomp, "window")
        subject = "\n\n".join([
            function_body(window, "fn_1_199C"),
            *(function_body(preview, name) for name in
              ("fn_1_2A4", "fn_1_43C", "fn_1_4F8")),
        ])
        fixture = (ROOT / "tests/native/tournament_native_selftest.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source = temporary / "tournament_probe.c"
            executable = temporary / "tournament_probe.exe"
            source.write_text(fixture.replace("/* TOURNAMENT_NATIVE_SUBJECT */", subject),
                              encoding="utf-8")
            result = subprocess.run([
                build.ZIG, "cc", *build.COMMON_FLAGS,
                "-I", str(ROOT / "include"), "-I", str(self.decomp / "include"),
                "-UNDEBUG", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                "-Werror=incompatible-pointer-types", "-Werror=int-conversion",
                "-Werror=pointer-to-int-cast", "-Werror=int-to-pointer-cast",
                str(source), "-o", str(executable),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
