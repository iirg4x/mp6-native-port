"""Exercise the real canyon haze draw body and its emitted margin geometry."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_native_modes import function_body

ROOT = Path(__file__).resolve().parents[1]


class S01Haze(unittest.TestCase):
    def test_gradual_bounded_opacity_never_draws_in_native_canvas(self):
        relative = 'src/REL/s01Dll/s01.c'
        source = (Path(apply_patches.require_pinned_decomp()) / relative).read_text()
        source = apply_patches.apply_unified_diff(source,
            (ROOT / 'compat/decomp' / (relative + '.patch')).read_text())
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            (directory / 's01_haze_subject.inc').write_text(
                function_body(source, 'mp6_s01_side_haze'))
            exe = directory / 's01-haze.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS,
                '-I' + str(directory), '-O2', '-UNDEBUG',
                'tests/native/s01_haze_selftest.c', '-o', str(exe)],
                cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('S01 gradual side-only haze: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
