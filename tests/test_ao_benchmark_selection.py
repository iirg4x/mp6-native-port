"""CPU-only argument coverage for the board AO benchmark selectors."""
import ast
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'tests/integration/run_engine_benchmark.py'
BOARD_RUNNER = ROOT / 'tests/integration/run_board_qa.py'


def pure_runner_helpers():
    """Load only schedule/validation helpers; never execute the benchmark module."""
    tree = ast.parse(RUNNER.read_text())
    wanted = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in ('MATRIX_SCHEDULE', 'DEPTH_STORAGE_MATRIX',
                                                                'PREPARED_CANONICAL_MATRIX_SCHEDULE',
                                                                '_DEPTH_STORAGE_LOG_RE')
                for target in node.targets):
            wanted.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in (
                'validation_error', 'schedule_for', 'validate_xegtao_depth_storage'):
            wanted.append(node)
    namespace = {}
    namespace['re'] = re
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(RUNNER), 'exec'), namespace)
    return namespace


def pure_board_helpers():
    """Load the board runner's log validator without starting a run."""
    tree = ast.parse(BOARD_RUNNER.read_text())
    wanted = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == '_DEPTH_STORAGE_LOG_RE'
                for target in node.targets):
            wanted.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name == 'validate_xegtao_depth_storage':
            wanted.append(node)
    namespace = {'re': re}
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(BOARD_RUNNER), 'exec'), namespace)
    return namespace


