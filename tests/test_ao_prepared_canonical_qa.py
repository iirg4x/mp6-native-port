"""CPU-only contracts for the explicit prepared-canonical board QA seam."""

import ast
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'tests/integration/run_board_qa.py'


def pure_runner_helpers():
    """Extract admission and log validators without parsing argv or launching QA."""

    tree = ast.parse(RUNNER.read_text())
    wanted = []
    constants = {
        '_XEGTAO_PREPARED_CANONICAL_MARKER',
        '_XEGTAO_PREPARED_CANONICAL_ROW',
    }
    functions = {
        'xegtao_prepared_canonical_error',
        'validate_xegtao_prepared_canonical',
    }
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in constants
                for target in node.targets):
            wanted.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in functions:
            wanted.append(node)
    namespace = {}
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(RUNNER), 'exec'), namespace)
    return namespace


class PreparedCanonicalQa(unittest.TestCase):
    def test_opt_in_admission_requires_full_xegtao_with_ao(self):
        error = pure_runner_helpers()['xegtao_prepared_canonical_error']
        self.assertIsNone(error(False, 'legacy', 'half', 0))
        self.assertIsNone(error(True, 'xegtao', 'full', 2))
        self.assertIn('--implementation xegtao', error(True, 'legacy', 'full', 2))
        self.assertIn('--xegtao-resolution full', error(True, 'xegtao', 'half', 2))
        self.assertIn('--ao > 0', error(True, 'xegtao', 'full', 0))

    def test_cli_validation_and_explicit_environment_assignment_are_bounded(self):
        source = RUNNER.read_text()
        self.assertIn(
            "parser.add_argument('--xegtao-prepared-canonical', action='store_true'",
            source,
        )
        admission = source.index('xegtao_prepared_canonical_error(')
        run_directory = source.index("directory = root / 'build/board-qa-runs'")
        self.assertLess(admission, run_directory)
        scrub = source.index("env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}")
        assignment = source.index("env['MP6_AO_XEGTAO_PREPARED_CANONICAL'] = '1'")
        self.assertGreater(assignment, scrub)
        self.assertEqual(source.count("env['MP6_AO_XEGTAO_PREPARED_CANONICAL'] = '1'"), 1)
        self.assertIn("'MP6_AO_XEGTAO_PREPARED_CANONICAL'", source)

    def test_request_and_result_provenance_include_the_boolean_opt_in(self):
        source = RUNNER.read_text()
        self.assertIn("report = dict(request=vars(args)", source)
        self.assertIn("report['xegtao_prepared_canonical'] = xegtao_prepared_canonical", source)
        self.assertIn("args.xegtao_prepared_canonical", source)

    def test_canonical_log_requires_exactly_one_row(self):
        validate = pure_runner_helpers()['validate_xegtao_prepared_canonical']
        row = '[MP6-AO] XeGTAO preparation: canonical\n'
        result = validate(row, True)
        self.assertEqual(result, dict(requested=True, actual='canonical', records=1))
        for log in (
                '',
                row + row,
                '[MP6-AO] XeGTAO preparation: viewz\n',
                '[MP6-AO] XeGTAO preparation: canonical extra\n',
        ):
            with self.subTest(log=log):
                with self.assertRaises(RuntimeError):
                    validate(log, True)

    def test_noncanonical_and_init_failure_runs_forbid_canonical_row(self):
        validate = pure_runner_helpers()['validate_xegtao_prepared_canonical']
        row = '[MP6-AO] XeGTAO preparation: canonical\n'
        for requested, init_failure in ((False, False), (False, True), (True, True)):
            with self.subTest(requested=requested, init_failure=init_failure):
                self.assertEqual(
                    validate('', requested, init_failure),
                    dict(requested=requested, actual='not-emitted', records=0),
                )
                with self.assertRaises(RuntimeError):
                    validate(row, requested, init_failure)


if __name__ == '__main__':
    unittest.main()
