"""Recovered non-void functions must return a value on every path.

Hardware returned whatever r3 held; native builds return 0 or garbage
(Stamp By Me's players never reached their win poses). The build rejects
both forms: a valueless `return;` is a compiler error, and a function that
falls off its end fails build.missing_return_check()."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from tools import build

ROOT = Path(__file__).resolve().parents[1]
COMPILE = [build.ZIG, 'cc', '-target', 'x86_64-windows-gnu', '-w', '-O2']


class MissingReturnCheck(unittest.TestCase):
    def check(self, directory, name, text):
        source = directory / name
        source.write_text(text, encoding='utf-8')
        with mock.patch.object(build, 'PATCHED_SRC_DIR', str(directory)):
            return build.missing_return_check(COMPILE, str(source))

    def test_recovered_sources_must_return_on_every_path(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            self.assertIsNone(self.check(directory, 'clean.c', 'int f(int a) { return a; }\n'))
            log = self.check(directory, 'falls_off.c', 'int f(int a) { if (a) return 1; }\n')
            self.assertIn('does not return a value', log)
            log = self.check(directory, 'valueless.c', 'int f(int a) { if (a) return; return 1; }\n')
            self.assertIn('should return a value', log)

    def test_port_sources_are_not_checked(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            source = Path(temporary) / 'port.c'
            source.write_text('int f(int a) { if (a) return 1; }\n', encoding='utf-8')
            self.assertIsNone(build.missing_return_check(COMPILE, str(source)))

    def test_valueless_return_stays_a_compile_error(self):
        self.assertNotIn('-Wno-error=return-type', build.COMMON_FLAGS)
        self.assertNotIn('-Wno-error=return-type', build.android_common_flags())
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            source = Path(temporary) / 'valueless.c'
            source.write_text('int f(int a) { if (a) return; return 1; }\n', encoding='utf-8')
            proc = subprocess.run(COMPILE + ['-c', str(source), '-o', str(source.with_suffix('.o'))],
                                  capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0, proc.stderr)


if __name__ == '__main__':
    unittest.main()
