"""Compile and run the native HSF widescreen-motion probe."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]


class WidescreenMotion(unittest.TestCase):
    def test_native_motion_snapshot_and_scale_reapply(self):
        """Use the real helper and HSF ABI, with one bounded compiler job."""

        with tempfile.TemporaryDirectory(prefix="wide-motion-", dir=ROOT / "build") as tmp:
            exe = Path(tmp) / "widescreen_motion_selftest.exe"
            command = [
                build.ZIG,
                "cc",
                *build.COMMON_FLAGS,
                "-UNDEBUG",
                "-O2",
                "-I" + str(ROOT / "include"),
                str(ROOT / "tests/native/widescreen_motion_selftest.c"),
                str(ROOT / "src/hsf/mp6_widescreen_motion.c"),
                "-o",
                str(exe),
            ]
            try:
                compiled = subprocess.run(
                    command,
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=60,
                )
            except subprocess.TimeoutExpired as error:
                self.fail(f"native motion probe compiler exceeded 60s: {error}")
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

            try:
                result = subprocess.run(
                    [str(exe)],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
            except subprocess.TimeoutExpired as error:
                self.fail(f"native motion probe exceeded 10s: {error}")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("widescreen motion native probe: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
