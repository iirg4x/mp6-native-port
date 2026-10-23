"""Pure parser coverage for the bounded Android AO benchmark report."""

import copy
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'integration'))

from android_ao_benchmark_metrics import benchmark_result


BEGIN = (
    '[ANDROID-AO-BENCH-BEGIN] first_age=600 target_last_age=1799 '
    'ticks=1200 global_tick=3058'
)
END = (
    '[ANDROID-AO-BENCH] ticks=1200 first_age=600 last_age=1799 '
    'global_tick=4257 game=3.827105 submit=2.365215 seal=0.710147 '
    'post=1.448901 frame=16.665272'
)
GPU = (
    '[ENGINE-GPU] status=2 samples=120 dropped=0 span=10.000 between=0.1\n'
    '[ENGINE-GPU-PASS] EFB avg=3 max=4\n'
    '[ENGINE-GPU-PASS] AO depth preparation avg=0.5 max=1\n'
    '[ENGINE-GPU-PASS] XeGTAO canonical depth avg=0.5 max=1\n'
    '[ENGINE-GPU-PASS] XeGTAO depth MIPs avg=0.75 max=1\n'
    '[ENGINE-GPU-PASS] XeGTAO normals avg=0.25 max=0.5\n'
    '[ENGINE-GPU-PASS] XeGTAO main avg=5 max=6\n'
    '[ENGINE-GPU-PASS] XeGTAO denoise avg=1 max=2\n'
    '[ENGINE-GPU-PASS] XeGTAO final denoise avg=1 max=2'
)
GPU_XEGTAO_ONE = GPU.replace(
    '[ENGINE-GPU-PASS] XeGTAO denoise avg=1 max=2\n', '', 1,
)
# The native encoder emits one row per intermediate dispatch, but the timing
# snapshot aggregates repeated pass names.  Three-pass evidence therefore has
# the same named-row inventory as two-pass evidence.
GPU_XEGTAO_THREE = GPU
GPU_OFF = (
    '[ENGINE-GPU] status=2 samples=120 dropped=0 span=6.063668 between=0.002488\n'
    '[ENGINE-GPU-PASS] EFB avg=4.941898 max=7.122612\n'
    '[ENGINE-GPU-PASS] Present + touch UI avg=1.119282 max=3.533207\n'
    '[ENGINE-GPU-PASS] EFB copy convert avg=0 max=0\n'
    '[ENGINE-GPU-PASS] EFB replay-copy clear avg=0 max=0'
)
GPU_LEGACY = (
    '[ENGINE-GPU] status=2 samples=120 dropped=0 span=10.000 between=0.1\n'
    '[ENGINE-GPU-PASS] EFB avg=3 max=4\n'
    '[ENGINE-GPU-PASS] AO depth preparation avg=0.5 max=1\n'
    '[ENGINE-GPU-PASS] GTAO avg=1 max=2\n'
    '[ENGINE-GPU-PASS] AO blur X avg=0.5 max=1\n'
    '[ENGINE-GPU-PASS] AO blur Y avg=0.5 max=1\n'
    '[ENGINE-GPU-PASS] Present + touch UI avg=0.1 max=0.2'
)
AO_REFERENCE = (
    '[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, '
    '2 spatial denoise passes; opt-in only'
)
CANONICAL_PREPARATION = '[MP6-AO] XeGTAO preparation: canonical'
CANONICAL_GPU = GPU.replace(
    '[ENGINE-GPU-PASS] XeGTAO canonical depth avg=0.5 max=1\n', '', 1,
)
PRESENT_RATE = '[MP6-PRESENTRATE] final: begin-frames=8584 end-frames=8583 ticks=4317'
BOOT = '[BOOT] reached 4317 VIWaitForRetrace ticks (limit 4317) -- shutting down Aurora, exiting 0'
DEVICE_DESTROYED = '[AURORA WARNING: aurora::gpu] Device lost: Device was destroyed.'
SHUTDOWN_CLEAN = '\n'.join((PRESENT_RATE, BOOT, DEVICE_DESTROYED))
SHUTDOWN_ACTUAL_TAIL = '\n'.join((
    SHUTDOWN_CLEAN,
    'FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x70f9203718)',
    'Fatal signal 6 (SIGABRT) at hwuiTask1',
))

