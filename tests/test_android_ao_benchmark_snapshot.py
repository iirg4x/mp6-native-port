"""Attested asynchronous GPU snapshots: pure parsing and a CPU-only C fixture.

The fixture substitutes the GPU API, so these tests never build a renderer,
launch the game, or access an Android device.
"""

import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'integration'))

from android_ao_benchmark_metrics import benchmark_result
from android_ao_benchmark_source import transform_source
import android_device_profile as device_profile
from tests.test_android_ao_benchmark_metrics import (
    AO_REFERENCE, BASE_REQUEST, BEGIN, END, GPU, SHUTDOWN_CLEAN, make_log,
    make_request,
)


REQUEST = '[ANDROID-AO-GPU-REQUEST] id=17 global_tick=4257'
SNAPSHOT = (
    '[ENGINE-GPU-SNAPSHOT] id=17 generation=3 global_tick=4259 '
    'span_median=9 span_p95=12 span_max=14 '
    'between_median=0.08 between_p95=0.2 between_max=0.4 '
    'ao_mean=9 ao_median=8.5 ao_p95=10 ao_max=12'
)


def snapshot_request(version=2):
    request = copy.deepcopy(BASE_REQUEST)
    request['board_benchmark']['gpu_timing_version'] = version
    return request


def snapshot_log(request=REQUEST, snapshot=SNAPSHOT, gpu=GPU):
    return '\n'.join(filter(None, (
        AO_REFERENCE, BEGIN, END, request, snapshot, gpu, SHUTDOWN_CLEAN,
    ))) + '\n'


