"""Pure unit coverage for the isolated Android AO benchmark helpers.

These tests exercise source text and command-line construction only.  They do
not build, launch, install, or contact a device.
"""

import copy
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / 'tests' / 'integration'
sys.path.insert(0, str(INTEGRATION))

import android_ao_benchmark_source as source_helpers
import build_android_ao_benchmark as builder


FRAND_ANCHOR = 'seed = OSGetTime();'
WORLD_ANCHOR = 'void MB1_Create(void)\n{'
BRIDGE_ANCHOR = '    if (tEntry > 0) g_phSamples++;'
SHUTDOWN_ANCHOR = '    aurora_shutdown();\n    exit(0);'

FRAND_SOURCE = '''\
#include "game/frand.h"

static u32 frand_seed;

u32 frandom(u32 seed)
{
    if (seed == 0) {
        seed = OSGetTime();
        seed ^= 0xD826BC89;
    }
    return seed;
}
'''

WORLD_SOURCE = '''\
#include "game/board.h"

void MB1_Create(void)
{
    int board_setup = 1;
    (void)board_setup;
}
'''

BRIDGE_SOURCE = '''\
#include <stdio.h>

static void mp6_clean_shutdown_exit(const char *reason)
{
    mp6_present_rate_log_final();
    mp6_launcher_note_window_position(1, 2);
    mp6_launcher_ui_teardown();
    aurora_shutdown();
    exit(0);
}

void bridge_frame(void)
{
    if (tEntry > 0) g_phSamples++;
}
'''


