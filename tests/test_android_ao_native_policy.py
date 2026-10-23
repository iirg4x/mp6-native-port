"""Current and historical native XeGTAO policy evidence, without a device run."""
import unittest

from tests.test_android_ao_benchmark_metrics import (
    AO_REFERENCE, GPU_OFF, GPU_LEGACY, benchmark_result, make_log, make_request,
)


STORAGE = '[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float'
CURRENT = AO_REFERENCE.removesuffix('; opt-in only')


class AndroidAoNativePolicyTests(unittest.TestCase):
    def parse(self, native, request=None, **kwargs):
        return benchmark_result(make_log(native=native, **kwargs),
                                make_request() if request is None else request)

    def test_current_and_historical_full_policy_formats(self):
        for native, actual in ((AO_REFERENCE, None),
                               (STORAGE + '\n' + AO_REFERENCE, 'r32float'),
                               (STORAGE + '\n' + CURRENT, 'r32float')):
            with self.subTest(native=native):
                result = self.parse(native)
                self.assertTrue(result['valid'], result['errors'])
                self.assertEqual(result['ao_policy']['depth_storage'], 'r32float')
                self.assertEqual(result['ao_policy']['effective_depth_storage'], actual)

    def test_current_format_requires_explicit_storage_evidence(self):
        result = self.parse(CURRENT)
        self.assertFalse(result['valid'])
        self.assertTrue(any('storage' in error for error in result['errors']))

    def test_storage_records_are_unique_exact_and_order_independent(self):
        self.assertTrue(self.parse(CURRENT + '\n' + STORAGE)['valid'])
        for storage in (STORAGE + '\n' + STORAGE, STORAGE + ' extra=1',
                        STORAGE.replace('actual=r32float', 'actual=unknown'),
                        STORAGE.replace('requested=r32float', 'requested=r16float'),
                        STORAGE.replace('actual=r32float', 'actual=r16float')):
            with self.subTest(storage=storage):
                self.assertFalse(self.parse(storage + '\n' + CURRENT)['valid'])

    def test_requested_storage_and_environment_must_agree(self):
        for policy, environment in (('r16float', None), ('r32float', 'r16float'),
                                    ('r16float', 'r32float'), ('unknown', None),
                                    (None, None), ('r32float', 'unknown')):
            request = make_request()
            request['requested_xegtao_policy']['depth_storage'] = policy
            if environment is not None:
                request['args'].append('MP6_AO_XEGTAO_DEPTH_STORAGE=' + environment)
            with self.subTest(policy=policy, environment=environment):
                self.assertFalse(self.parse(STORAGE + '\n' + CURRENT, request)['valid'])

    def test_r16_is_experimental_and_requires_actual_requested_storage(self):
        request = make_request()
        request['requested_xegtao_policy']['depth_storage'] = 'r16float'
        request['args'].append('MP6_AO_XEGTAO_DEPTH_STORAGE=r16float')
        native = STORAGE.replace('r32float', 'r16float') + '\n' + CURRENT.replace(
            'reference:', 'experimental:')
        result = self.parse(native, request)
        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(result['ao_policy']['effective_depth_storage'], 'r16float')
        for invalid in (native.replace('experimental:', 'reference:'),
                        native.replace('actual=r16float', 'actual=r32float'),
                        native.splitlines()[1], AO_REFERENCE):
            with self.subTest(native=invalid):
                self.assertFalse(self.parse(invalid, request)['valid'])

    def test_half_format_keeps_resolution_and_storage_independent(self):
        request = make_request(resolution='half')
        native = AO_REFERENCE.replace('reference:', 'experimental:').replace(
            'full-resolution direct', 'half-resolution bilateral')
        for prefix in ('', STORAGE + '\n'):
            result = self.parse(prefix + native, request)
            self.assertTrue(result['valid'], result['errors'])
            self.assertEqual(result['ao_policy']['effective_resolution'], 'half')

    def test_off_and_legacy_reject_storage_execution_evidence(self):
        for ao, gpu in ((0, GPU_OFF), (2, GPU_LEGACY)):
            request = make_request(implementation='legacy', ao=ao, xegtao=0)
            with self.subTest(ao=ao):
                self.assertFalse(self.parse(STORAGE, request, gpu=gpu)['valid'])
        request = make_request(implementation='legacy', ao=2, xegtao=0)
        request['requested_xegtao_policy']['depth_storage'] = 'r16float'
        request['args'].append('MP6_AO_XEGTAO_DEPTH_STORAGE=r16float')
        self.assertFalse(self.parse(None, request, gpu=GPU_LEGACY)['valid'])

    def test_unknown_extra_policy_records_still_fail_closed(self):
        valid = STORAGE + '\n' + CURRENT
        for extra in ('[MP6-AO] XeGTAO', '[MP6-AO] XeGTAO malformed', CURRENT, AO_REFERENCE):
            with self.subTest(extra=extra):
                self.assertFalse(self.parse(valid + '\n' + extra)['valid'])

    def test_scheduling_path_is_observed_not_inferred_from_policy(self):
        valid = STORAGE + '\n' + CURRENT
        self.assertIsNone(self.parse(valid)['ao_policy']['shader_scheduling_path'])
        for path in ('optimized', 'compatibility'):
            marker = '[MP6-AO] shader/scheduling path: ' + path
            result = self.parse(valid + '\n' + marker)
            self.assertTrue(result['valid'], result['errors'])
            self.assertEqual(result['ao_policy']['shader_scheduling_path'], path)
            legacy = make_request(implementation='legacy', ao=2, xegtao=0)
            self.assertTrue(self.parse(marker, legacy, gpu=GPU_LEGACY)['valid'])
            off = make_request(ao=0)
            self.assertFalse(self.parse(marker, off, gpu=GPU_OFF)['valid'])
            for invalid in (marker + ' extra=1', marker + '\n' + marker,
                            marker.replace(path, 'unknown')):
                with self.subTest(marker=invalid):
                    self.assertFalse(self.parse(valid + '\n' + invalid)['valid'])


if __name__ == '__main__':
    unittest.main()
