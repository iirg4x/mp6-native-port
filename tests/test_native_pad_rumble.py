"""Exercise the real PadReadVSync rumble body with a zero-length cycle."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_native_modes import function_body


ROOT = Path(__file__).resolve().parents[1]


class NativePadRumble(unittest.TestCase):
    def test_zero_cycle_does_not_fault_and_normal_cycle_still_phases(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        relative = "src/game/pad.c"
        source = (decomp / relative).read_text(encoding="utf-8")
        patch = ROOT / "compat/decomp" / (relative + ".patch")
        source = apply_patches.apply_unified_diff(
            source, patch.read_text(encoding="utf-8"), relative
        )
        body = function_body(source, "PadReadVSync")
        self.assertIn("rumbleState->offTime + rumbleState->onTime != 0", body)

        fixture = (ROOT / "tests/native/pad_rumble_selftest.c").read_text(
            encoding="utf-8"
        )
        fixture = fixture.replace("/* PAD_RUMBLE_NATIVE_SUBJECT */", body)
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source_path = temporary / "pad_rumble_probe.c"
            executable = temporary / "pad_rumble_probe.exe"
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
