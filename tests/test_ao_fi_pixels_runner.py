"""Admission contracts for isolated, same-phase FI pixel captures."""
import ast
from pathlib import Path
import re
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'tests/integration/run_board_qa.py'


def helpers():
    tree = ast.parse(RUNNER.read_text(encoding='utf-8'))
    names = {'ao_fi_pixels_error', 'capture_rect_error', 'validate_ao_fi_pixel_policy'}
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    assert len(functions) == len(names)
    namespace = {'re': re}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(RUNNER), 'exec'), namespace)
    return namespace


class FiPixelRunnerTests(unittest.TestCase):
    def setUp(self):
        self.functions = helpers()

    def request(self, **changes):
        values = dict(ao_fi_pixels='3300:3420:6', ticks=3460,
                      unlocked_fps=True, tick_hz=60, implementation='default',
                      xegtao_quality='high', xegtao_denoise=2,
                      xegtao_resolution='full', xegtao_depth_storage='r32float',
                      xegtao_init_failure=False, xegtao_prepared_canonical=False,
                      ao=2, aa=0, leaf_camera=True, leaf_freeze=True,
                      ao_fi_matrix_range='3300:3420', capture_frames=21,
                      capture_stride=1, capture_delay=0, capture_trigger='-')
        for key in ('ao_cycle', 'resize_ao', 'raw_ao_depth', 'foreground_ao', 'water_ao',
                    'legacy_foliage_viewport', 'foliage_no_depth', 'no_grounding',
                    'no_lawn_lift', 'no_connector_lift', 'no_log_grounding',
                    'no_tree_grounding', 'no_foliage_mask', 'legacy_foliage_blend'):
            values[key] = None if key == 'ao_cycle' else False
        values.update(changes)
        return SimpleNamespace(**values)

    def test_opt_in_and_valid_schedule(self):
        check = self.functions['ao_fi_pixels_error']
        self.assertIsNone(check(SimpleNamespace(ao_fi_pixels=None)))
        self.assertIsNone(check(self.request()))
        self.assertIsNone(check(self.request(
            implementation='xegtao', xegtao_quality='ultra', xegtao_denoise=3)))

    def test_bad_ranges_rejected_before_any_run(self):
        check = self.functions['ao_fi_pixels_error']
        for value in ('', '3300:3420', '3300:3420:0', '3300:3421:6',
                      '2999:3005:1', '3400:3300:1', '3300:3420:121',
                      '3300:3420:6junk', '3300:9999999999999:6'):
            with self.subTest(value=value):
                self.assertIsNotNone(check(self.request(ao_fi_pixels=value)))

    def test_wrong_population_or_geometry_rejected(self):
        check = self.functions['ao_fi_pixels_error']
        for changes in (
            {'ticks': 3420}, {'unlocked_fps': False}, {'tick_hz': 0},
            {'implementation': 'legacy'}, {'ao': 0}, {'aa': 4},
            {'leaf_freeze': False}, {'leaf_camera': False},
            {'ao_fi_matrix_range': '3300:3340'}, {'capture_frames': 20},
            {'capture_stride': 2}, {'capture_delay': 1}, {'capture_trigger': 'leaf.camera'},
            {'water_ao': True}, {'no_grounding': True}, {'ao_cycle': 3300}, {'ao_cycle': 0},
        ):
            with self.subTest(changes=changes):
                self.assertIsNotNone(check(self.request(**changes)))

    def test_xegtao_pixel_policy_excludes_other_resolution_storage_and_failure_modes(self):
        check = self.functions['ao_fi_pixels_error']
        base = dict(implementation='xegtao', xegtao_quality='ultra', xegtao_denoise=3)
        for changes in (
            {'xegtao_resolution': 'half'},
            {'xegtao_depth_storage': 'r16float'},
            {'xegtao_init_failure': True},
            {'xegtao_prepared_canonical': True},
        ):
            with self.subTest(changes=changes):
                values = dict(base)
                values.update(changes)
                self.assertIsNotNone(check(self.request(**values)))

    def test_capture_rectangle(self):
        check = self.functions['capture_rect_error']
        self.assertIsNone(check(None, 0))
        self.assertIsNone(check([280, 300, 720, 250], 21))
        for rect, count in (([0, 0, 1, 1], 0), ([-1, 0, 100, 100], 1),
                            ([0, 0, 0, 2], 1), ([9900, 0, 100, 2], 1)):
            self.assertIsNotNone(check(rect, count))

    def test_probe_is_source_bound_and_environment_is_scrubbed(self):
        source = RUNNER.read_text(encoding='utf-8')
        self.assertIn("ready.get('ao_fi_pixel_probe_sha256') != fi_pixel_probe_hash", source)
        self.assertIn("parser.error('--ao-fi-pixels requires the isolated board-QA executable')", source)
        self.assertLess(source.index("if (error := ao_fi_pixels_error(args))"),
                        source.index('directory.mkdir('))
        self.assertLess(source.index("not k.startswith('MP6_')"),
                        source.index("env['MP6_QA_AO_FI_PIXELS'] = args.ao_fi_pixels"))
        self.assertIn("env['MP6_FRAME_DUMP_RECT'] = ','.join(map(str, args.capture_rect))", source)
        report_region = source[source.index("if args.ao_fi_pixels is not None:"):]
        self.assertIn('effective_policy=validate_ao_fi_pixel_policy(', report_region)

    def test_native_policy_validator_accepts_default_and_explicit_xe_policies(self):
        check = self.functions['validate_ao_fi_pixel_policy']
        for implementation, quality, denoise, selection in (
            ('default', 'high', 2, 'platform default'),
            ('xegtao', 'high', 2, 'environment override'),
            ('xegtao', 'ultra', 3, 'environment override'),
        ):
            with self.subTest(implementation=implementation, quality=quality, denoise=denoise):
                report = check(self.policy_log(implementation, quality, denoise, selection),
                               implementation, quality, denoise)
                self.assertEqual(report, dict(
                    implementation=implementation, quality=quality,
                    denoise_passes=denoise, resolution='full',
                    depth_storage='r32float', encoded_passes=7))

    @staticmethod
    def policy_log(implementation='default', quality='high', denoise=2,
                   selection=None, *, counters='7 7 0 7', storage='r32float r32float',
                   policy_kind=None):
        if selection is None:
            selection = 'platform default' if implementation == 'default' else 'environment override'
        if policy_kind is None:
            policy_kind = 'reference' if quality == 'high' and denoise == 2 else 'experimental'
        suffix = '' if denoise == 1 else 'es'
        encoded, composited, foliage, xegtao = counters.split()
        requested, actual = storage.split()
        return '\n'.join((
            f'[MP6-AO] requested implementation: XeGTAO ({selection})',
            f'[MP6-AO] XeGTAO {policy_kind}: full-resolution direct uint output, '
            f'{quality.title()}, {denoise} spatial denoise pass{suffix}',
            f'[MP6-AO] XeGTAO depth storage: requested={requested} actual={actual}',
            f'[MP6-AO] encoded={encoded} composited={composited} foliage={foliage} xegtao={xegtao}',
        ))

    def test_native_policy_validator_rejects_wrong_effective_evidence(self):
        check = self.functions['validate_ao_fi_pixel_policy']
        good = self.policy_log('xegtao', 'ultra', 3)
        cases = {
            'wrong policy': good.replace('Ultra, 3', 'High, 2'),
            'counter mismatch': good.replace('encoded=7 composited=7', 'encoded=7 composited=6'),
            'dropped counter': good.replace('[MP6-AO] encoded=', '[DROPPED] encoded='),
            'fatal': good + '\n[FATAL] GPU failure',
            'wrong storage': good.replace('actual=r32float', 'actual=r16float'),
            'duplicate selection': good + '\n' + good.splitlines()[0],
        }
        for name, log in cases.items():
            with self.subTest(name=name):
                with self.assertRaises((RuntimeError, ValueError)):
                    check(log, 'xegtao', 'ultra', 3)
        with self.assertRaises(ValueError):
            check(good, 'legacy', 'ultra', 3)
        with self.assertRaises(RuntimeError):
            check(good, 'xegtao', 'ultra', 3, 1, 'exited')
        with self.assertRaises(RuntimeError):
            check(good, 'xegtao', 'ultra', 3, 0, 'wall-clock limit')


if __name__ == '__main__':
    unittest.main()
