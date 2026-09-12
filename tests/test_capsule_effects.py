"""Compile the actual capsule grid setter against its actual native structs."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


class CapsuleEffects(unittest.TestCase):
    def test_animation_grid_preserves_renderer_state(self):
        relative = 'src/board/capevent.c'
        original = (Path(build.DECOMP) / relative).read_text(encoding='utf-8')
        source = apply_patches.apply_unified_diff(
            original, (ROOT / 'compat/decomp' / (relative + '.patch')).read_text())
        # Extract source, not a hand-maintained copy of the faulty layout or
        # algorithm. No game assets, renderer, or upstream writes are needed.
        declarations = [re.search(
            r'typedef struct CapEffGlowParticleData \{.*?^\} \w+;',
            source, re.M | re.S).group()]
        declarations.append(re.search(
            r'typedef struct CapEffParticleSystemWork CAPEFFPARTICLESYSTEMWORK;.*?^\};',
            source, re.M | re.S).group())
        declarations.append(re.search(
            r'typedef struct CapEffGlowKinokoParticleSystemWork \{.*?^\} \w+;',
            source, re.M | re.S).group())
        setter = re.search(r'static void ev_CapEffGridSet\([^;]+?\)\n\{.*?^\}',
                           source, re.M | re.S).group()
        with tempfile.TemporaryDirectory(prefix='capsule-layout-', dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            (directory / 'capsule_effect_subject.inc').write_text(
                '\n\n'.join(declarations + [setter]), encoding='utf-8')
            exe = directory / 'capsule-effects.exe'
            result = subprocess.run(
                [build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-fno-strict-aliasing',
                 '-UNDEBUG', '-I', str(directory),
                 'tests/native/capsule_effects_selftest.c', '-o', str(exe)],
                cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('capsule animation grid: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
