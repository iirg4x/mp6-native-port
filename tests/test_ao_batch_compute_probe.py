"""CPU-only fail-closed checks for batched AO submission runs."""
from pathlib import Path
from types import SimpleNamespace
import unittest

from tests.integration.ao_batch_compute_probe import (
    batch_compute_error, validate_batch_compute, BATCHED_ROW)


class BatchComputeProbe(unittest.TestCase):
    def args(self, **changes):
        values = dict(xegtao_batch_compute=True, implementation='xegtao', ao=2,
                      xegtao_resolution='full', xegtao_depth_storage='r32float')
        values.update(changes)
        return SimpleNamespace(**values)

    def log(self):
        return '\n'.join((BATCHED_ROW,
            '[MP6-AO] requested implementation: XeGTAO (environment override)',
            '[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, 2 spatial denoise passes',
            '[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float',
            '[MP6-AO] encoded=10 composited=10 foliage=0 xegtao=10'))

    def test_bounded_admission(self):
        self.assertIsNone(batch_compute_error(self.args()))
        self.assertIsNone(batch_compute_error(self.args(xegtao_batch_compute=False, ao=0)))
        for changes in (dict(implementation='legacy'), dict(implementation='default'),
                        dict(ao=0), dict(ao=1), dict(xegtao_resolution='half'),
                        dict(xegtao_depth_storage='r16float'), dict(xegtao_init_failure=True),
                        dict(ao_cycle=100), dict(ao_fi_pixels='3300:3310:1'),
                        dict(deferred_capture='3300:3310')):
            with self.subTest(changes=changes):
                self.assertIsNotNone(batch_compute_error(self.args(**changes)))

    def test_exact_runtime_marker_and_conserved_policy(self):
        report = validate_batch_compute(self.log(), True, 'high', 2, 0, 'exited')
        self.assertEqual(report['actual'], 'batched')
        self.assertEqual(report['effective_policy']['encoded_passes'], 10)
        for log in ('', self.log() + '\n' + BATCHED_ROW,
                    self.log().replace('submission: batched', 'submission: separate'),
                    self.log().replace('composited=10', 'composited=9'),
                    self.log() + '\nGPUValidationError',
                    self.log().replace('High, 2', 'High, 3')):
            with self.subTest(log=log):
                with self.assertRaises(RuntimeError):
                    validate_batch_compute(log, True, 'high', 2, 0, 'exited')

    def test_clean_exit_required_and_unsolicited_marker_rejected(self):
        for code, reason in ((1, 'exited'), (0, 'timeout')):
            with self.assertRaises(RuntimeError):
                validate_batch_compute(self.log(), True, 'high', 2, code, reason)
        self.assertIsNone(validate_batch_compute('', False, 'high', 2, 0, 'exited'))
        with self.assertRaises(RuntimeError):
            validate_batch_compute(self.log(), False, 'high', 2, 0, 'exited')

    def test_runner_scrubs_before_explicit_assignment_and_reports_proof(self):
        source = (Path(__file__).resolve().parents[1]/'tests/integration/run_board_qa.py').read_text()
        self.assertIn("parser.add_argument('--xegtao-batch-compute', action='store_true'", source)
        scrub = source.index("env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}")
        self.assertGreater(source.index("env['MP6_AO_XEGTAO_BATCH_COMPUTE'] = '1'"), scrub)
        self.assertIn("report['xegtao_batch_compute'] = xegtao_batch_compute", source)


if __name__ == '__main__':
    unittest.main()