class AndroidAoSnapshotMetricsTests(unittest.TestCase):
    def assert_invalid(self, log, request=None):
        result = benchmark_result(log, snapshot_request() if request is None else request)
        self.assertFalse(result['valid'], result)
        self.assertTrue(result['errors'])
        # Results must remain serializable without non-standard NaN/Infinity.
        json.dumps(result, allow_nan=False)
        return result

    def test_v2_preserves_snapshot_identity_and_actual_distribution(self):
        result = benchmark_result(snapshot_log(), snapshot_request())
        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(result['stage'], 'complete')
        distribution = result['gpu_distribution']
        self.assertEqual(distribution['request_id'], 17)
        self.assertEqual(distribution['generation'], 3)
        self.assertEqual(distribution['request_global_tick'], 4257)
        self.assertEqual(distribution['snapshot_global_tick'], 4259)
        self.assertEqual(distribution['sample_count'], 120)
        self.assertEqual(distribution['ao_mean_ms'], 9)
        self.assertEqual(distribution['ao_p95_ms'], 10)
        self.assertEqual(distribution['ao_max_ms'], 12)
        self.assertEqual(distribution['span_median_ms'], 9)
        self.assertEqual(distribution['span_p95_ms'], 12)
        self.assertEqual(result['gpu_final_window']['span_average_ms'], 10)
        self.assertEqual(result['cpu_window']['sample_count'], 1200)
        self.assertIn('per-frame sum', distribution['scope'])
        self.assertIn('not wall time', distribution['scope'])
        # The aggregate percentile is independent of the row maxima
        # (1+1+0.5+6+2+2).
        self.assertNotEqual(distribution['ao_p95_ms'], sum(
            row['max_ms'] for name, row in result['gpu_final_window']['passes'].items()
            if name.startswith('XeGTAO ')))

    def test_legacy_reports_do_not_invent_a_distribution(self):
        for request in (copy.deepcopy(BASE_REQUEST), snapshot_request(1)):
            with self.subTest(request=request):
                result = benchmark_result(make_log(), request)
                self.assertTrue(result['valid'], result['errors'])
                self.assertIsNone(result['gpu_distribution'])
                self.assertNotIn('median_ms', result['gpu_final_window'])
        for request in (copy.deepcopy(BASE_REQUEST), snapshot_request(1)):
            with self.subTest(unattested=request):
                self.assert_invalid(snapshot_log(), request)

    def test_unsupported_version_types_and_values_fail_closed(self):
        for version in (0, 3, -1, True, False, 2.0, '2', None, [], {}):
            with self.subTest(version=version):
                self.assert_invalid(snapshot_log(), snapshot_request(version))

    def test_request_and_snapshot_records_must_exist_exactly_once(self):
        for marker in (REQUEST, SNAPSHOT, GPU.splitlines()[0]):
            with self.subTest(marker=marker, case='missing'):
                self.assert_invalid(snapshot_log().replace(marker + '\n', '', 1))
            with self.subTest(marker=marker, case='duplicate'):
                self.assert_invalid(snapshot_log() + marker + '\n')
        with self.subTest(case='duplicate pass'):
            self.assert_invalid(snapshot_log() + '[ENGINE-GPU-PASS] EFB avg=3 max=4\n')

    def test_request_id_generation_and_response_bounds_are_strict(self):
        cases = (
            ('zero request', REQUEST.replace('id=17', 'id=0'), SNAPSHOT.replace('id=17', 'id=0')),
            ('request exceeds uint64', REQUEST.replace('id=17', 'id=18446744073709551616'),
             SNAPSHOT.replace('id=17', 'id=18446744073709551616')),
            ('stale request', REQUEST, SNAPSHOT.replace('id=17', 'id=16')),
            ('future request', REQUEST, SNAPSHOT.replace('id=17', 'id=18')),
            ('zero generation', REQUEST, SNAPSHOT.replace('generation=3', 'generation=0')),
            ('even generation', REQUEST, SNAPSHOT.replace('generation=3', 'generation=4')),
            ('generation exceeds uint64', REQUEST,
             SNAPSHOT.replace('generation=3', 'generation=18446744073709551617')),
            ('request before CPU end', REQUEST.replace('4257', '4256'), SNAPSHOT),
            ('request after CPU end', REQUEST.replace('4257', '4258'), SNAPSHOT),
            ('response before request', REQUEST, SNAPSHOT.replace('4259', '4256')),
            ('response at deadline', REQUEST, SNAPSHOT.replace('4259', '4287')),
            ('response after deadline', REQUEST, SNAPSHOT.replace('4259', '4288')),
        )
        for label, requested, snapshot in cases:
            with self.subTest(case=label):
                self.assert_invalid(snapshot_log(request=requested, snapshot=snapshot))
        for tick in (4257, 4286):
            with self.subTest(valid_tick=tick):
                result = benchmark_result(
                    snapshot_log(snapshot=SNAPSHOT.replace('4259', str(tick))), snapshot_request())
                self.assertTrue(result['valid'], result['errors'])

    def test_record_order_keeps_all_means_in_the_same_snapshot(self):
        log = snapshot_log()
        first_pass = '[ENGINE-GPU-PASS] EFB avg=3 max=4'
        cases = (
            ('request before CPU completion', log.replace(END + '\n' + REQUEST, REQUEST + '\n' + END)),
            ('snapshot before request', log.replace(REQUEST + '\n' + SNAPSHOT, SNAPSHOT + '\n' + REQUEST)),
            ('header before snapshot', log.replace(SNAPSHOT + '\n' + GPU, GPU + '\n' + SNAPSHOT)),
            ('pass before header', log.replace(first_pass + '\n', '').replace(GPU.splitlines()[0], first_pass + '\n' + GPU.splitlines()[0])),
            ('pass before request', log.replace(first_pass + '\n', '').replace(REQUEST, first_pass + '\n' + REQUEST)),
            ('pass after shutdown', log.replace(first_pass + '\n', '') + first_pass + '\n'),
        )
        for label, corrupted in cases:
            with self.subTest(case=label):
                self.assert_invalid(corrupted)

    def test_malformed_numeric_tokens_and_extra_gpu_records_fail_closed(self):
        for token in ('-1', 'nan', 'NaN', 'inf', 'Infinity', '1..2', '.', '1e309', '9' * 400):
            with self.subTest(token=token, record='snapshot'):
                self.assert_invalid(snapshot_log(snapshot=SNAPSHOT.replace('ao_p95=10', 'ao_p95=' + token)))
            with self.subTest(token=token, record='header'):
                self.assert_invalid(snapshot_log(gpu=GPU.replace('between=0.1', 'between=' + token)))
            with self.subTest(token=token, record='pass'):
                self.assert_invalid(snapshot_log(gpu=GPU.replace('EFB avg=3', 'EFB avg=' + token)))
        for line in (
            '[ENGINE-GPU] status=broken',
            '[ENGINE-GPU-PASS] Unexpected avg=nan max=2',
            '[ENGINE-GPU-PASS] Unexpected avg=1 max=2 extra=7',
        ):
            with self.subTest(extra=line):
                self.assert_invalid(snapshot_log(gpu=GPU + '\n' + line))
        self.assert_invalid(snapshot_log(snapshot=SNAPSHOT + ' extra=7'))
        self.assert_invalid(snapshot_log(request=REQUEST + ' extra=7'))

    def test_distribution_order_means_and_physical_ranges_are_checked(self):
        for old, new in (
            ('span_median=9', 'span_median=13'),
            ('span_p95=12', 'span_p95=15'),
            ('span_max=14', 'span_max=9'),
            ('between_median=0.08', 'between_median=0.3'),
            ('between_p95=0.2', 'between_p95=0.5'),
            ('ao_median=8.5', 'ao_median=11'),
            ('ao_p95=10', 'ao_p95=13'),
            ('ao_mean=9', 'ao_mean=13'),
            ('ao_max=12', 'ao_max=6'),
            ('between_max=0.4', 'between_max=15'),
        ):
            with self.subTest(field=new):
                self.assert_invalid(snapshot_log(snapshot=SNAPSHOT.replace(old, new)))
        for replacement in (
            'between_median=10 between_p95=11 between_max=13',
            'between_median=0.08 between_p95=13 between_max=13',
        ):
            self.assert_invalid(snapshot_log(snapshot=SNAPSHOT.replace(
                'between_median=0.08 between_p95=0.2 between_max=0.4', replacement)))
        self.assert_invalid(snapshot_log(snapshot=SNAPSHOT.replace(
            'ao_mean=9 ao_median=8.5 ao_p95=10 ao_max=12',
            'ao_mean=9 ao_median=7 ao_p95=8 ao_max=8')))

    def test_overlapping_ao_pass_sums_may_exceed_the_whole_span(self):
        # Pass durations are summed independently. Their intervals may overlap,
        # so this sum is not bounded by the first-to-last timestamp span.
        gpu = GPU.replace('XeGTAO main avg=5 max=6', 'XeGTAO main avg=8 max=12')
        gpu = gpu.replace('avg=1 max=2', 'avg=2 max=3')
        snapshot = SNAPSHOT.replace(
            'ao_mean=9 ao_median=8.5 ao_p95=10 ao_max=12',
            'ao_mean=14 ao_median=11 ao_p95=15 ao_max=18')
        result = benchmark_result(snapshot_log(gpu=gpu, snapshot=snapshot), snapshot_request())
        self.assertTrue(result['valid'], result['errors'])
        distribution = result['gpu_distribution']
        self.assertGreater(distribution['ao_mean_ms'], result['gpu_final_window']['span_average_ms'])
        for measure in ('median', 'p95', 'max'):
            self.assertGreater(distribution['ao_' + measure + '_ms'],
                               distribution['span_' + measure + '_ms'])
        self.assertEqual(distribution['ao_mean_ms'], sum(
            row['average_ms'] for name, row in result['gpu_final_window']['passes'].items()
            if name == 'GTAO' or name.startswith(('AO ', 'XeGTAO '))))

    def test_ao_mean_uses_only_named_ao_rows_and_never_other_costs(self):
        gpu = GPU + '\n[ENGINE-GPU-PASS] Depth snapshot avg=0.2 max=0.5'
        result = benchmark_result(snapshot_log(gpu=gpu), snapshot_request())
        self.assertTrue(result['valid'], result['errors'])
        self.assertEqual(result['gpu_distribution']['ao_mean_ms'], 9)
        self.assertIn('Named-stage presence only', result['ao_stage_coverage']['scope'])
        for name in ('AO depth adapter', 'AO foreground', 'AO decal', 'AO foliage'):
            with self.subTest(name=name):
                extra = gpu + f'\n[ENGINE-GPU-PASS] {name} avg=0.2 max=0.5'
                expected_mean = 9.2
                result = benchmark_result(snapshot_log(
                    gpu=extra, snapshot=SNAPSHOT.replace('ao_mean=9', f'ao_mean={expected_mean}')),
                    snapshot_request())
                self.assertTrue(result['valid'], result['errors'])

    def test_partial_historical_xegtao_runs_fail_closed_for_each_required_stage(self):
        required = (
            'AO depth preparation', 'XeGTAO canonical depth', 'XeGTAO depth MIPs',
            'XeGTAO normals', 'XeGTAO main', 'XeGTAO denoise', 'XeGTAO final denoise',
        )
        for stage in required:
            with self.subTest(stage=stage):
                gpu = '\n'.join(line for line in GPU.splitlines() if stage not in line)
                result = self.assert_invalid(snapshot_log(gpu=gpu))
                self.assertIn(stage, result['ao_stage_coverage']['missing'])

    def test_invalid_superseded_or_timeout_markers_override_complete_data(self):
        for diagnostic in ('GPU snapshot unsupported or disabled',
                           'GPU snapshot superseded id=17', 'GPU snapshot timed out id=17'):
            with self.subTest(diagnostic=diagnostic):
                self.assert_invalid(snapshot_log() + '[ANDROID-AO-BENCH-INVALID] ' + diagnostic + '\n')