BASE_REQUEST = {
    'args': [
        'MP6_ENH_AMBIENT_OCCLUSION=2',
        'MP6_AO_XEGTAO=1',
        'MP6_AO_XEGTAO_QUALITY=high',
        'MP6_AO_XEGTAO_DENOISE=2',
    ],
    'requested_ao_implementation': 'xegtao',
    'requested_xegtao_policy': {'quality': 'high', 'denoise_passes': 2},
    'board_benchmark': {
        'warmup_board_age': 600,
        'sample_count': 1200,
        'last_board_age': 1799,
    },
}


def make_log(native=AO_REFERENCE, gpu=GPU, shutdown=SHUTDOWN_CLEAN, preparation=None):
    rows = [BEGIN, END]
    if native:
        rows.insert(0, native)
    if preparation:
        rows.append(preparation)
    if gpu:
        rows.append(gpu)
    if shutdown:
        rows.append(shutdown)
    return '\n'.join(rows) + '\n'


def make_request(implementation='xegtao', ao=2, xegtao=1,
                 quality='high', denoise=2, resolution='full',
                 prepared_canonical=None):
    request = copy.deepcopy(BASE_REQUEST)
    request['requested_ao_implementation'] = implementation
    request['requested_xegtao_policy'] = {
        'quality': quality,
        'denoise_passes': denoise,
        'resolution': resolution,
    }
    request['args'] = [
        f'MP6_ENH_AMBIENT_OCCLUSION={ao}',
        f'MP6_AO_XEGTAO={xegtao}',
        f'MP6_AO_XEGTAO_QUALITY={quality}',
        f'MP6_AO_XEGTAO_DENOISE={denoise}',
        f'MP6_AO_XEGTAO_RESOLUTION={resolution}',
    ]
    if prepared_canonical is not None:
        request['requested_xegtao_prepared_canonical'] = prepared_canonical
        if prepared_canonical:
            request['args'].append('MP6_AO_XEGTAO_PREPARED_CANONICAL=1')
    return request


