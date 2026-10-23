"""Device ownership, bounded measurement and cleanup guards; no ADB is invoked."""
import hashlib
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

INTEGRATION = Path(__file__).resolve().parent/'integration'
sys.path.insert(0, str(INTEGRATION))
import android_device_profile as profile
from finish_android_profile import canonical_session_path


class AndroidProfileSafety(unittest.TestCase):
    def test_screenshot_run_retains_execution_proof_but_cannot_count_as_comparison(self):
        helper = types.SimpleNamespace(benchmark_result=lambda log, request: {'valid': True, 'errors': []})
        with tempfile.TemporaryDirectory() as tmp, patch.dict(sys.modules, android_ao_benchmark_metrics=helper):
            run = Path(tmp)
            self.assertTrue(profile.benchmark_run_evidence('complete', {}, run)['performance_comparison_eligible'])
            (run/'screen-1.png').write_bytes(b'capture')
            result = profile.benchmark_run_evidence('complete', {}, run)
            self.assertTrue(result['valid'])
            self.assertFalse(result['performance_comparison_eligible'])
            self.assertFalse(result['uninterrupted_measurement'])
            self.assertEqual(result['screen_captures'], ['screen-1.png'])

    def test_attested_timing_run_refuses_screenshot_before_device_capture(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            output, _, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
            run = output/'run'
            run.mkdir(parents=True)
            (output/'session-owner.json').write_text(json.dumps(owner))
            (output/'original-manifest.json').write_text('{}')
            (run/'request.json').write_text(json.dumps(dict(
                serial='phone', pid='123', board_benchmark={'sample_count': 1200})))
            argv = ['profile', 'capture', '--serial', 'phone', '--model', 'SM-S906E',
                    '--session', 'safe-session', '--name', 'run']
            with patch.object(sys, 'argv', argv), patch.object(profile, 'shell', return_value='SM-S906E'), \
                 patch.object(profile, 'adb') as adb:
                with self.assertRaisesRegex(RuntimeError, 'separate visual-only run'):
                    profile.main()
                adb.assert_not_called()

    def test_natural_exit_requires_valid_attested_benchmark(self):
        helper = types.SimpleNamespace(benchmark_result=lambda log, request: {'valid': log == 'complete'})
        with patch.dict(sys.modules, android_ao_benchmark_metrics=helper):
            self.assertEqual(profile.completed_exit_reason('complete', {}), 'owned process exited')
            request = {'board_benchmark': {'sample_count': 1200}}
            self.assertEqual(profile.completed_exit_reason('incomplete', request), 'owned process exited')
            self.assertEqual(profile.completed_exit_reason('complete', request), 'attested board benchmark completed')

    def test_attested_completion_preserves_thermal_window_and_does_not_stop_other_process(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            output, _, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
            run = output/'run'
            run.mkdir(parents=True)
            (output/'session-owner.json').write_text(json.dumps(owner))
            (output/'original-manifest.json').write_text('{}')
            (run/'request.json').write_text(json.dumps(dict(
                serial='phone', pid='123', board_benchmark={'sample_count': 1200})))
            pid_reads = 0
            def fake_shell(serial, *args, **kwargs):
                nonlocal pid_reads
                if args[0] == 'getprop': return 'SM-S906E'
                if args[0] == 'pidof':
                    pid_reads += 1
                    return '123' if pid_reads == 1 else '456'
                if args[:2] == ('dumpsys', 'thermalservice'): return 'Thermal Status: 0\n'
                if args[:2] == ('dumpsys', 'activity'):
                    return 'topResumedActivity=ActivityRecord{ com.mp6.game/ }'
                raise AssertionError(('unowned process must not be affected', args))
            helper = types.SimpleNamespace(benchmark_result=lambda log, request: {'valid': True, 'errors': []})
            argv = ['profile', 'measure', '--serial', 'phone', '--model', 'SM-S906E',
                    '--session', 'safe-session', '--name', 'run', '--seconds', '10']
            with patch.object(sys, 'argv', argv), patch.object(profile, 'shell', fake_shell), \
                 patch.object(profile, 'adb', return_value=subprocess.CompletedProcess([], 0, b'complete')), \
                 patch.dict(sys.modules, android_ao_benchmark_metrics=helper), \
                 patch.object(profile.time, 'sleep') as sleep:
                profile.main()
                sleep.assert_not_called()
            window = json.loads(next(run.glob('thermal-window-*.json')).read_text())
            self.assertEqual(window['reason'], 'attested board benchmark completed')
            self.assertTrue(window['thermal_comparison_valid'])
            self.assertTrue(json.loads((run/'benchmark-result.json').read_text())['valid'])

    def test_upstream_ao_launch_is_explicit_and_recorded(self):
        for requested, unlocked, resolution in ((None, 1, 'full'), ('legacy', 1, 'full'),
                                                 ('xegtao', 1, 'full'), ('xegtao', 0, 'full'),
                                                 ('xegtao', 1, 'half'), ('xegtao', 0, 'half')):
            with self.subTest(implementation=requested, unlocked=unlocked, resolution=resolution), tempfile.TemporaryDirectory() as tmp, \
                 patch.object(profile, 'ROOT', Path(tmp)):
                output, _, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
                output.mkdir(parents=True)
                (output/'session-owner.json').write_text(json.dumps(owner))
                (output/'original-manifest.json').write_text('{}')
                launch = []
                def fake_shell(serial, *args, **kwargs):
                    if args[0] == 'getprop': return 'SM-S906E'
                    if args[0] == 'pidof': return '123' if launch else ''
                    if args[:2] == ('dumpsys', 'thermalservice'): return 'Thermal Status: 0\n'
                    if args[:2] == ('pm', 'path'): return 'package:/data/app/mock/base.apk'
                    if args[0] == 'sha256sum': return 'abc  /data/app/mock/base.apk'
                    if args[:2] == ('am', 'start'):
                        launch.append(args)
                        return 'Success'
                    raise AssertionError(args)
                argv = ['profile', 'launch', '--serial', 'phone', '--model', 'SM-S906E',
                        '--session', 'safe-session', '--name', 'run', '--ao', '2']
                if requested is not None: argv += ['--implementation', requested]
                if not unlocked: argv += ['--unlocked-fps', '0']
                if resolution != 'full': argv += ['--xegtao-resolution', resolution]
                with patch.object(sys, 'argv', argv), patch.object(profile, 'shell', fake_shell):
                    profile.main()
                request = json.loads((output/'run/request.json').read_text())
                self.assertEqual(request['requested_ao_implementation'], requested or 'legacy')
                self.assertIn('MP6_AO_XEGTAO='+str(int(requested == 'xegtao')), request['args'])
                self.assertIn('MP6_AO_XEGTAO='+str(int(requested == 'xegtao')), launch[0][-1].split())
                self.assertEqual(request['requested_xegtao_policy'],
                                 dict(quality='high', denoise_passes=2, resolution=resolution))
                self.assertIn('MP6_AO_XEGTAO_QUALITY=high', request['args'])
                self.assertIn('MP6_AO_XEGTAO_DENOISE=2', request['args'])
                self.assertIn('MP6_AO_XEGTAO_RESOLUTION='+resolution, request['args'])
                self.assertIn('MP6_AO_XEGTAO_RESOLUTION='+resolution, launch[0][-1].split())
                self.assertFalse(request['requested_xegtao_prepared_canonical'])
                self.assertNotIn('MP6_AO_XEGTAO_PREPARED_CANONICAL=0', request['args'])
                self.assertNotIn('MP6_AO_XEGTAO_PREPARED_CANONICAL=1', request['args'])
                self.assertNotIn('effective_resolution', request['requested_xegtao_policy'])
                self.assertIn('MP6_UNLOCKED_FPS='+str(unlocked), request['args'])
                self.assertEqual(request['presentation_workload'],
                                 'interpolated high refresh' if unlocked else 'simulation frames only')

    def test_prepared_canonical_launch_rejects_invalid_combinations_before_device_access(self):
        cases = (
            ('legacy implementation', ['--xegtao-prepared-canonical']),
            ('AO off', ['--ao', '0', '--implementation', 'xegtao', '--xegtao-prepared-canonical']),
            ('half resolution', ['--ao', '2', '--implementation', 'xegtao',
                                  '--xegtao-resolution', 'half', '--xegtao-prepared-canonical']),
        )
        for label, options in cases:
            argv = ['profile', 'launch', '--serial', 'phone'] + options
            with self.subTest(case=label), patch.object(sys, 'argv', argv), \
                 patch.object(profile, 'shell') as shell:
                with self.assertRaises(SystemExit):
                    profile.main()
                shell.assert_not_called()

    def test_prepared_canonical_launch_records_true_and_emits_only_exact_one(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            output, _, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
            output.mkdir(parents=True)
            (output/'session-owner.json').write_text(json.dumps(owner))
            (output/'original-manifest.json').write_text('{}')
            launch = []

            def fake_shell(serial, *args, **kwargs):
                if args[0] == 'getprop': return 'SM-S906E'
                if args[0] == 'pidof': return '123' if launch else ''
                if args[:2] == ('dumpsys', 'thermalservice'): return 'Thermal Status: 0\n'
                if args[:2] == ('pm', 'path'): return 'package:/data/app/mock/base.apk'
                if args[0] == 'sha256sum': return 'abc  /data/app/mock/base.apk'
                if args[:2] == ('am', 'start'):
                    launch.append(args)
                    return 'Success'
                raise AssertionError(args)

            argv = ['profile', 'launch', '--serial', 'phone', '--model', 'SM-S906E',
                    '--session', 'safe-session', '--name', 'canonical', '--ao', '2',
                    '--implementation', 'xegtao', '--xegtao-prepared-canonical']
            with patch.object(sys, 'argv', argv), patch.object(profile, 'shell', fake_shell):
                profile.main()

            request = json.loads((output/'canonical/request.json').read_text())
            self.assertTrue(request['requested_xegtao_prepared_canonical'])
            self.assertEqual(request['args'].count('MP6_AO_XEGTAO_PREPARED_CANONICAL=1'), 1)
            self.assertNotIn('MP6_AO_XEGTAO_PREPARED_CANONICAL=0', request['args'])
            launch_tokens = launch[0][-1].split()
            self.assertEqual(launch_tokens.count('MP6_AO_XEGTAO_PREPARED_CANONICAL=1'), 1)
            self.assertNotIn('MP6_AO_XEGTAO_PREPARED_CANONICAL=0', launch_tokens)

    def test_ultra_three_pass_launch_is_explicit_and_recorded(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            output, _, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
            output.mkdir(parents=True)
            (output/'session-owner.json').write_text(json.dumps(owner))
            (output/'original-manifest.json').write_text('{}')
            launch = []

            def fake_shell(serial, *args, **kwargs):
                if args[0] == 'getprop': return 'SM-S906E'
                if args[0] == 'pidof': return '123' if launch else ''
                if args[:2] == ('dumpsys', 'thermalservice'): return 'Thermal Status: 0\n'
                if args[:2] == ('pm', 'path'): return 'package:/data/app/mock/base.apk'
                if args[0] == 'sha256sum': return 'abc  /data/app/mock/base.apk'
                if args[:2] == ('am', 'start'):
                    launch.append(args)
                    return 'Success'
                raise AssertionError(args)

            argv = [
                'profile', 'launch', '--serial', 'phone', '--model', 'SM-S906E',
                '--session', 'safe-session', '--name', 'run', '--ao', '2',
                '--implementation', 'xegtao', '--xegtao-quality', 'ultra',
                '--xegtao-denoise', '3', '--xegtao-resolution', 'half',
            ]
            with patch.object(sys, 'argv', argv), patch.object(profile, 'shell', fake_shell):
                profile.main()

            request = json.loads((output/'run/request.json').read_text())
            expected = dict(quality='ultra', denoise_passes=3, resolution='half')
            self.assertEqual(request['requested_xegtao_policy'], expected)
            for token in (
                'MP6_AO_XEGTAO=1', 'MP6_AO_XEGTAO_QUALITY=ultra',
                'MP6_AO_XEGTAO_DENOISE=3', 'MP6_AO_XEGTAO_RESOLUTION=half',
            ):
                self.assertIn(token, request['args'])
                self.assertIn(token, launch[0][-1].split())

    def test_board_probe_requires_matching_installed_apk_native_and_window(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            root = Path(tmp)/'build'
            native = root/'probe/stripped/libmp6game.so'
            native.parent.mkdir(parents=True)
            native.write_bytes(b'fixed seed benchmark')
            probe_path = native.parent.parent/'provenance.json'
            probe = dict(release_asset=False, benchmark='fixed-seed-board',
                         warmup_board_age=600, sample_count=1200, last_board_age=1799,
                         stripped_sha256=hashlib.sha256(native.read_bytes()).hexdigest())
            probe_path.write_text(json.dumps(probe))
            wrapper_path = root/'apk/provenance.json'
            wrapper_path.parent.mkdir()
            wrapper = dict(release_asset=False, native_override=str(native), sha256='installed')
            wrapper_path.write_text(json.dumps(wrapper))
            result = profile.verify_board_benchmark(wrapper_path, 'installed')
            self.assertEqual(result['last_board_age'], 1799)
            with self.assertRaises(RuntimeError):
                profile.verify_board_benchmark(wrapper_path, 'other-apk')
            for key, value in (('release_asset', True), ('sample_count', 1199),
                               ('benchmark', 'unknown'), ('warmup_board_age', True)):
                probe_path.write_text(json.dumps({**probe, key: value}))
                with self.assertRaises(RuntimeError):
                    profile.verify_board_benchmark(wrapper_path, 'installed')
            probe_path.write_text(json.dumps(probe))
            native.write_bytes(b'stale library')
            with self.assertRaises(RuntimeError):
                profile.verify_board_benchmark(wrapper_path, 'installed')

    def test_policy_overrides_cannot_be_labeled_legacy(self):
        for options in (['--xegtao-quality', 'medium'], ['--xegtao-quality', 'ultra'],
                        ['--xegtao-denoise', '1'], ['--xegtao-denoise', '3'],
                        ['--xegtao-resolution', 'half']):
            argv = ['profile', 'launch', '--serial', 'phone', *options]
            with self.subTest(options=options), patch.object(sys, 'argv', argv), patch.object(profile, 'shell') as shell:
                with self.assertRaises(SystemExit):
                    profile.main()
                shell.assert_not_called()

    def test_invalid_xegtao_quality_and_denoise_fail_before_device_access(self):
        for option, value in (('--xegtao-quality', 'cinematic'), ('--xegtao-denoise', '4')):
            argv = ['profile', 'launch', '--serial', 'phone', '--implementation', 'xegtao', option, value]
            with self.subTest(option=option), patch.object(sys, 'argv', argv), patch.object(profile, 'shell') as shell:
                with self.assertRaises(SystemExit):
                    profile.main()
                shell.assert_not_called()

    def test_invalid_resolution_fails_before_device_access(self):
        for resolution in ('quarter', 'Half', ''):
            argv = ['profile', 'launch', '--serial', 'phone', '--implementation', 'xegtao',
                    '--xegtao-resolution', resolution]
            with self.subTest(resolution=resolution), patch.object(sys, 'argv', argv), \
                    patch.object(profile, 'shell') as shell:
                with self.assertRaises(SystemExit):
                    profile.main()
                shell.assert_not_called()

    def test_composite_ablation_requires_exact_local_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            native = output/'probe/stripped/libmp6game.so'
            native.parent.mkdir(parents=True)
            native.write_bytes(b'local diagnostic')
            (output/'apk-probe').mkdir()
            probe = dict(release_asset=False, stripped_sha256=hashlib.sha256(native.read_bytes()).hexdigest())
            wrapper = dict(release_asset=False, native_override=str(native), sha256='installed')
            (output/'probe/provenance.json').write_text(json.dumps(probe))
            (output/'apk-probe/provenance.json').write_text(json.dumps(wrapper))
            profile.verify_composite_probe(output, 'installed')
            with self.assertRaises(RuntimeError):
                profile.verify_composite_probe(output, 'other APK')
            for key, value in (('release_asset', True), ('native_override', str(output/'other.so'))):
                (output/'apk-probe/provenance.json').write_text(json.dumps({**wrapper, key: value}))
                with self.assertRaises(RuntimeError):
                    profile.verify_composite_probe(output, 'installed')
            (output/'apk-probe/provenance.json').write_text(json.dumps(wrapper))
            native.write_bytes(b'replaced')
            with self.assertRaises(RuntimeError):
                profile.verify_composite_probe(output, 'installed')

    def test_legacy_tablet_session_is_not_reused_on_phone(self):
        with self.assertRaises(ValueError):
            profile.session_paths('RZCTB00SF3W', 'SM-S906E', 'tablet-performance-20260910')

    def test_session_names_cannot_escape_build_or_device_root(self):
        for name in ('../other', '.', '', 'a/b', 'a\\b', '/data', 'x;rm'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                profile.session_paths('phone', 'SM-S906E', name)

    def test_existing_owner_must_match_physical_device(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            output, device, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
            output.mkdir(parents=True)
            (output/'session-owner.json').write_text(json.dumps(owner))
            self.assertEqual(profile.session_paths('phone', 'SM-S906E', 'safe-session')[1], device)
            with self.assertRaises(ValueError):
                profile.session_paths('another-phone', 'SM-S906E', 'safe-session')

    def test_cleanup_requires_exact_canonical_target(self):
        target = '/data/user/0/com.mp6.game/files/perf-test'
        self.assertTrue(canonical_session_path(target, target))
        self.assertTrue(canonical_session_path('/data/data/com.mp6.game/files/perf-test', target))
        for bad in ('/data/user/0/com.mp6.game/files', target+'/child', target+'-other', '/sdcard'):
            self.assertFalse(canonical_session_path(bad, target))

    def test_thermal_status_is_required(self):
        self.assertEqual(profile.thermal_status('x\nThermal Status: 2\n'), 2)
        for bad in ('', 'Thermal Status: unknown', 'Thermal Status: -1'):
            with self.assertRaises(ValueError):
                profile.thermal_status(bad)

    def test_temperature_limit_rejects_unbounded_values(self):
        self.assertEqual(profile.temperature_limit('37.5'), 37.5)
        for value in ('nan', 'inf', '-inf', '-1', '81', 'not a temperature'):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                profile.temperature_limit(value)

    def test_temperature_admission_checks_sensor_and_status(self):
        cool = ('Thermal Status: 0\n'
                'Temperature{mValue=36.8, mType=0, mName=AP, mStatus=0}\n'
                'Temperature{mValue=35.9, mType=3, mName=SKIN, mStatus=0}\n')
        limits = dict(AP=37.3, SKIN=36.3)
        profile.verify_start_thermal(cool, 0, limits)
        profile.verify_start_thermal('Thermal Status: 0\n', 0, dict(AP=None, SKIN=None))
        for bad in (cool.replace('36.8', '38.5'), cool.replace('35.9', '37.1'),
                    cool.replace('AP,', 'AP_OTHER,'), cool.replace('36.8', 'NaN'),
                    cool.replace('36.8', 'bad'), cool.replace('Thermal Status: 0', 'Thermal Status: 1'),
                    cool+'Temperature{mValue=38.5, mType=0, mName=AP, mStatus=0}\n'):
            with self.subTest(report=bad), self.assertRaises(RuntimeError):
                profile.verify_start_thermal(bad, 0, limits)

    def test_warm_start_refused_before_launch_or_resume(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            output, _, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
            output.mkdir(parents=True)
            (output/'session-owner.json').write_text(json.dumps(owner))
            (output/'original-manifest.json').write_text('{}')
            for action in ('launch', 'resume'):
                if action == 'resume':
                    (output/'run').mkdir()
                    (output/'run/request.json').write_text(json.dumps(dict(serial='phone', pid='123')))
                def fake_shell(serial, *args, **kwargs):
                    if args[0] == 'getprop': return 'SM-S906E'
                    if args[0] == 'pidof': return '' if action == 'launch' else '123'
                    if args[:2] == ('dumpsys', 'thermalservice'):
                        return 'Thermal Status: 0\nTemperature{mValue=38.5, mType=0, mName=AP, mStatus=0}\n'
                    raise AssertionError(('unexpected operation', args))
                argv = ['profile', action, '--serial', 'phone', '--model', 'SM-S906E',
                        '--session', 'safe-session', '--name', 'run', '--max-start-ap-c', '37.3']
                with patch.object(sys, 'argv', argv), patch.object(profile, 'shell', fake_shell), \
                     patch.object(profile, 'adb') as adb:
                    with self.assertRaisesRegex(RuntimeError, 'cool to <= 37.3'):
                        profile.main()
                    adb.assert_not_called()
                if action == 'launch': self.assertFalse((output/'run').exists())
                else: self.assertFalse(list((output/'run').glob('resume-*.json')))

    def check_hot_window(self, pause=False, foreground=True, replaced=False, strict=False):
        with tempfile.TemporaryDirectory() as tmp, patch.object(profile, 'ROOT', Path(tmp)):
            output, _, owner = profile.session_paths('phone', 'SM-S906E', 'safe-session')
            run = output/'run'
            run.mkdir(parents=True)
            (output/'session-owner.json').write_text(json.dumps(owner))
            (output/'original-manifest.json').write_text('{}')
            (run/'request.json').write_text(json.dumps(dict(serial='phone', pid='123')))
            state = {'pid': '123'}
            calls = []
            pid_reads = 0
            def fake_shell(serial, *args, **kwargs):
                nonlocal pid_reads
                calls.append(args)
                if args[0] == 'getprop': return 'SM-S906E'
                if args[0] == 'pidof':
                    pid_reads += 1
                    if replaced and pid_reads > 1: state['pid'] = '456'
                    return state['pid']
                if args[:2] == ('dumpsys', 'thermalservice'):
                    return 'Thermal Status: 1\n' if strict else 'Thermal Status: 2\n'
                if args[:2] == ('dumpsys', 'activity'):
                    return 'topResumedActivity=ActivityRecord{ '+('com.mp6.game/' if foreground else 'other.app/')+' }'
                if args == ('input', 'keyevent', 'KEYCODE_HOME'): return ''
                if args[:2] == ('am', 'force-stop'):
                    state['pid'] = ''
                    return ''
                raise AssertionError(args)
            argv = ['profile', 'measure', '--serial', 'phone', '--model', 'SM-S906E',
                    '--session', 'safe-session', '--name', 'run', '--seconds', '10']
            if pause: argv.append('--pause')
            if strict: argv += ['--max-window-status', '0']
            with patch.object(sys, 'argv', argv), patch.object(profile, 'shell', fake_shell), \
                 patch.object(profile, 'adb', return_value=subprocess.CompletedProcess([], 0, b'log')), \
                 patch.object(profile.time, 'sleep') as sleep:
                if not foreground:
                    with self.assertRaises(RuntimeError): profile.main()
                else:
                    profile.main()
                sleep.assert_not_called()
            if not foreground:
                self.assertEqual(state['pid'], '123')
                self.assertFalse(any(c[0] in ('am', 'input') for c in calls))
                self.assertFalse(list(run.glob('thermal-window-*.json')))
                self.assertFalse((run/'logcat.txt').exists())
                return
            self.assertEqual(state['pid'], '456' if replaced else '123' if pause else '')
            self.assertEqual(sum(c[:2] == ('am', 'force-stop') for c in calls), int(not pause and not replaced))
            self.assertEqual(sum(c[:2] == ('input', 'keyevent') for c in calls), int(pause and foreground and not replaced))
            windows = list(run.glob('thermal-window-*.json'))
            self.assertEqual(len(windows), 1)
            window = json.loads(windows[0].read_text())
            self.assertIn('thermal status', window['reason'])
            self.assertFalse(window['thermal_comparison_valid'])
            self.assertEqual(window['max_allowed_status'], 0 if strict else 1)
            if not (pause and not foreground and not replaced):
                self.assertEqual((run/'logcat.txt').read_bytes(), b'log')

    def test_hot_measurement_stops_only_owned_process_and_retains_evidence(self):
        self.check_hot_window()

    def test_strict_window_stops_at_first_light_thermal_status(self):
        self.check_hot_window(strict=True)
        self.check_hot_window(strict=True, pause=True)

    def test_pause_preserves_loaded_game(self):
        self.check_hot_window(pause=True)

    def test_never_background_another_foreground_app(self):
        self.check_hot_window(pause=True, foreground=False)

    def test_background_measurement_refused_without_stopping_app(self):
        self.check_hot_window(foreground=False)

    def test_replaced_process_is_neither_stopped_nor_backgrounded(self):
        self.check_hot_window(replaced=True)
        self.check_hot_window(pause=True, replaced=True)


if __name__ == '__main__':
    unittest.main()
