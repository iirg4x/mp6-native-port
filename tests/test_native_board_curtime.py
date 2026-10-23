"""Exercise board day/night callbacks against the real GW_SYSTEM layout."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_native_modes import function_body


ROOT = Path(__file__).resolve().parents[1]


class NativeBoardCurTime(unittest.TestCase):
    def test_board_callbacks_use_named_cur_time_for_lights_and_events(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        modules = (
            ("w03Dll", ("fn_1_1BB8",)),
            ("w04Dll", ("fn_1_D3C",)),
            ("w05Dll", ("fn_1_2010", "fn_1_20F8")),
        )
        bodies = []
        for module, names in modules:
            relative = f"src/REL/{module}/board.c"
            source = (decomp / relative).read_text(encoding="utf-8")
            patch = ROOT / "compat/decomp" / (relative + ".patch")
            source = apply_patches.apply_unified_diff(
                source, patch.read_text(encoding="utf-8"), relative
            )
            for name in names:
                body = function_body(source, name)
                self.assertNotIn("((u8 *)", body)
                self.assertIn("GwSystem.curTime", body)
                bodies.append(body)

        fixture = (ROOT / "tests/native/board_curtime_selftest.c").read_text(encoding="utf-8")
        fixture = fixture.replace("/* BOARD_CURTIME_NATIVE_SUBJECT */", "\n\n".join(bodies))
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source_path = temporary / "board_curtime_probe.c"
            executable = temporary / "board_curtime_probe.exe"
            source_path.write_text(fixture, encoding="utf-8")
            result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    *build.COMMON_FLAGS,
                    "-UNDEBUG",
                    "-fsanitize=undefined",
                    "-fno-sanitize-recover=all",
                    str(source_path),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
