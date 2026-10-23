"""The optional inline AO timestamp experiment cannot select release outputs."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InlineAoBuildSelection(unittest.TestCase):
    def test_profile_requires_current_frame_instrumentation(self):
        for flags in (
            ['--name', 'current'],
            ['--name', 'current', '--gpu-profile'],
            ['--name', 'baseline', '--frame-profile'],
        ):
            with self.subTest(flags=flags):
                result = subprocess.run(
                    [sys.executable, '-B', 'tests/integration/build_engine_benchmark.py',
                     *flags, '--inline-ao-gpu-profile'], cwd=ROOT,
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('requires --name current --frame-profile', result.stderr)

    def test_distinct_output_and_composed_transform_are_explicit(self):
        source = (ROOT / 'tests/integration/build_engine_benchmark.py').read_text()
        self.assertIn("'-frame-inline-ao' if args.inline_ao_gpu_profile", source)
        self.assertIn('text=instrument_ao(text)', source)
        self.assertIn('text=instrument_inline_ao(text)', source)
        self.assertIn("'lib/webgpu/gpu.cpp': ('libaurora_core.a', instrument_gpu)", source)


if __name__ == '__main__':
    unittest.main()
