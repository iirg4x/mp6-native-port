"""Compile and run the native HSF tile-link probe."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]


class WidescreenTile(unittest.TestCase):
    def test_native_tile_links_phase_visibility_and_teardown(self):
        source = (ROOT / "src/hsf/mp6_widescreen_tile.c").read_text()
        for allocator in ("malloc", "calloc", "free"):
            self.assertNotIn(allocator, source)

        with tempfile.TemporaryDirectory(prefix="wide-tile-", dir=ROOT / "build") as tmp:
            exe = Path(tmp) / "widescreen_tile_selftest.exe"
            command = [
                build.ZIG,
                "cc",
                *build.COMMON_FLAGS,
                "-UNDEBUG",
                "-O2",
                "-I" + str(ROOT / "include"),
                str(ROOT / "tests/native/widescreen_tile_selftest.c"),
                str(ROOT / "src/hsf/mp6_widescreen_tile.c"),
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
                self.fail(f"native tile probe compiler exceeded 60s: {error}")
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
                self.fail(f"native tile probe exceeded 10s: {error}")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("widescreen tile native probe: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
