"""Test the actual snapshot, camera-cut and reset functions without the engine."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


class ReplayCameraCache(unittest.TestCase):
    def test_snapshots_cuts_invalid_cameras_and_reset(self):
        source = (ROOT/'src/hsf/mp6_fi_model.c').read_text()
        definitions = source[source.index('#define FI_CAM_TRANSLATION_CUT'):
                             source.index('/* Must match hsfdraw.c')]
        state = source[source.index('static FiCamera s_camPrev'):
                       source.index('static uint32_t next_generation')]
        functions = source[source.index('void mp6_fi_model_snapshot'):
                           source.index('static int animlog_enabled')]
        functions = functions.replace('static int fi_camera_stable_uncached(int camera_id)\n{',
            'static int fi_camera_stable_uncached(int camera_id)\n{\n    ++calculations;')
        with tempfile.TemporaryDirectory(prefix='fi-camera-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            (folder/'camera-under-test.inc').write_text(definitions+state+functions)
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG', '-I'+str(folder),
                str(ROOT/'tests/native/replay_camera_cache_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