class AndroidAoSnapshotProvenanceTests(unittest.TestCase):
    def test_attested_version_is_preserved_and_old_provenance_defaults_to_v1(self):
        with tempfile.TemporaryDirectory(prefix='ao-snapshot-attestation-') as temporary:
            root = Path(temporary)
            native = root / 'build' / 'probe' / 'stripped' / 'libmp6game.so'
            native.parent.mkdir(parents=True)
            native.write_bytes(b'CPU-only provenance fixture; not a native library')
            wrapper_path = root / 'build' / 'wrapper.json'
            wrapper_path.write_text(json.dumps(dict(
                release_asset=False, sha256='installed-apk', native_override=str(native))))
            probe_path = native.parent.parent / 'provenance.json'
            probe = dict(release_asset=False, benchmark='fixed-seed-board',
                         stripped_sha256=hashlib.sha256(native.read_bytes()).hexdigest(),
                         warmup_board_age=600, sample_count=1200, last_board_age=1799)
            missing = object()
            for version in (missing, 1, 2, 0, 3, True, '2', 2.0, None):
                with self.subTest(version=version), mock.patch.object(device_profile, 'ROOT', root):
                    candidate = dict(probe)
                    if version is not missing:
                        candidate['gpu_timing_version'] = version
                    probe_path.write_text(json.dumps(candidate))
                    if (type(version) is int and version in (1, 2)) or version is missing:
                        result = device_profile.verify_board_benchmark(wrapper_path, 'installed-apk')
                        self.assertEqual(result['gpu_timing_version'], 1 if version is missing else version)
                        self.assertEqual(result['last_board_age'], 1799)
                    else:
                        with self.assertRaises(RuntimeError):
                            device_profile.verify_board_benchmark(wrapper_path, 'installed-apk')

    def test_builder_attests_v2_in_private_benchmark_provenance(self):
        # Inspect the builder rather than invoking its native link path.
        source = (ROOT / 'tests/integration/build_android_ao_benchmark.py').read_text()
        self.assertRegex(source, r'provenance\s*=\s*dict\([\s\S]*?gpu_timing_version\s*=\s*2')


