"""Execute the real replay builder against the per-command-copy reference."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


def replay_test_source():
    source = (ROOT/'src/gx/frame_interp.c').read_text()
    start = source.index('static uint32_t fi_build_replay(')
    end = source.index('\nint mp6_fi_idle_present(', start)
    reference = (ROOT/'tests/native/frame_replay_reference.inc').read_text()
    # No SDL/runtime calls are reachable from the builder under test.
    source = source[:end].replace('#include <SDL3/SDL.h>', '// SDL not needed by the isolated builder')
    source=source.replace('    const uint8_t b = data[pos];',
                          '    ++test_walk_calls;\n    const uint8_t b = data[pos];')
    source = source.replace('    FiMtx a;\n    double dx, dy, dz;',
                            '    ++test_prepare_calls;\n    FiMtx a;\n    double dx, dy, dz;')
    source = source.replace('    if (need <= *cap) return 1;',
        '    if (test_cache_grow_fail && (buf == (void **)&s_pairCache || '
        'buf == (void **)&s_pairState)) return 0;\n    if (need <= *cap) return 1;')
    reset = (ROOT/'src/gx/frame_interp.c').read_text().split('void mp6_fi_savestate_reset(void)', 1)[1]
    return ('static unsigned test_walk_calls, test_prepare_calls;\n'
            'static int test_cache_grow_fail;\n'+source+'\n'+reference+
            '\nvoid mp6_fi_savestate_reset(void)'+reset)


class FrameReplayBatch(unittest.TestCase):
    def test_replay_bytes_markers_and_matrix_rewrites(self):
        self.check_replays(4096)

    def test_bounded_cache_overflow_uses_exact_fallback(self):
        self.check_replays(16)

    def check_replays(self, limit):
        with tempfile.TemporaryDirectory(prefix='fi-copy-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            source = replay_test_source().replace('#define FI_PAIR_CACHE_MAX 4096u',
                                                  f'#define FI_PAIR_CACHE_MAX {limit}u')
            (folder/'replay-under-test.inc').write_text(source)
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG', '-ffunction-sections',
                '-fdata-sections', '-Wl,--gc-sections', '-I'+str(folder), '-I'+str(ROOT/'include'),
                '-I'+str(ROOT/'src/host'),
                '-I'+str(ROOT/'build/android-aurora-source/include'),
                str(ROOT/'tests/native/frame_replay_batch_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            import os
            for policy in ({}, {'MP6_FI_NO_SCALE_HOLD': '1'},
                           {'MP6_FI_NO_RESIDUAL': '1'}, {'MP6_FI_SCALE_SNAP': '0.01'}):
                result = subprocess.run([str(exe)], env=dict(os.environ, **policy),
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