class AoBenchmarkSelection(unittest.TestCase):
    def test_board_runner_exposes_implementation_selector(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / 'tests/integration/run_board_qa.py'), '--help'],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--implementation {legacy,xegtao,default}', result.stdout)
        self.assertIn('--xegtao-quality {low,medium,high,ultra}', result.stdout)
        self.assertIn('--xegtao-denoise {1,2,3}', result.stdout)
        self.assertIn('--xegtao-resolution {full,half}', result.stdout)
        self.assertIn('--xegtao-depth-storage {r32float,r16float}', result.stdout)

    def test_board_quality_selectors_are_explicit_after_environment_scrub(self):
        source = (ROOT / 'tests/integration/run_board_qa.py').read_text()
        scrub = source.index("if not k.startswith('MP6_')")
        for name, value in (('QUALITY', 'args.xegtao_quality'),
                            ('DENOISE', 'str(args.xegtao_denoise)'),
                            ('RESOLUTION', 'args.xegtao_resolution')):
            expression = f"env['MP6_AO_XEGTAO_{name}'] = {value}"
            self.assertEqual(source.count(expression), 1)
            self.assertGreater(source.index(expression), scrub)

    def test_engine_benchmark_exposes_and_records_implementation_selector(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / 'tests/integration/run_engine_benchmark.py'), '--help'],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--implementation {legacy,xegtao}', result.stdout)
        self.assertIn('--xegtao-quality {low,medium,high,ultra}', result.stdout)
        self.assertIn('--xegtao-denoise {1,2,3}', result.stdout)
        self.assertIn('--xegtao-resolution {full,half}', result.stdout)
        self.assertIn('--xegtao-depth-storage {r32float,r16float}', result.stdout)
        self.assertIn('--depth-storage-matrix', result.stdout)
        self.assertIn('--prepared-canonical-matrix', result.stdout)
        self.assertIn('--matrix-repetitions', result.stdout)

        source = RUNNER.read_text()
        self.assertIn("'--implementation',implementation", source)
        self.assertIn("'implementation':args.implementation", source)
        self.assertIn("'--xegtao-quality',args.xegtao_quality", source)
        self.assertIn("'--xegtao-denoise',str(args.xegtao_denoise)", source)
        self.assertIn("'--xegtao-resolution',args.xegtao_resolution", source)
        self.assertIn("'--xegtao-depth-storage',depth_storage", source)
        self.assertIn("row['xegtao_policy'] = dict(quality=args.xegtao_quality", source)
        self.assertIn('depth_storage=depth_storage', source)
        self.assertIn("row['requested_xegtao_depth_storage'] = depth_storage", source)
        self.assertIn("'requested_xegtao_policy':dict(quality=args.xegtao_quality", source)
        self.assertIn("'requested_xegtao_depth_storage':args.xegtao_depth_storage", source)

    def test_matrix_schedule_is_symmetric_and_condition_local(self):
        helpers = pure_runner_helpers()
        args = SimpleNamespace(ao_matrix=True, depth_storage_matrix=False, captures=False, control=None, aa=0,
                               ao=0, implementation='legacy', frame_profile=False)
        schedule = helpers['schedule_for'](args)
        self.assertEqual(
            schedule,
            (('off', 'current', 0, 'legacy'),
             ('legacy', 'current', 2, 'legacy'),
             ('xegtao', 'current', 2, 'xegtao'),
             ('xegtao', 'current', 2, 'xegtao'),
             ('legacy', 'current', 2, 'legacy'),
             ('off', 'current', 0, 'legacy')),
        )

    def test_prepared_canonical_matrix_schedule_repeats_on_current(self):
        helpers = pure_runner_helpers()
        args = SimpleNamespace(
            prepared_canonical_matrix=True, matrix_repetitions=2,
            ao_matrix=False, depth_storage_matrix=False, captures=False,
            control=None, aa=0, ao=0, implementation='legacy',
            frame_profile=True, xegtao_quality='high', xegtao_denoise=2,
            xegtao_resolution='full', xegtao_depth_storage='r32float',
        )
        self.assertEqual(
            helpers['schedule_for'](args),
            (('off', 'current', 0, 'legacy'),
             ('viewz', 'current', 2, 'xegtao'),
             ('canonical', 'current', 2, 'xegtao'),
             ('canonical', 'current', 2, 'xegtao'),
             ('viewz', 'current', 2, 'xegtao'),
             ('off', 'current', 0, 'legacy')) * 2,
        )

    def test_prepared_canonical_matrix_requires_isolated_full_profile(self):
        helpers = pure_runner_helpers()
        base = dict(
            prepared_canonical_matrix=True, matrix_repetitions=2,
            ao_matrix=False, depth_storage_matrix=False, captures=False,
            control=None, aa=0, ao=0, implementation='legacy',
            frame_profile=True, xegtao_quality='high', xegtao_denoise=2,
            xegtao_resolution='full', xegtao_depth_storage='r32float',
        )
        self.assertIsNone(helpers['validation_error'](SimpleNamespace(**base)))
        cases = (
            ('frame-profile', dict(frame_profile=False)),
            ('aa', dict(aa=2)),
            ('policy', dict(xegtao_quality='ultra')),
            ('depth storage', dict(xegtao_depth_storage='r16float')),
            ('other matrix', dict(ao_matrix=True)),
            ('captures', dict(captures=True)),
            ('control', dict(control='current', captures=True)),
        )
        for label, changes in cases:
            with self.subTest(label=label):
                self.assertIsNotNone(helpers['validation_error'](
                    SimpleNamespace(**{**base, **changes})))

    def test_matrix_rejects_capture_mode_and_nonzero_aa(self):
        helpers = pure_runner_helpers()
        base = dict(ao_matrix=True, depth_storage_matrix=False, captures=False, control=None, aa=0,
                    ao=0, implementation='legacy', frame_profile=False)
        self.assertIn('incompatible', helpers['validation_error'](
            SimpleNamespace(**{**base, 'captures': True})))
        self.assertIn('requires --aa 0', helpers['validation_error'](
            SimpleNamespace(**{**base, 'aa': 2})))
        self.assertIsNone(helpers['validation_error'](SimpleNamespace(**base)))

    def test_depth_storage_matrix_is_current_and_symmetric(self):
        helpers = pure_runner_helpers()
        args = SimpleNamespace(ao_matrix=False, depth_storage_matrix=True, captures=False,
                               control=None, aa=0, ao=0, implementation='legacy',
                               frame_profile=False)
        self.assertEqual(
            helpers['schedule_for'](args),
            (('r32float', 'current', 2, 'xegtao'),
             ('r16float', 'current', 2, 'xegtao'),
             ('r16float', 'current', 2, 'xegtao'),
             ('r32float', 'current', 2, 'xegtao')),
        )

    def test_depth_storage_matrix_rejects_other_measurement_modes(self):
        helpers = pure_runner_helpers()
        base = dict(ao_matrix=False, depth_storage_matrix=True, captures=False, control=None,
                    aa=0, ao=0, implementation='legacy', frame_profile=False)
        self.assertIn('incompatible', helpers['validation_error'](
            SimpleNamespace(**{**base, 'captures': True})))
        self.assertIn('requires --aa 0', helpers['validation_error'](
            SimpleNamespace(**{**base, 'aa': 2})))
        self.assertIn('selects its own AO', helpers['validation_error'](
            SimpleNamespace(**{**base, 'implementation': 'xegtao'})))
        self.assertIsNone(helpers['validation_error'](SimpleNamespace(**base)))

    def test_depth_storage_log_validator_rejects_missing_and_fallback(self):
        engine = pure_runner_helpers()['validate_xegtao_depth_storage']
        board = pure_board_helpers()['validate_xegtao_depth_storage']
        valid = '[MP6-AO] XeGTAO depth storage: requested=r16float actual=r16float\n'
        for validator in (engine, board):
            with self.subTest(validator=validator):
                self.assertEqual(validator(valid, 'r16float')['actual'], 'r16float')
                with self.assertRaises(RuntimeError):
                    validator('', 'r16float')
                with self.assertRaises(RuntimeError):
                    validator(valid.replace('actual=r16float', 'actual=r32float'), 'r16float')

    def test_board_depth_storage_is_explicit_after_environment_scrub(self):
        source = BOARD_RUNNER.read_text()
        scrub = source.index("if not k.startswith('MP6_')")
        expression = "env['MP6_AO_XEGTAO_DEPTH_STORAGE'] = args.xegtao_depth_storage"
        self.assertEqual(source.count(expression), 1)
        self.assertGreater(source.index(expression), scrub)


if __name__ == '__main__':
    unittest.main()