class AndroidAoBenchmarkSourceTests(unittest.TestCase):
    def test_validate_window_accepts_defaults_boundaries_and_custom_windows(self):
        for warmup, samples in ((120, 120), (600, 1200), (1800, 4000),
                                (6000, 4000)):
            with self.subTest(warmup=warmup, samples=samples):
                self.assertIsNone(source_helpers.validate_window(warmup, samples))

    def test_validate_window_requires_exact_ints(self):
        invalid = (
            (True, 120),
            (False, 120),
            (600.0, 120),
            ('600', 120),
            (None, 120),
            (600, True),
            (600, 120.0),
            (600, '120'),
            (600, None),
        )
        for warmup, samples in invalid:
            with self.subTest(warmup=warmup, samples=samples), self.assertRaises(ValueError):
                source_helpers.validate_window(warmup, samples)

    def test_validate_window_rejects_each_bound_and_end_past_limit(self):
        invalid = (
            (119, 120),
            (6001, 120),
            (120, 119),
            (120, 4001),
            (6000, 4001),
        )
        for warmup, samples in invalid:
            with self.subTest(warmup=warmup, samples=samples), self.assertRaises(ValueError):
                source_helpers.validate_window(warmup, samples)

    def test_unknown_source_kind_fails_closed(self):
        with self.assertRaises(ValueError):
            source_helpers.transform_source('unknown', 'fixture')

    def test_frand_uses_one_exact_anchor_and_fixed_seeds(self):
        self.assertEqual(FRAND_SOURCE.count(FRAND_ANCHOR), 1)
        original = FRAND_SOURCE
        transformed = source_helpers.transform_source('frand', original)

        self.assertEqual(original, FRAND_SOURCE)
        self.assertEqual(transformed.count(FRAND_ANCHOR), 0)
        self.assertEqual(transformed.count('seed = 0x12345678u;'), 1)
        self.assertEqual(transformed.count('void mp6_benchmark_seed_board(void)'), 1)
        self.assertEqual(transformed.count('frand_seed = 0xA341316Cu;'), 1)
        with self.assertRaises(ValueError):
            source_helpers.transform_source('frand', transformed)

    def test_world01_inserts_one_board_seed_reset_after_exact_marker(self):
        transformed = source_helpers.transform_source('world01', WORLD_SOURCE)

        self.assertEqual(WORLD_SOURCE.count(WORLD_ANCHOR), 1)
        self.assertEqual(transformed.count(WORLD_ANCHOR), 1)
        self.assertEqual(transformed.count('extern void mp6_benchmark_seed_board(void);'), 1)
        self.assertEqual(transformed.count('mp6_benchmark_seed_board();'), 1)
        self.assertLess(
            transformed.index('mp6_benchmark_seed_board();'),
            transformed.index('int board_setup = 1;'),
        )
        with self.assertRaises(ValueError):
            source_helpers.transform_source('world01', transformed)

    def assert_bridge_window(self, transformed, warmup, samples):
        end = warmup + samples
        last = end - 1
        self.assertEqual(transformed.count(BRIDGE_ANCHOR), 1)
        self.assertIn(f'qaBoardAge >= {warmup}u', transformed)
        self.assertIn(f'qaBoardAge < {end}u', transformed)
        self.assertIn(f'qaBoardAge != {warmup}u + qaCount', transformed)
        self.assertIn(f'qaCount == {samples}u', transformed)
        # The generated BEGIN line records this inclusive upper endpoint as a
        # value; the active predicate uses the equivalent exclusive end.
        self.assertIn(f'{last}u', transformed)
        self.assertIn('first_age=', transformed)
        self.assertIn('last_age=', transformed)
        for marker in (
            'ANDROID-AO-BENCH-BEGIN',
            'ANDROID-AO-BENCH',
            'ANDROID-AO-BENCH-INVALID',
            '[ENGINE-GPU]',
            '[ENGINE-GPU-PASS]',
        ):
            self.assertIn(marker, transformed)

    def test_bridge_replaces_only_shutdown_exit_with_android_flush_guard(self):
        original = BRIDGE_SOURCE
        transformed = source_helpers.transform_source('bridge', original)

        self.assertEqual(original, BRIDGE_SOURCE)
        self.assertNotIn(SHUTDOWN_ANCHOR, transformed)
        self.assertEqual(transformed.count('mp6_present_rate_log_final();'), 1)
        self.assertEqual(transformed.count('mp6_launcher_note_window_position(1, 2);'), 1)
        self.assertEqual(transformed.count('mp6_launcher_ui_teardown();'), 1)
        self.assertEqual(transformed.count('aurora_shutdown();'), 1)
        self.assertEqual(transformed.count('fflush(NULL);'), 1)
        self.assertEqual(transformed.count('_Exit(0);'), 1)
        self.assertIn('#if defined(__ANDROID__)', transformed)
        self.assertIn('#else\n    exit(0);\n#endif', transformed)
        self.assertLess(
            transformed.index('mp6_launcher_ui_teardown();'),
            transformed.index('aurora_shutdown();'),
        )
        self.assertLess(
            transformed.index('aurora_shutdown();'),
            transformed.index('fflush(NULL);'),
        )
        self.assertLess(
            transformed.index('fflush(NULL);'),
            transformed.index('_Exit(0);'),
        )

    def test_bridge_shutdown_anchor_missing_or_duplicate_fails_closed(self):
        missing = BRIDGE_SOURCE.replace(SHUTDOWN_ANCHOR, '', 1)
        duplicate = BRIDGE_SOURCE + '\n' + SHUTDOWN_ANCHOR
        for case, fixture in (('missing', missing), ('duplicate', duplicate)):
            with self.subTest(case=case), self.assertRaises(ValueError):
                source_helpers.transform_source('bridge', fixture)

    def test_bridge_default_window_is_explicit_and_contains_cpu_and_gpu_logs(self):
        original = BRIDGE_SOURCE
        transformed = source_helpers.transform_source('bridge', original)

        self.assertEqual(original, BRIDGE_SOURCE)
        self.assert_bridge_window(transformed, 600, 1200)
        with self.assertRaises(ValueError):
            source_helpers.transform_source('bridge', transformed)

    def test_bridge_custom_window_is_deterministic_and_preserves_input(self):
        original = BRIDGE_SOURCE
        first = source_helpers.transform_source('bridge', original, 1800, 4000)
        second = source_helpers.transform_source('bridge', original, 1800, 4000)

        self.assertEqual(original, BRIDGE_SOURCE)
        self.assertEqual(first, second)
        self.assert_bridge_window(first, 1800, 4000)

    def test_missing_or_duplicate_exact_anchors_fail_closed(self):
        cases = (
            ('frand', FRAND_SOURCE, FRAND_ANCHOR),
            ('world01', WORLD_SOURCE, WORLD_ANCHOR),
            ('bridge', BRIDGE_SOURCE, BRIDGE_ANCHOR),
        )
        for kind, fixture, anchor in cases:
            with self.subTest(kind=kind, case='missing'):
                self.assertRaises(
                    ValueError,
                    source_helpers.transform_source,
                    kind,
                    fixture.replace(anchor, '', 1),
                )
            with self.subTest(kind=kind, case='duplicate'):
                self.assertRaises(
                    ValueError,
                    source_helpers.transform_source,
                    kind,
                    fixture + anchor,
                )


