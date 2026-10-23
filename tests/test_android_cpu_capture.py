"""Focused safety and admission tests for the bounded Android CPU capture."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent/'integration'))
import capture_android_cpu_profile as capture


class AndroidCpuCapture(unittest.TestCase):
    def test_exact_models_are_admitted_and_other_models_are_not(self):
        self.assertEqual(capture.SUPPORTED_MODELS, ('SM-X920', 'SM-S906E'))
        argv = ['capture', '--serial', 'tablet', '--model', 'SM-T999',
                '--session', 'session', '--name', 'run']
        with patch.object(sys, 'argv', argv), patch.object(capture, 'shell') as shell, \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                capture.main()
            shell.assert_not_called()

    def test_remote_paths_are_session_scoped_and_reject_path_syntax(self):
        self.assertEqual(
            capture.remote_capture_paths('tablet-session', 'baseline-off'),
            ('/data/local/tmp/mp6-simpleperf-tablet-session',
             '/data/local/tmp/mp6-cpu-tablet-session-baseline-off.data'))
        for session, name in (('../other', 'run'), ('session', '../run'), ('', 'run')):
            with self.subTest(session=session, name=name), self.assertRaises(ValueError):
                capture.remote_capture_paths(session, name)

    def test_foreground_parser_requires_one_mp6_resumed_activity(self):
        self.assertTrue(capture.package_is_foreground(
            'topResumedActivity=ActivityRecord{ com.mp6.game/MP6 }'))
        self.assertFalse(capture.package_is_foreground(
            'topResumedActivity=ActivityRecord{ other.app/Main }'))
        self.assertFalse(capture.package_is_foreground(
            'mResumedActivity: ActivityRecord{ com.mp6.game/MP6 }\n'
            'topResumedActivity=ActivityRecord{ com.mp6.game/MP6 }'))

    def test_warmup_parser_requires_settled_board_and_bounded_input(self):
        self.assertEqual(capture.warmup_board_ticks('1800'), 1800)
        self.assertEqual(capture.warmup_board_ticks('6000'), 6000)
        for value in ('1799', '6001', 'not-a-number'):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                capture.warmup_board_ticks(value)

    def test_capture_uses_session_paths_and_foreground_guard(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            (output/'session-owner.json').write_text('{}')
            run = output/'baseline-off'
            run.mkdir()
            (run/'request.json').write_text(json.dumps({'serial': 'tablet', 'pid': '9584'}))
            log = b'[EVENT] w01.live=board tick=2578\n[MP6-TICKRATE] tick=6117\n'
            shell_calls = []
            adb_calls = []

            def fake_shell(serial, *args, **kwargs):
                shell_calls.append(args)
                if args[0] == 'getprop':
                    return 'SM-X920'
                if args[0] == 'pidof':
                    return '9584'
                if args[:2] == ('dumpsys', 'activity'):
                    return 'topResumedActivity=ActivityRecord{ com.mp6.game/MP6 }'
                if args[:2] == ('dumpsys', 'thermalservice'):
                    return 'Thermal Status: 0\n'
                return ''

            def fake_adb(serial, *args, **kwargs):
                adb_calls.append(args)
                if args[0] == 'logcat':
                    return subprocess.CompletedProcess(args, 0, log, b'')
                if args[0] == 'exec-out':
                    return subprocess.CompletedProcess(args, 0, b'PERFILE2recording', b'')
                if args[0] == 'shell' and args[1].startswith('test -e '):
                    return subprocess.CompletedProcess(args, 1, b'', b'')
                if args[0] == 'shell':
                    return subprocess.CompletedProcess(args, 0, b'', b'')
                raise AssertionError(args)

            argv = ['capture', '--serial', 'tablet', '--model', 'SM-X920',
                    '--session', 'tablet-session', '--name', 'baseline-off']
            with patch.object(sys, 'argv', argv), \
                 patch.object(capture, 'session_paths', return_value=(output, '/private', {})), \
                 patch.object(capture, 'shell', side_effect=fake_shell), \
                 patch.object(capture, 'adb', side_effect=fake_adb), \
                 contextlib.redirect_stdout(io.StringIO()):
                capture.main()

            request = json.loads((run/'cpu-capture-request.json').read_text())
            self.assertEqual(request['warmup_board_ticks'], 1800)
            self.assertEqual(request['command'][0],
                             '/data/local/tmp/mp6-simpleperf-tablet-session')
            self.assertIn('/data/local/tmp/mp6-cpu-tablet-session-baseline-off.data',
                          request['command'])
            self.assertIn('--duration', request['command'])
            self.assertEqual(request['command'][request['command'].index('--duration')+1], '8')
            self.assertIn('--size-limit', request['command'])
            self.assertEqual(request['command'][request['command'].index('--size-limit')+1], '32M')
            self.assertIn('--no-inherit', request['command'])
            self.assertGreaterEqual(sum(c[:2] == ('dumpsys', 'activity') for c in shell_calls), 3)
            self.assertTrue(any(c[0] == 'shell' and 'mp6-simpleperf-tablet-session' in c[1]
                                for c in adb_calls))
            self.assertTrue((run/'perf.data').read_bytes().startswith(b'PERFILE2'))


if __name__ == '__main__':
    unittest.main()
