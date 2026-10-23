"""Run Wrasslin' Rapids' collision query through the native ABI."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M664NativeAbi(unittest.TestCase):
    def test_collision_query_uses_complete_native_record(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            source = (patched / 'src/REL/m664dll/game.c').read_text(encoding='utf-8')
            self.assertIn('MGACTOR_COLMAP_POLY hitPolygon;', source)
            self.assertIn('-1U, &hitPolygon', source)
            self.assertNotIn('(MGACTOR_COLMAP_POLY *)&hitPolygon', source)
            subject = function_definition_body(source, 'fn_1_9704')
            (directory / 'm664_subject.inc').write_text(subject, encoding='utf-8')

            executable = directory / 'm664.exe'
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                *build.COMMON_FLAGS, '-I', str(directory), '-O1', '-UNDEBUG',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                str(ROOT / 'tests/native/m664_native_selftest.c'), '-lm',
                '-o', str(executable)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
