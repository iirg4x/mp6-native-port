"""Compile the actual producer boundary; verify ordering and refusal behavior."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]


class BorrowedFoliageDepth(unittest.TestCase):
    def test_actual_pass_boundary(self):
        source = (ROOT/'build/android-aurora-source/lib/gfx/common.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='foliage-depth-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            (folder/'borrowed_depth.inc').write_text(function(source, 'create_pass_from_efb_depth'))
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I'+str(folder), str(ROOT/'tests/native/borrowed_foliage_depth_selftest.cpp'),
                '-o', str(exe)], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_shared_api_and_compatibility_fallback(self):
        for unit in ('lib/gfx/common.cpp', 'include/aurora/gfx.hpp'):
            self.assertEqual((ROOT/'build/android-aurora-source'/unit).read_text(),
                             (ROOT/'build/aurora-release-source'/unit).read_text())
        source = (ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        self.assertIn('!gfx::ao_compatibility_mode() && gfx::create_pass_from_efb_depth(source)', source)
        self.assertIn('job->sourceSamples=source.sampleCount;', source)
        self.assertIn('p.sourceSamples==job->sourceSamples', source)
        self.assertIn('g_seedDepthLayouts[job->sourceSamples>1 ? 1 : 0]', source)


if __name__ == '__main__':
    unittest.main()