class AndroidAoBenchmarkBuilderTests(unittest.TestCase):
    def setUp(self):
        self.source = (ROOT / 'build' / 'release source' / 'bridge.c').resolve()
        self.obj = (ROOT / 'build' / 'release objects' / 'bridge.o').resolve()
        self.generated = (ROOT / 'build' / 'benchmark' / 'bridge.c').resolve()
        self.target = (ROOT / 'build' / 'benchmark' / 'bridge.o').resolve()
        self.quoted_include = (ROOT / 'third party' / 'quoted include').resolve()
        self.command = [
            'zig', 'cc', '-O2', '-fno-strict-aliasing',
            '-iquote', str(self.quoted_include),
            '-c', str(self.source), '-o', str(self.obj),
            '-MF', str(self.obj.with_suffix('.d')),
            '-MT', str(self.obj),
        ]

    def test_single_value_requires_one_complete_flag_value(self):
        self.assertEqual(builder.single_value(['cc', '-c', 'input.c'], '-c'), 'input.c')
        for command in ([], ['cc', '-c'], ['cc', '-c', 'a.c', '-c', 'b.c']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                builder.single_value(command, '-c')

    def test_redirected_compile_moves_only_outputs_and_preserves_release_flags(self):
        original = copy.deepcopy(self.command)
        redirected = builder.redirected_compile(
            self.command, self.source, self.obj, self.generated, self.target,
        )

        self.assertEqual(self.command, original)
        self.assertEqual(redirected[redirected.index('-c') + 1], str(self.generated))
        self.assertEqual(redirected[redirected.index('-o') + 1], str(self.target))
        self.assertEqual(redirected[redirected.index('-MF') + 1], str(self.target.with_suffix('.d')))
        self.assertEqual(redirected[redirected.index('-MT') + 1], str(self.target))
        self.assertIn('-O2', redirected)
        self.assertNotIn('-O3', redirected)
        include_index = redirected.index('-iquote')
        self.assertEqual(redirected[include_index + 1], str(self.quoted_include))
        self.assertIn(['-iquote', str(self.source.parent)], [
            redirected[index:index + 2]
            for index, value in enumerate(redirected[:-1]) if value == '-iquote'
        ])

    def test_redirected_compile_rejects_identity_duplicate_and_optimization_errors(self):
        wrong_source = list(self.command)
        wrong_source[wrong_source.index('-c') + 1] = str(self.source.with_name('other.c'))
        with self.assertRaises(RuntimeError):
            builder.redirected_compile(wrong_source, self.source, self.obj,
                                       self.generated, self.target)

        wrong_object = list(self.command)
        wrong_object[wrong_object.index('-o') + 1] = str(self.obj.with_name('other.o'))
        with self.assertRaises(RuntimeError):
            builder.redirected_compile(wrong_object, self.source, self.obj,
                                       self.generated, self.target)

        for flag in ('-c', '-o', '-MF', '-MT'):
            duplicate = list(self.command)
            duplicate.extend((flag, duplicate[duplicate.index(flag) + 1]))
            with self.subTest(flag=flag), self.assertRaises(RuntimeError):
                builder.redirected_compile(duplicate, self.source, self.obj,
                                           self.generated, self.target)

        for bad in (
            [arg for arg in self.command if arg != '-O2'],
            self.command + ['-O3'],
            self.command + ['-O2'],
            [arg for arg in self.command if arg != '-fno-strict-aliasing'],
        ):
            with self.subTest(command=bad), self.assertRaises(RuntimeError):
                builder.redirected_compile(bad, self.source, self.obj,
                                           self.generated, self.target)


if __name__ == '__main__':
    unittest.main()
