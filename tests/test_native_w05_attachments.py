"""Exercise the real ship setters with the native-width board work type."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


class W05Attachments(unittest.TestCase):
    def test_named_fields_preserve_every_other_byte(self):
        relative = 'src/REL/w05Dll/board.c'
        original = (Path(build.DECOMP) / relative).read_text()
        patched = apply_patches.apply_unified_diff(original,
            (ROOT / 'compat/decomp' / (relative + '.patch')).read_text())
        for source, correct in ((original, False), (patched, True)):
            with self.subTest(patched=correct), tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
                directory = Path(tmp)
                functions = []
                for suffix in ('80AC', '80D4', '80FC', '8124'):
                    match = re.search(r'^void fn_1_' + suffix + r'\([^;\n]*\)\s*\{.*?^\}', source, re.M | re.S)
                    self.assertIsNotNone(match)
                    functions.append(match.group())
                (directory / 'w05_attachment_subject.inc').write_text('\n'.join(functions))
                exe = directory / 'w05-attachments.exe'
                result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS,
                    '-I' + str(directory), '-O2', '-UNDEBUG',
                    'tests/native/w05_attachments_selftest.c', '-o', str(exe)],
                    cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
                if correct:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('W05 typed ship attachments: PASS', result.stdout)
                else:
                    self.assertNotEqual(result.returncode, 0, '32-bit offsets unexpectedly worked')


if __name__ == '__main__':
    unittest.main()
