"""Execute the real replay builder against the per-command-copy reference."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import os
from tools import build

ROOT = Path(__file__).resolve().parents[1]


def replay_test_source():
    source = (ROOT/'src/gx/frame_interp.c').read_text()
    start = source.index('static uint32_t fi_build_replay(')
    end = source.index('\nint mp6_fi_idle_present(', start)
    reference = (ROOT/'tests/native/frame_replay_reference.inc').read_text()
    # The native fixture reaches the plan's file-local state because this
    # extracted source is included into the same translation unit.  Keep the
    # retained-capacity assertions test-only: the initial plan frees storage
    # on ordinary invalidation, while the capacity follow-up retains bounded
    # arrays and releases them from savestate reset.
    retained_capacity = (
        'FI_REPLAY_PLAN_CACHE_MAX_ACTIONS' in source and
        'fi_replay_plan_release' in source
    )
    # No SDL/runtime calls are reachable from the builder under test.
    source = source[:end].replace('#include <SDL3/SDL.h>', '// SDL not needed by the isolated builder')
    source=source.replace('    const uint8_t b = data[pos];',
                          '    #ifndef MP6_BENCHMARK_WALK\n'
                          '    ++test_walk_calls;\n'
                          '    #endif\n'
                          '    const uint8_t b = data[pos];')
    source = source.replace('    FiMtx a;\n    double dx, dy, dz;',
                            '    ++test_prepare_calls;\n    FiMtx a;\n    double dx, dy, dz;')
    source = source.replace('    if (need <= *cap) return 1;',
        '    if (test_cache_grow_fail && (buf == (void **)&s_pairCache || '
        'buf == (void **)&s_pairState)) return 0;\n'
        '    if (need <= *cap) return 1;\n'
        '    if (test_plan_alloc_fail && '
        '(buf == test_plan_actions_addr || buf == test_plan_template_addr)) return 0;')
    capacity_define = ('#define MP6_FI_REPLAY_PLAN_RETAINED_CAPACITY 1\n'
                       if retained_capacity else '')
    reset = (ROOT/'src/gx/frame_interp.c').read_text().split('void mp6_fi_savestate_reset(void)', 1)[1]
    return ('static unsigned test_walk_calls, test_prepare_calls;\n'
            'static int test_cache_grow_fail;\n'
            'static int test_plan_alloc_fail;\n'
            'static void **test_plan_actions_addr, **test_plan_template_addr;\n'
            + capacity_define + source+'\n'+reference+
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
            compile_command = [build.ZIG, 'cc', '-O2', '-UNDEBUG', '-DMP6_FI_REPLAY_TEST',
                '-ffunction-sections',
                '-fdata-sections', '-Wl,--gc-sections', '-I'+str(folder), '-I'+str(ROOT/'include'),
                '-I'+str(ROOT/'src/host'),
                '-I'+str(ROOT/'third_party/aurora/include'),
                str(ROOT/'tests/native/frame_replay_batch_selftest.c'), '-o', str(exe)]
            benchmark_walk = bool(os.environ.get('MP6_BENCHMARK_WALK'))
            benchmark_memo = bool(os.environ.get('MP6_BENCHMARK_MEMO'))
            benchmark_plan = bool(os.environ.get('MP6_BENCHMARK_PLAN'))
            if benchmark_walk:
                compile_command.insert(3, '-DMP6_BENCHMARK_WALK')
            if benchmark_memo:
                compile_command.insert(3, '-DMP6_BENCHMARK_MEMO')
            if benchmark_plan:
                compile_command.insert(3, '-DMP6_BENCHMARK_PLAN')
            result = subprocess.run(compile_command,
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            printed_benchmark = False
            for policy in ({}, {'MP6_FI_NO_SCALE_HOLD': '1'},
                           {'MP6_FI_NO_RESIDUAL': '1'}, {'MP6_FI_SCALE_SNAP': '0.01'}):
                result = subprocess.run([str(exe)], env=dict(os.environ, **policy),
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('Old/new stream walker oracle passed', result.stdout)
                self.assertIn('Replay plan byte/marker/cut/fallback oracle passed',
                              result.stdout)
                self.assertIn('Replay plan stable/unstable/stable oracle passed',
                              result.stdout)
                if 'MP6_FI_REPLAY_PLAN_RETAINED_CAPACITY' in source:
                    self.assertIn('Replay plan retained-capacity/lifecycle oracle passed',
                                  result.stdout)
                if benchmark_walk:
                    self.assertIn('WALK trial=19', result.stdout)
                if benchmark_memo:
                    self.assertIn('MEMO_COMPONENT_ONLY replays=8', result.stdout)
                if benchmark_plan:
                    self.assertIn('PLAN_OWNER replays=8', result.stdout)
                if benchmark_walk or benchmark_memo or benchmark_plan:
                    if not printed_benchmark:
                        print(result.stdout, end='')
                        printed_benchmark = True


if __name__ == '__main__':
    unittest.main()
