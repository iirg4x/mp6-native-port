"""CPU sampling must not overwrite a previously measured FPS window."""
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


class AndroidCpuProfile(unittest.TestCase):
    def test_cpu_artifacts_do_not_replace_timing_artifacts(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            (output/'session-owner.json').write_text('{}')
            run = output/'run'
            run.mkdir()
            (run/'request.json').write_text(json.dumps({'serial': 'phone', 'pid': '123'}))
            (run/'logcat.txt').write_bytes(b'original timing window')
            (run/'thermal.txt').write_bytes(b'original thermal window')
            log = b'[EVENT] w01.live=board tick=100\n[MP6-TICKRATE] tick=1000\n'

            def shell(serial, *args, **kwargs):
                if args[0] == 'getprop':
                    return 'SM-S906E'
                if args[0] == 'pidof':
                    return '123'
                if args[0] == 'dumpsys':
                    return 'Thermal Status: 0\n'
                return ''

            def adb(serial, *args, **kwargs):
                data = b''
                code = 0
                if args[0] == 'logcat':
                    data = log
                elif args[0] == 'exec-out':
                    data = b'PERFILE2recording'
                elif args[0] == 'shell' and args[1].startswith('test -e '):
                    code = 1
                return subprocess.CompletedProcess(args, code, data, b'')

            argv = ['capture', '--serial', 'phone', '--model', 'SM-S906E',
                    '--session', 'session', '--name', 'run']
            with patch.object(sys, 'argv', argv), \
                 patch.object(capture, 'session_paths', return_value=(output, '/private', {})), \
                 patch.object(capture, 'shell', side_effect=shell), \
                 patch.object(capture, 'adb', side_effect=adb), \
                 contextlib.redirect_stdout(io.StringIO()):
                capture.main()
            self.assertEqual((run/'logcat.txt').read_bytes(), b'original timing window')
            self.assertEqual((run/'thermal.txt').read_bytes(), b'original thermal window')
            self.assertEqual((run/'cpu-logcat.txt').read_bytes(), log)
            self.assertTrue((run/'cpu-thermal.txt').exists())
            self.assertEqual((run/'perf.data').read_bytes(), b'PERFILE2recording')


if __name__ == '__main__':
    unittest.main()
