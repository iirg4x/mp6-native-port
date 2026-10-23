"""CPU-only contracts for the opt-in XeGTAO initialization-failure QA run."""
import ast
import re
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / 'tests/integration/build_board_qa.py'
RUNNER = ROOT / 'tests/integration/run_board_qa.py'


def pure_runner_helpers():
    """Extract validation helpers without parsing argv or starting a process."""

    tree = ast.parse(RUNNER.read_text())
    wanted = []
    names = {
        '_AO_SHUTDOWN_COUNTERS_RE',
        '_XEGTAO_INIT_FAILURE_INJECTION_MARKER',
        '_XEGTAO_INIT_FAILURE_MARKER',
        '_XEGTAO_INIT_FAILURE_BAD_MARKERS',
    }
    functions = {'xegtao_init_failure_error', 'validate_xegtao_init_failure'}
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in names
                for target in node.targets):
            wanted.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in functions:
            wanted.append(node)
    namespace = {'re': re}
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(RUNNER), 'exec'), namespace)
    return namespace


class AoInitFailureQa(unittest.TestCase):
    def test_build_seam_define_is_limited_to_ao_qa_object(self):
        source = BUILDER.read_text()
        define = "'-DMP6_XEGTAO_QA_INVALID_SHADER'"
        self.assertEqual(source.count(define), 1)
        object_block = source.index("if name == 'plat_ambient_occlusion_aurora.o':")
        self.assertGreater(source.index(define), object_block)

    def test_runner_rejects_invalid_init_failure_combinations_before_run_dir(self):
        source = RUNNER.read_text()
        self.assertIn("parser.add_argument('--xegtao-init-failure', action='store_true'", source)
        self.assertIn("env['MP6_QA_XEGTAO_INVALID_SHADER'] = '1'", source)
        self.assertIn("report['expected_init_failure'] = expected_init_failure", source)
        self.assertLess(
            source.index('xegtao_init_failure_error(args.xegtao_init_failure, args.implementation, args.ao)'),
            source.index("directory = root / 'build/board-qa-runs'"),
        )
        self.assertLess(
            source.index("env['MP6_QA_XEGTAO_INVALID_SHADER'] = '1'"),
            source.index("with (directory / 'game.log').open"),
        )

        error = pure_runner_helpers()['xegtao_init_failure_error']
        self.assertIsNone(error(False, 'legacy', 0))
        self.assertIsNone(error(True, 'xegtao', 1))
        self.assertIn('--implementation xegtao', error(True, 'legacy', 2))
        self.assertIn('--ao > 0', error(True, 'xegtao', 0))

    def test_expected_failure_requires_fallback_and_real_legacy_work(self):
        helpers = pure_runner_helpers()
        validate = helpers['validate_xegtao_init_failure']
        valid = (
            '[MP6-AO-QA] injecting invalid XeGTAO main shader\n'
            '[MP6-AO] XeGTAO unavailable: malformed WGSL; using legacy\n'
            '[MP6-AO] encoded=37 composited=31 foliage=12 xegtao=0\n'
        )
        result = validate(valid, 0, 'exited')
        self.assertEqual(result['counters'],
                         dict(encoded=37, composited=31, foliage=12, xegtao=0))
        self.assertTrue(result['legacy_fallback'])
        self.assertEqual(result['exit_code'], 0)
        self.assertEqual(result['reason'], 'exited')

        invalid = (
            '',
            valid.replace('[MP6-AO-QA] injecting invalid XeGTAO main shader\n', ''),
            '[MP6-AO] encoded=37 composited=31 foliage=12 xegtao=0\n',
            valid.replace('using legacy', 'ignored fallback'),
            '[MP6-AO] XeGTAO unavailable: malformed WGSL\n'
            '[MP6-AO] encoded=37 composited=31 foliage=12 xegtao=0\n',
            valid.replace('encoded=37', 'encoded=0'),
            valid.replace('xegtao=0', 'xegtao=1'),
            valid + '[AURORA ERROR] shader validation failed\n',
            valid + '[FATAL] shader failure\n',
            valid + 'GPUValidationError: uncaptured error\n',
        )
        for log in invalid:
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                validate(log)
        for exit_code, reason in ((1, 'exited'), (0, 'wall-clock limit')):
            with self.subTest(exit_code=exit_code, reason=reason), self.assertRaises(RuntimeError):
                validate(valid, exit_code, reason)

        # The no-injection case above is intentionally a complete otherwise-valid
        # log, proving unsupported-limit fallback cannot satisfy this diagnostic.
        self.assertRaises(
            RuntimeError,
            validate,
            valid.replace('[MP6-AO-QA] injecting invalid XeGTAO main shader\n', ''),
        )


if __name__ == '__main__':
    unittest.main()