FAKE_HEADER = r'''
#ifndef FAKE_AURORA_GPU_TIMING_SNAPSHOT_H
#define FAKE_AURORA_GPU_TIMING_SNAPSHOT_H
#include <stdbool.h>
#include <stdint.h>
#define AURORA_GPU_STAT_ROWS 32
typedef struct {
    char name[64];
    double averageMs, maxMs;
} AuroraGpuStatRow;
typedef struct {
    uint32_t status, sampleCount, rowCount, droppedFrames;
    double spanAverageMs, spanMaxMs, betweenAverageMs, betweenMaxMs;
    AuroraGpuStatRow rows[AURORA_GPU_STAT_ROWS];
} AuroraGpuStats;
typedef struct {
    uint64_t requestId, generation;
    AuroraGpuStats stats;
    double spanMedianMs, spanP95Ms, betweenMedianMs, betweenP95Ms;
    double aoAverageMs, aoMaxMs, aoMedianMs, aoP95Ms;
} AuroraGpuTimingSnapshot;
uint64_t aurora_gpu_timing_snapshot_request(void);
bool aurora_gpu_timing_snapshot_read(uint64_t request, AuroraGpuTimingSnapshot *out);
#endif
'''

FAKE_BRIDGE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned age, g_phSamples, requestCalls, readCalls;
static long mp6_tick_count, mp6_max_ticks, requestTick, lastReadTick;
static long long tEntry, tPrevReturn, tEndFrame, tSeal, tThrottleOut, g_phLastReturnNs;
static int mode;
unsigned mp6_diag_board_ticks(void) { return age; }
int mp6_diag_board_no(void) { return 0; }
static void aurora_shutdown(void) {}
static void mp6_clean_shutdown_exit(void) {
    aurora_shutdown();
    exit(0);
}
uint64_t aurora_gpu_timing_snapshot_request(void) {
    ++requestCalls;
    assert(requestCalls == 1);
    requestTick = mp6_tick_count;
    return mode == 4 ? 0 : 17;
}
bool aurora_gpu_timing_snapshot_read(uint64_t request, AuroraGpuTimingSnapshot *out) {
    assert(request == 17);
    assert(lastReadTick != mp6_tick_count); /* At most one non-blocking poll per tick. */
    lastReadTick = mp6_tick_count;
    ++readCalls;
    if (mode == 2 || mode == 5 || (mode == 1 && mp6_tick_count < requestTick + 2) ||
            (mode == 3 && mp6_tick_count < requestTick + 30)) return false;
    memset(out, 0, sizeof(*out));
    out->requestId = request;
    out->generation = 3;
    out->stats.status = 2;
    out->stats.sampleCount = 120;
    out->stats.rowCount = 1;
    out->stats.spanAverageMs = 3;
    out->stats.spanMaxMs = 4;
    strcpy(out->stats.rows[0].name, "EFB");
    out->stats.rows[0].averageMs = 3;
    out->stats.rows[0].maxMs = 4;
    out->spanMedianMs = 3;
    out->spanP95Ms = 4;
    return true;
}
static void bridge_frame(void) {
    if (tEntry > 0) g_phSamples++;
}
int main(int argc, char **argv) {
    mode = argc > 1 ? atoi(argv[1]) : 0;
    for (age = 1; age <= 400; ++age) {
        mp6_tick_count = 2818 + age;
        tPrevReturn = (long long)mp6_tick_count * 20000000;
        tEntry = tPrevReturn + 1000000;
        tEndFrame = tEntry + 2000000;
        tSeal = tEndFrame + 3000000;
        tThrottleOut = tSeal + 1000000;
        g_phLastReturnNs = tPrevReturn + 17000000;
        bridge_frame();
        if (mp6_max_ticks && mp6_tick_count >= mp6_max_ticks) break;
    }
    printf("requests=%u reads=%u request_tick=%ld last_read_tick=%ld last_tick=%ld limit=%ld samples=%u\n",
           requestCalls, readCalls, requestTick, lastReadTick,
           mp6_tick_count, mp6_max_ticks, g_phSamples);
    fprintf(stderr, "[BOOT] reached %ld VIWaitForRetrace ticks (limit %ld) -- shutting down Aurora, exiting 0\n",
            mp6_tick_count, mp6_max_ticks);
    mp6_clean_shutdown_exit();
    return 99;
}
'''


class AndroidAoSnapshotSourceTests(unittest.TestCase):
    def test_transform_uses_typed_nonblocking_request_and_unchanged_window(self):
        transformed = transform_source('bridge', FAKE_BRIDGE, 120, 120)
        inserted = transformed[transformed.index('    if (tEntry > 0) g_phSamples++;'):]
        inserted = inserted[:inserted.index('int main(')]
        self.assertEqual(inserted.count('aurora_gpu_timing_snapshot_request()'), 1)
        self.assertEqual(inserted.count('aurora_gpu_timing_snapshot_read('), 1)
        self.assertIn('#include <aurora/gpu_timing_snapshot.h>', transformed)
        self.assertIn('AuroraGpuTimingSnapshot qaSnapshot;', inserted)
        self.assertIn('AuroraGpuStats qaGpu = qaSnapshot.stats;', inserted)
        self.assertIn('qaGpuRequestTick = mp6_tick_count;', inserted)
        self.assertIn('qaBoardAge >= 120u && qaBoardAge < 240u', inserted)
        self.assertIn('mp6_max_ticks = mp6_tick_count + 60;', inserted)
        self.assertNotRegex(inserted, r'\b(?:while|sleep|usleep|qsort|aurora_gpu_stats_read)\s*\(')

    @classmethod
    def setUpClass(cls):
        compiler = ROOT.parent / 'toolchain/zig-x86_64-windows-0.16.0/zig.exe'
        if compiler.is_file():
            command = [str(compiler), 'cc']
        elif shutil.which('cc'):
            command = [shutil.which('cc')]
        else:
            cls.executable = None
            return
        cls.temporary = tempfile.TemporaryDirectory(prefix='ao-snapshot-cpu-')
        cls.addClassCleanup(cls.temporary.cleanup)
        folder = Path(cls.temporary.name)
        (folder / 'aurora').mkdir()
        (folder / 'aurora/gpu_timing_snapshot.h').write_text(FAKE_HEADER)
        source = folder / 'bridge.c'
        source.write_text(transform_source('bridge', FAKE_BRIDGE, 120, 120))
        cls.executable = folder / 'bridge.exe'
        result = subprocess.run(command + ['-std=c11', '-O1', '-UNDEBUG', '-I', str(folder),
                                          str(source), '-o', str(cls.executable)],
                                capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError('CPU snapshot harness compilation failed:\n' + result.stderr)

    def run_fixture(self, mode):
        if self.executable is None:
            self.skipTest('a C compiler is required for the CPU-only source fixture')
        result = subprocess.run([str(self.executable), str(mode)], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = dict((name, int(value)) for name, value in re.findall(r'(\w+)=(\d+)', result.stdout))
        self.assertEqual(fields['requests'], 1)
        self.assertEqual(fields['request_tick'], 3057)
        self.assertEqual(fields['last_tick'], 3117)
        self.assertEqual(fields['limit'], 3117)
        self.assertEqual(fields['samples'], 299)
        self.assertEqual(result.stderr.count('[ANDROID-AO-BENCH-BEGIN]'), 1)
        self.assertEqual(result.stderr.count('[ANDROID-AO-BENCH]'), 1)
        self.assertEqual(result.stderr.count('[ANDROID-AO-GPU-REQUEST]'), 1)
        self.assertIn('ticks=120 first_age=120 last_age=239 global_tick=3057', result.stderr)
        self.assertIn('game=1.000000 submit=2.000000 seal=3.000000 post=10.000000 frame=17.000000', result.stderr)
        return result.stderr, fields

    def test_compiled_fixture_completes_immediately_or_after_nonblocking_polls(self):
        for mode, expected_reads, snapshot_tick in ((0, 1, 3057), (1, 3, 3059)):
            with self.subTest(mode=mode):
                log, fields = self.run_fixture(mode)
                self.assertEqual(fields['reads'], expected_reads)
                self.assertEqual(fields['last_read_tick'], snapshot_tick)
                self.assertEqual(log.count('[ENGINE-GPU-SNAPSHOT]'), 1)
                self.assertEqual(log.count('[ENGINE-GPU]'), 1)
                request = make_request(implementation='legacy', ao=0, xegtao=0)
                request['board_benchmark'] = dict(warmup_board_age=120, sample_count=120,
                                                last_board_age=239, gpu_timing_version=2)
                parsed = benchmark_result(log, request)
                self.assertTrue(parsed['valid'], parsed['errors'])
                self.assertEqual(parsed['gpu_distribution']['ao_p95_ms'], 0)

    def test_compiled_fixture_rejects_timeout_boundary_and_superseded_requests(self):
        for mode in (2, 3, 5):
            with self.subTest(mode=mode):
                log, fields = self.run_fixture(mode)
                self.assertIn('[ANDROID-AO-BENCH-INVALID] GPU snapshot timed out id=17', log)
                self.assertNotIn('[ENGINE-GPU-SNAPSHOT]', log)
                self.assertEqual(fields['reads'], 30)
                self.assertEqual(fields['last_read_tick'], 3086)

    def test_compiled_fixture_rejects_unsupported_request_without_polling(self):
        log, fields = self.run_fixture(4)
        self.assertIn('[ANDROID-AO-BENCH-INVALID] GPU snapshot unsupported or disabled', log)
        self.assertNotIn('[ENGINE-GPU-SNAPSHOT]', log)
        self.assertEqual(fields['reads'], 0)


if __name__ == '__main__':
    unittest.main()