class AndroidAoBenchmarkMetricsTests(unittest.TestCase):
    def assert_invalid(self, log, request=None):
        result = benchmark_result(log, copy.deepcopy(request or BASE_REQUEST))
        self.assertFalse(result['valid'])
        self.assertTrue(result['errors'])
        return result

    def test_valid_reference_report_has_cpu_delta_gpu_scope_and_policy(self):
        result = benchmark_result(make_log(), copy.deepcopy(BASE_REQUEST))

        self.assertTrue(result['valid'])
        self.assertEqual(result['errors'], [])
        self.assertEqual(result['stage'], 'complete')
        cpu = result['cpu_window']
        self.assertEqual(cpu['sample_count'], 1200)
        self.assertEqual(cpu['first_age'], 600)
        self.assertEqual(cpu['last_age'], 1799)
        self.assertEqual(cpu['global_tick_delta'], 1199)
        gpu = result['gpu_final_window']
        self.assertEqual(gpu['sample_count'], 120)
        self.assertEqual(gpu['dropped_frames'], 0)
        self.assertEqual(gpu['passes']['EFB']['average_ms'], 3)
        self.assertEqual(gpu['passes']['XeGTAO main']['max_ms'], 6)
        self.assertEqual(gpu['passes']['XeGTAO denoise']['average_ms'], 1)
        self.assertEqual(gpu['passes']['XeGTAO final denoise']['average_ms'], 1)
        self.assertIn('1200', result['scope'])
        self.assertIn('120', result['scope'])
        self.assertNotIn('4000', result['scope'])
        self.assertEqual(result['ao_policy']['implementation'], 'xegtao')
        self.assertEqual(result['ao_policy']['quality'], 'high')
        self.assertEqual(result['ao_policy']['denoise_passes'], 2)
        # Historical full-resolution requests predate the explicit field and
        # environment flag, but their actual native log remains mandatory.
        self.assertEqual(result['ao_policy']['resolution'], 'full')
        self.assertEqual(result['ao_policy']['effective_resolution'], 'full')
        coverage = result['ao_stage_coverage']
        self.assertEqual(coverage['required'], [
            'AO depth preparation', 'XeGTAO canonical depth', 'XeGTAO depth MIPs',
            'XeGTAO normals', 'XeGTAO main', 'XeGTAO final denoise', 'XeGTAO denoise',
        ])
        self.assertEqual(coverage['missing'], [])
        self.assertEqual(coverage['observed'], [
            'AO depth preparation', 'XeGTAO canonical depth', 'XeGTAO depth MIPs',
            'XeGTAO normals', 'XeGTAO main', 'XeGTAO denoise', 'XeGTAO final denoise',
        ])
        self.assertIn('Named-stage presence only', coverage['scope'])
        self.assertIn('per-frame execution counts', coverage['scope'])
        self.assertIn('not a complete isolated AO cost', coverage['scope'])

    def test_explicit_prepared_canonical_report_uses_fused_stage_contract(self):
        request = make_request(prepared_canonical=True)
        result = benchmark_result(
            make_log(gpu=CANONICAL_GPU, preparation=CANONICAL_PREPARATION), request)

        self.assertTrue(result['valid'], result['errors'])
        self.assertTrue(result['ao_policy']['prepared_canonical'])
        self.assertEqual(result['ao_stage_coverage']['required'], [
            'AO depth preparation', 'XeGTAO depth MIPs', 'XeGTAO normals',
            'XeGTAO main', 'XeGTAO final denoise', 'XeGTAO denoise',
        ])
        self.assertNotIn('XeGTAO canonical depth', result['ao_stage_coverage']['observed'])

    def test_prepared_canonical_attestation_is_exactly_one_row_before_gpu(self):
        request = make_request(prepared_canonical=True)
        cases = (
            ('missing', make_log(gpu=CANONICAL_GPU)),
            ('duplicate', make_log(gpu=CANONICAL_GPU,
                                   preparation=CANONICAL_PREPARATION + '\n' + CANONICAL_PREPARATION)),
            ('unknown', make_log(gpu=CANONICAL_GPU,
                                 preparation='[MP6-AO] XeGTAO preparation: viewz')),
            ('suffix', make_log(gpu=CANONICAL_GPU,
                                preparation=CANONICAL_PREPARATION + ' extra')),
            ('after final gpu', make_log(gpu=CANONICAL_GPU).replace(
                CANONICAL_GPU, CANONICAL_GPU + '\n' + CANONICAL_PREPARATION)),
        )
        for label, log in cases:
            with self.subTest(case=label):
                result = self.assert_invalid(log, request)
                self.assertTrue(any('preparation' in error or 'canonical' in error
                                    for error in result['errors']))

    def test_prepared_canonical_requires_exact_environment_and_request_type(self):
        missing_env = make_request(prepared_canonical=True)
        missing_env['args'] = [arg for arg in missing_env['args']
                               if not arg.startswith('MP6_AO_XEGTAO_PREPARED_CANONICAL=')]
        zero_env = make_request(prepared_canonical=True)
        zero_env['args'] = [arg.replace('=1', '=0')
                            if arg.startswith('MP6_AO_XEGTAO_PREPARED_CANONICAL=') else arg
                            for arg in zero_env['args']]
        duplicate_env = make_request(prepared_canonical=True)
        duplicate_env['args'].append('MP6_AO_XEGTAO_PREPARED_CANONICAL=1')
        for label, request in (
            ('missing environment', missing_env),
            ('zero environment', zero_env),
            ('duplicate environment', duplicate_env),
        ):
            with self.subTest(case=label):
                self.assert_invalid(make_log(gpu=CANONICAL_GPU,
                                             preparation=CANONICAL_PREPARATION), request)
        for value in (1, 'true', None):
            request = make_request(prepared_canonical=value)
            with self.subTest(value=value):
                self.assert_invalid(make_log(gpu=CANONICAL_GPU,
                                             preparation=CANONICAL_PREPARATION), request)

    def test_prepared_canonical_is_forbidden_for_off_legacy_and_half(self):
        cases = (
            make_request(implementation='legacy', ao=0, xegtao=0, prepared_canonical=True),
            make_request(implementation='legacy', ao=2, xegtao=0, prepared_canonical=True),
            make_request(resolution='half', prepared_canonical=True),
        )
        for request in cases:
            with self.subTest(request=request):
                self.assert_invalid(make_log(gpu=CANONICAL_GPU,
                                             preparation=CANONICAL_PREPARATION), request)

    def test_explicit_false_or_historical_missing_selector_keeps_viewz_contract(self):
        for request in (make_request(prepared_canonical=False), copy.deepcopy(BASE_REQUEST)):
            with self.subTest(explicit='requested_xegtao_prepared_canonical' in request):
                result = benchmark_result(make_log(), request)
                self.assertTrue(result['valid'], result['errors'])
                self.assertFalse(result['ao_policy']['prepared_canonical'])
                self.assertIn('XeGTAO canonical depth', result['ao_stage_coverage']['required'])
        self.assert_invalid(make_log(preparation=CANONICAL_PREPARATION),
                            make_request(prepared_canonical=False))

    def test_prepared_canonical_rejects_old_raster_and_legacy_rows(self):
        old_raster = CANONICAL_GPU + '\n[ENGINE-GPU-PASS] XeGTAO canonical depth avg=0.5 max=1'
        legacy = CANONICAL_GPU + '\n[ENGINE-GPU-PASS] GTAO avg=0 max=0'
        request = make_request(prepared_canonical=True)
        self.assert_invalid(make_log(gpu=old_raster, preparation=CANONICAL_PREPARATION), request)
        self.assert_invalid(make_log(gpu=legacy, preparation=CANONICAL_PREPARATION), request)

    def test_clean_tick_budget_shutdown_allows_the_exact_device_destroyed_warning(self):
        request = make_request(implementation='legacy', ao=0, xegtao=0)
        result = benchmark_result(make_log(native=None, gpu=GPU_OFF), request)

        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(result['shutdown']['expected_device_destruction_count'], 1)
        self.assertTrue(result['shutdown']['requested_clean_shutdown'])

    def test_actual_full_tail_remains_invalid_after_a_valid_window(self):
        request = make_request(implementation='legacy', ao=0, xegtao=0)
        self.assert_invalid(
            make_log(native=None, gpu=GPU_OFF, shutdown=SHUTDOWN_ACTUAL_TAIL),
            request,
        )

    def test_device_destroyed_warning_requires_exact_ordered_clean_shutdown(self):
        mutations = (
            ('warning before boot', '\n'.join((PRESENT_RATE, DEVICE_DESTROYED, BOOT))),
            ('missing boot', '\n'.join((PRESENT_RATE, DEVICE_DESTROYED))),
            ('wrong tick limit', '\n'.join((
                PRESENT_RATE,
                BOOT.replace('limit 4317', 'limit 4316'),
                DEVICE_DESTROYED,
            ))),
            ('wrong shutdown reason', '\n'.join((
                PRESENT_RATE,
                BOOT.replace('shutting down Aurora, exiting 0', 'stopping Aurora, exiting 0'),
                DEVICE_DESTROYED,
            ))),
            ('device hung warning', '\n'.join((
                PRESENT_RATE,
                BOOT,
                DEVICE_DESTROYED.replace('Device was destroyed.', 'Device hung.'),
            ))),
            ('validation after boot', '\n'.join((
                PRESENT_RATE,
                BOOT,
                DEVICE_DESTROYED,
                '[VALIDATION-ERROR] post-shutdown validation failed',
            ))),
        )
        request = make_request(implementation='legacy', ao=0, xegtao=0)
        for label, shutdown in mutations:
            with self.subTest(case=label):
                self.assert_invalid(make_log(native=None, gpu=GPU_OFF, shutdown=shutdown), request)

    def test_off_and_legacy_reports_are_valid_without_xegtao_native_line(self):
        for implementation, ao, gpu in (('legacy', 0, GPU_OFF),
                                        ('legacy', 2, GPU_LEGACY)):
            with self.subTest(implementation=implementation, ao=ao):
                request = make_request(implementation=implementation, ao=ao, xegtao=0)
                result = benchmark_result(make_log(native=None, gpu=gpu), request)
                self.assertTrue(result['valid'], result['errors'])
                self.assertEqual(result['ao_policy']['implementation'], 'legacy')
                self.assertIsNone(result['ao_policy']['effective_resolution'])
                if ao == 0:
                    self.assertEqual(result['ao_stage_coverage']['required'], [])
                    self.assertEqual(result['ao_stage_coverage']['observed'], [])
                else:
                    self.assertEqual(result['ao_stage_coverage']['required'], [
                        'AO depth preparation', 'GTAO', 'AO blur X', 'AO blur Y',
                    ])
                    self.assertEqual(result['ao_stage_coverage']['missing'], [])

    def test_all_xegtao_presets_and_pass_counts_use_exact_native_log_format(self):
        labels = {'low': 'Low', 'medium': 'Medium', 'high': 'High', 'ultra': 'Ultra'}
        for resolution in ('full', 'half'):
            for quality in labels:
                for denoise in (1, 2, 3):
                    with self.subTest(resolution=resolution, quality=quality, denoise=denoise):
                        mode = ('reference' if resolution == 'full' and quality == 'high' and denoise == 2
                                else 'experimental')
                        suffix = 'pass' if denoise == 1 else 'passes'
                        output = ('full-resolution direct' if resolution == 'full'
                                  else 'half-resolution bilateral')
                        native = (
                            f'[MP6-AO] XeGTAO {mode}: {output} uint output, '
                            f'{labels[quality]}, {denoise} spatial denoise {suffix}; opt-in only'
                        )
                        request = make_request(quality=quality, denoise=denoise, resolution=resolution)
                        gpu = GPU_XEGTAO_ONE if denoise == 1 else GPU_XEGTAO_THREE
                        result = benchmark_result(make_log(native=native, gpu=gpu), request)
                        self.assertTrue(result['valid'], result['errors'])
                        self.assertEqual(result['ao_policy']['quality'], quality)
                        self.assertEqual(result['ao_policy']['denoise_passes'], denoise)
                        self.assertEqual(result['ao_policy']['resolution'], resolution)
                        self.assertEqual(result['ao_policy']['effective_resolution'], resolution)

    def test_half_resolution_policies_require_the_actual_bilateral_log(self):
        for quality in ('low', 'medium', 'high'):
            for denoise in (1, 2):
                with self.subTest(quality=quality, denoise=denoise):
                    suffix = 'pass' if denoise == 1 else 'passes'
                    native = (
                        f'[MP6-AO] XeGTAO experimental: half-resolution bilateral uint output, '
                        f'{quality.capitalize()}, {denoise} spatial denoise {suffix}; opt-in only'
                    )
                    request = make_request(quality=quality, denoise=denoise, resolution='half')
                    gpu = GPU if denoise == 2 else GPU_XEGTAO_ONE
                    # Actual logcat prefixes must not alter effective evidence.
                    log = '\n'.join('I/mp6     ( 1234): '+line
                                    for line in make_log(native=native, gpu=gpu).splitlines())
                    result = benchmark_result(log, request)
                    self.assertTrue(result['valid'], result['errors'])
                    self.assertEqual(result['ao_policy']['resolution'], 'half')
                    self.assertEqual(result['ao_policy']['effective_resolution'], 'half')

    def test_resolution_mismatch_missing_argument_and_malformed_native_are_rejected(self):
        half = AO_REFERENCE.replace('reference:', 'experimental:').replace(
            'full-resolution direct', 'half-resolution bilateral')
        request = make_request(resolution='half')
        full_result = self.assert_invalid(make_log(), request)
        self.assertEqual(full_result['ao_policy']['resolution'], 'half')
        self.assertEqual(full_result['ao_policy']['effective_resolution'], 'full')
        self.assert_invalid(make_log(native=half), make_request())
        self.assert_invalid(make_log(native=half), copy.deepcopy(BASE_REQUEST))
        for malformed in (half.replace('experimental:', 'reference:'),
                          half.replace('bilateral', 'direct'), half+'\n'+AO_REFERENCE,
                          half+'\n[MP6-AO] XeGTAO malformed'):
            with self.subTest(native=malformed):
                self.assert_invalid(make_log(native=malformed), request)
        for value in ('full', 'quarter', None):
            mismatch = copy.deepcopy(request)
            mismatch['args'] = [arg for arg in mismatch['args'] if not arg.startswith('MP6_AO_XEGTAO_RESOLUTION=')]
            if value is not None:
                mismatch['args'].append('MP6_AO_XEGTAO_RESOLUTION='+value)
            with self.subTest(environment=value):
                self.assert_invalid(make_log(native=half), mismatch)
        for value in ('quarter', None):
            invalid = make_request(resolution=value)
            with self.subTest(requested=value):
                self.assert_invalid(make_log(native=half), invalid)
        self.assert_invalid(make_log(native=None, gpu=GPU_LEGACY),
                            make_request(implementation='legacy', xegtao=0, resolution='half'))

    def test_ao_off_half_request_never_claims_effective_resolution(self):
        request = make_request(ao=0, resolution='half')
        result = benchmark_result(make_log(native=None, gpu=GPU_OFF), request)
        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(result['ao_policy']['resolution'], 'half')
        self.assertIsNone(result['ao_policy']['effective_resolution'])

    def test_begin_and_end_markers_must_be_unique_and_exact(self):
        for label, log in (
            ('missing begin', make_log().replace(BEGIN + '\n', '', 1)),
            ('duplicate begin', make_log() + BEGIN + '\n'),
            ('missing end', make_log().replace('\n' + END + '\n', '\n', 1)),
            ('duplicate end', make_log() + END + '\n'),
        ):
            with self.subTest(case=label):
                self.assert_invalid(log)

    def test_malformed_age_and_global_duration_are_rejected(self):
        cases = (
            ('begin first age', make_log().replace('first_age=600', 'first_age=601', 1)),
            ('begin target age', make_log().replace('target_last_age=1799', 'target_last_age=1800', 1)),
            ('end first age', make_log().replace('first_age=600 last_age=1799', 'first_age=601 last_age=1799', 1)),
            ('end last age', make_log().replace('last_age=1799', 'last_age=1800', 1)),
            ('global duration', make_log().replace('global_tick=4257', 'global_tick=4258', 1)),
            ('sample count', make_log().replace('ticks=1200', 'ticks=1199', 1)),
        )
        for label, log in cases:
            with self.subTest(case=label):
                self.assert_invalid(log)

    def test_missing_or_malformed_gpu_window_is_rejected(self):
        for label, log in (
            ('missing gpu', make_log(gpu=None)),
            ('bad status', make_log(gpu=GPU.replace('status=2', 'status=1', 1))),
            ('bad sample count', make_log(gpu=GPU.replace('samples=120', 'samples=119', 1))),
            ('dropped frame', make_log(gpu=GPU.replace('dropped=0', 'dropped=1', 1))),
            ('malformed pass suffix', make_log(gpu=GPU.replace(
                'XeGTAO main avg=5 max=6', 'XeGTAO main avg=5 max=6 trailing', 1))),
            ('malformed header suffix', make_log(gpu=GPU.replace(
                'span=10.000 between=0.1', 'span=10.000 between=0.1 trailing', 1))),
        ):
            with self.subTest(case=label):
                self.assert_invalid(log)

    def test_every_required_xegtao_stage_is_mandatory(self):
        stages = (
            'AO depth preparation', 'XeGTAO canonical depth', 'XeGTAO depth MIPs',
            'XeGTAO normals', 'XeGTAO main', 'XeGTAO denoise', 'XeGTAO final denoise',
        )
        for stage in stages:
            with self.subTest(stage=stage):
                gpu = '\n'.join(line for line in GPU.splitlines() if stage not in line)
                result = self.assert_invalid(make_log(gpu=gpu))
                self.assertIn(stage, result['ao_stage_coverage']['missing'])

    def test_every_required_legacy_stage_is_mandatory(self):
        request = make_request(implementation='legacy', ao=2, xegtao=0)
        for stage in ('AO depth preparation', 'GTAO', 'AO blur X', 'AO blur Y'):
            with self.subTest(stage=stage):
                gpu = '\n'.join(line for line in GPU_LEGACY.splitlines() if stage not in line)
                result = self.assert_invalid(make_log(native=None, gpu=gpu), request)
                self.assertIn(stage, result['ao_stage_coverage']['missing'])

    def test_required_zero_timing_rows_are_valid(self):
        gpu = GPU
        for row in (
                'AO depth preparation avg=0.5 max=1',
                'XeGTAO canonical depth avg=0.5 max=1',
                'XeGTAO depth MIPs avg=0.75 max=1',
                'XeGTAO normals avg=0.25 max=0.5',
                'XeGTAO main avg=5 max=6',
                'XeGTAO denoise avg=1 max=2',
                'XeGTAO final denoise avg=1 max=2'):
            gpu = gpu.replace(row, row.rsplit('avg=', 1)[0] + 'avg=0 max=0')
        result = benchmark_result(make_log(gpu=gpu), copy.deepcopy(BASE_REQUEST))
        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(result['ao_stage_coverage']['missing'], [])
        self.assertEqual(result['gpu_final_window']['passes']['XeGTAO main']['average_ms'], 0)

    def test_xegtao_rejects_legacy_gtao_and_blur_rows_as_mixed_fallback(self):
        gpu = GPU + '\n' + '\n'.join(
            line for line in GPU_LEGACY.splitlines() if 'GTAO' in line or 'AO blur' in line)
        result = self.assert_invalid(make_log(gpu=gpu))
        self.assertTrue(any('legacy GTAO/blur' in error for error in result['errors']))

    def test_optional_ao_rows_are_not_required_and_are_reported_when_present(self):
        optional = ('AO depth adapter', 'AO foreground', 'AO decal', 'AO foliage')
        gpu = GPU + ''.join(f'\n[ENGINE-GPU-PASS] {name} avg=0 max=0' for name in optional)
        result = benchmark_result(make_log(gpu=gpu), copy.deepcopy(BASE_REQUEST))
        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(result['ao_stage_coverage']['missing'], [])
        self.assertEqual(result['ao_stage_coverage']['observed'][-4:], list(optional))

    def test_invalid_marker_crash_and_validation_error_fail_closed(self):
        for label, suffix in (
            ('invalid board window', '[ANDROID-AO-BENCH-INVALID] expected_age=601 actual_age=602 ticks=1'),
            ('crash', '[MP6-CRASH] signal=11'),
            ('fatal signal', 'Fatal signal 11'),
            ('validation error', '[VALIDATION-ERROR] benchmark validation failed'),
        ):
            with self.subTest(case=label):
                self.assert_invalid(make_log() + suffix + '\n')

    def test_requested_ao_policy_and_environment_must_match_observed_policy(self):
        mismatch_cases = (
            ('xegtao disabled', make_request(xegtao=0)),
            ('ao disabled', make_request(ao=0)),
            ('wrong implementation', make_request(implementation='legacy')),
            ('wrong quality', make_request(quality='medium')),
            ('wrong denoise count', make_request(denoise=1)),
        )
        for label, request in mismatch_cases:
            with self.subTest(case=label):
                self.assert_invalid(make_log(), request)

    def test_invalid_quality_and_denoise_count_are_rejected(self):
        for label, request in (
            ('unknown quality', make_request(quality='cinematic')),
            ('uppercase quality', make_request(quality='Ultra')),
            ('four denoise passes', make_request(denoise=4)),
            ('noninteger denoise passes', make_request(denoise='3')),
        ):
            with self.subTest(case=label):
                self.assert_invalid(make_log(), request)

    def test_actual_native_policy_must_match_ultra_and_three_pass_requests(self):
        ultra3 = (
            '[MP6-AO] XeGTAO experimental: full-resolution direct uint output, '
            'Ultra, 3 spatial denoise passes; opt-in only'
        )
        self.assert_invalid(make_log(native=ultra3), make_request(quality='high', denoise=3))
        self.assert_invalid(make_log(native=ultra3), make_request(quality='ultra', denoise=2))

    def test_experimental_fallback_cannot_claim_reference_policy(self):
        fallback = (
            '[MP6-AO] XeGTAO experimental: full-resolution direct uint output, '
            'High, 2 spatial denoise passes; opt-in only'
        )
        self.assert_invalid(make_log(native=fallback))

    def test_xegtao_request_requires_effective_main_and_final_gpu_passes(self):
        for label, gpu in (
            ('no xegtao passes', GPU_OFF),
            ('legacy gtao only', GPU_LEGACY),
            ('missing main', GPU.replace('[ENGINE-GPU-PASS] XeGTAO main avg=5 max=6\n', '', 1)),
            ('missing final denoise', GPU.replace('[ENGINE-GPU-PASS] XeGTAO final denoise avg=1 max=2', '', 1)),
        ):
            with self.subTest(case=label):
                self.assert_invalid(make_log(native=AO_REFERENCE, gpu=gpu))

    def test_ao_off_and_legacy_requests_reject_xegtao_gpu_passes(self):
        self.assert_invalid(make_log(native=None, gpu=GPU), make_request(ao=0, xegtao=0))
        self.assert_invalid(
            make_log(native=None, gpu=GPU),
            make_request(implementation='legacy', ao=2, xegtao=0),
        )

    def test_xegtao_denoise_inventory_matches_requested_pass_count(self):
        self.assert_invalid(make_log(gpu=GPU_XEGTAO_ONE), make_request(denoise=2))
        self.assert_invalid(make_log(gpu=GPU), make_request(denoise=1))
        ultra3 = (
            '[MP6-AO] XeGTAO experimental: full-resolution direct uint output, '
            'Ultra, 3 spatial denoise passes; opt-in only'
        )
        # Two intermediate dispatches may be aggregated into the single
        # `XeGTAO denoise` timing row, but that row is still mandatory.
        self.assert_invalid(
            make_log(native=ultra3, gpu=GPU_XEGTAO_ONE),
            make_request(quality='ultra', denoise=3),
        )


if __name__ == '__main__':
    unittest.main()
