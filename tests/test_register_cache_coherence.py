"""Execute production scalar register paths against uncached decoders.

The reference retains BP's selective-write merge and removes only skip guards.
Old cache policies are also compiled to prove each regression is detectable.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/android-aurora-source'


def subjects(folder, old_policy=False):
    cp = (SOURCE / 'lib/gx/command_processor.cpp').read_text()
    header = (SOURCE / 'lib/gx/gx.hpp').read_text()
    helpers = cp[cp.index('static inline bool repeated_bp_register('):
                 cp.index('// BP register handler')]
    if old_policy:
        helpers = '''static inline bool repeated_bp_register(u32 reg, u32 value) {
            return g_gxState.bpRegCache[reg] == value && reg != 0x52;
        }
        static inline bool repeated_xf_register(u32 reg, u32 value) {
            if (reg <= 0x19 && value == g_gxState.xfRegCache[reg]) return true;
            if (reg <= 0x19) g_gxState.xfRegCache[reg] = value;
            return false;
        }
        '''
    bp = cp[cp.index('static void handle_bp(u32 value, bool bigEndian) {'):
            cp.index('  // TEV color combiner stages')]
    bp += 'switch (regId) {\n'
    bp += cp[cp.index('  // genMode (0x00)'):cp.index('  // BP mask (0x0F)')]
    bp += cp[cp.index('  // Texture copy\n'):cp.index('  // Alpha compare (0xF3)')]
    bp += 'default: break;\n}\n}\n'
    start = cp.index('      if (repeated_xf_register(reg, val))')
    xf = 'static void handle_scalar_xf(u32 reg, u32 val) { do {\n'
    xf += cp[start:cp.index('      case 0x1A:', start)]
    start = cp.index('      case 0x3F:', start)
    xf += cp[start:cp.index('      default:', start)]
    xf += 'default: break;\n}\n} while(false);\n}\n'
    start = cp.index('  // Matrix index A (0x30)')
    matrix = 'static void handle_matrix_cp(u32 value) { switch(0x30) {\n'
    matrix += cp[start:cp.index('  // Matrix index B', start)]
    matrix += '}\n}\n'
    subject = bp + xf + matrix
    reference = subject.replace('if (repeated_bp_register(regId, value))', 'if (false)')
    reference = reference.replace('if (repeated_xf_register(reg, val))', 'if (false)')
    assert 'if (repeated_' not in reference
    (folder / 'register_subject.inc').write_text(helpers + subject)
    (folder / 'register_reference.inc').write_text(reference)
    start = header.index('struct ColorChannelConfig {')
    (folder / 'register_types.inc').write_text(header[start:header.index('struct FogState {', start)])
    for field in ('xfRegCache', 'xfRegCacheValid'):
        declaration = re.search(r'^  (?:std::array<u32, 0x1A>|u32) ' + field + r'[^\n]*', header, re.M)
        assert declaration, field
        (folder / (field + '.inc')).write_text(declaration[0])


def compile_subject(folder, old_policy=False):
    subjects(folder, old_policy)
    exe = folder / 'register-coherence.exe'
    command = [build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG', '-I' + str(folder),
               '-I' + str(SOURCE / 'include'),
               str(ROOT / 'tests/native/register_cache_coherence_selftest.cpp'), '-o', str(exe)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return exe


class RegisterCacheCoherence(unittest.TestCase):
    def test_production_decoders_match_uncached_writes(self):
        with tempfile.TemporaryDirectory(prefix='register-coherence-', dir=ROOT / 'build') as tmp:
            exe = compile_subject(Path(tmp))
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_old_policy_fails_each_regression(self):
        with tempfile.TemporaryDirectory(prefix='register-coherence-old-', dir=ROOT / 'build') as tmp:
            exe = compile_subject(Path(tmp), old_policy=True)
            for case in ('zero_xf', 'zero_bp', 'matrix', 'channels', 'genmode', 'tlut'):
                with self.subTest(case=case):
                    result = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn('Assertion failed:', result.stderr)


if __name__ == '__main__':
    unittest.main()
