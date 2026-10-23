"""Required game draws must use Aurora's existing wait-for-ready path."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]

class PipelineReadiness(unittest.TestCase):
    @unittest.skip("find_pipeline's specializations were replaced by the unified pipeline cache "
                   "(former base patch 0088); its own tests cover the required-pipeline wait")
    def test_game_and_clear_pipelines_are_required(self):
        # Upstream Aurora (the pinned commit, read from the legacy checkout's
        # Git objects) must fail the fixture; the port's source must pass it.
        legacy = Path(build.AURORA_LEGACY_CHECKOUT)
        if not (legacy / '.git').exists():
            self.skipTest('upstream Aurora Git objects are unavailable')
        from setup.lib import step_aurora
        source = subprocess.check_output(['git', '-C', str(legacy), 'show',
            f'{step_aurora.read_aurora_pin()}:lib/gfx/pipeline_cache.cpp'], text=True)
        fixed = (ROOT / 'third_party/aurora/lib/gfx/pipeline_cache.cpp').read_text(encoding='utf-8')
        for body, expect_success in ((source, False), (fixed, True)):
            definitions = re.findall(r'template <>\nPipelineRef find_pipeline\(ShaderType type, '
                r'const (?:clear|gx)::PipelineConfig& config, NewPipelineCallback&& cb\) \{.*?^\}',
                body, re.M | re.S)
            self.assertEqual(len(definitions), 2)
            with tempfile.TemporaryDirectory(prefix='pipeline-ready-', dir=ROOT / 'build') as temporary:
                directory = Path(temporary)
                (directory / 'pipeline_subject.inc').write_text('\n'.join(definitions))
                executable = directory / 'pipeline-ready.exe'
                result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2',
                    '-target', 'x86_64-windows-gnu', '-I', str(directory),
                    str(ROOT / 'tests/native/pipeline_priority_selftest.cpp'), '-o', str(executable)],
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0 if expect_success else 1, result.stdout + result.stderr)
                self.assertIn('PASS' if expect_success else 'FAIL', result.stdout)

if __name__ == '__main__':
    unittest.main()
