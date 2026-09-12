"""Execute the Windows GUI logging seam, including redirected test output."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

from tools import build

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'nt', 'Windows subsystem/CRT contract')
class WindowsGuiRelease(unittest.TestCase):
    def test_subsystem_is_gui_only_for_windowed_release(self):
        from unittest.mock import patch
        for configuration in ('debug', 'release'):
            with patch.object(build, 'WINDOWS_CONFIGURATION', configuration):
                self.assertEqual(build.windows_subsystem_flags(True), [])
                self.assertEqual(build.windows_subsystem_flags(False),
                                 ['-Wl,--subsystem,windows'] if configuration == 'release' else [])

    def test_gui_log_rotation_and_caller_owned_output(self):
        source = (ROOT / 'src/host/host_win32.c').read_text()
        start = source.index('static int mp6_host_output_redirected(')
        end = source.index('/* =======================================================================', start)
        subject = source[start:end]
        program = '''#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "mp6_utf8_file.h"
''' + subject + '''
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR args, int show) {
    mp6_host_release_logging_init();
    puts("release stdout survives");
    fflush(stdout);
    fputs("release stderr survives\\n", stderr);
    fflush(stderr);
    return GetConsoleWindow() != NULL ? 2 : 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='gui-log-\u00e9-', dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            fixture = directory / 'log-test.c'
            exe = directory / 'log-test.exe'
            fixture.write_text(program, encoding='utf-8')
            result = subprocess.run([build.ZIG, 'cc', '-O2', '-Wl,--subsystem,windows',
                                     '-I', str(ROOT / 'include'), str(fixture), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            binary = exe.read_bytes()
            pe = struct.unpack_from('<I', binary, 0x3c)[0]
            self.assertEqual(struct.unpack_from('<H', binary, pe + 24 + 68)[0], 2)
            # No terminal attached, like an Explorer GUI launch. Actual CRT
            # streams begin invalid; freopen must make both usable.
            for _ in range(2):
                result = subprocess.run([str(exe)], cwd=directory,
                                        creationflags=subprocess.DETACHED_PROCESS, timeout=5)
                self.assertEqual(result.returncode, 0)
            for name in ('mp6.log', 'mp6.previous.log'):
                log = (directory / 'logs' / name).read_text()
                self.assertEqual(log.count('release stdout survives'), 1)
                self.assertEqual(log.count('release stderr survives'), 1)
            latest = (directory / 'logs/mp6.log').read_bytes()
            previous = (directory / 'logs/mp6.previous.log').read_bytes()
            result = subprocess.run([str(exe)], cwd=directory, capture_output=True,
                                    text=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn('release stdout survives', result.stdout)
            self.assertIn('release stderr survives', result.stderr)
            self.assertEqual((directory / 'logs/mp6.log').read_bytes(), latest)
            self.assertEqual((directory / 'logs/mp6.previous.log').read_bytes(), previous)
            # Redirecting stderr alone must keep diagnostics in that pipe.
            result = subprocess.run([str(exe)], cwd=directory, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE, text=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn('release stderr survives', result.stderr)
            self.assertEqual((directory / 'logs/mp6.log').read_text().strip(),
                             'release stdout survives')


if __name__ == '__main__':
    unittest.main()
