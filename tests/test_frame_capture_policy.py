"""Exercise the real scheduler policy and capture lifecycle across live changes."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from test_frame_replay_batch import replay_test_source

ROOT = Path(__file__).resolve().parents[1]


class FrameCapturePolicy(unittest.TestCase):
    def run_subject(self, old=False):
        with tempfile.TemporaryDirectory(prefix='fi-policy-', dir=ROOT/'build') as temp:
            folder = Path(temp)
            bridge = (ROOT/'src/gx/aurora_bridge.c').read_text()
            scheduler = bridge[bridge.index('#define MP6_TICK_HZ_DEFAULT'):
                               bridge.index('/* Scheduler-lateness stats')]
            (folder/'scheduler-under-test.inc').write_text(scheduler)
            source = replay_test_source()
            if old:
                source = source.replace(' && mp6_tick_interpolation_possible()', '')
            (folder/'replay-under-test.inc').write_text(source)
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG',
                '-I'+str(folder), '-I'+str(ROOT/'include'), '-I'+str(ROOT/'src/host'),
                '-I'+str(ROOT/'build/android-aurora-source/include'),
                str(ROOT/'tests/native/frame_capture_policy_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)

    def test_scheduler_and_capture_transitions(self):
        result = self.run_subject()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_old_free_run_capture_fails(self):
        result = self.run_subject(old=True)
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn('Assertion failed', result.stderr)


if __name__ == '__main__':
    unittest.main()
