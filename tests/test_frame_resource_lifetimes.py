"""Compile real frame-resource functions against deterministic lifetime oracles."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^([A-Za-z_][^\n;{}]*\b' + re.escape(name) + r'\((?:[^;{}]|\{\})*?\)\s*(?:noexcept\s*)?\{.*?^\})', source, re.M | re.S)
    if not match:
        raise AssertionError('Missing function: ' + name)
    return match.group(1)


class FrameResourceLifetimes(unittest.TestCase):
    def test_actual_frame_and_attachment_lifetimes(self):
        renderer = ROOT / 'build/android-aurora-source'
        common = (renderer / 'lib/gfx/common.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='frame-lifetimes-', dir=ROOT / 'build') as tmp:
            folder = Path(tmp)
            parts = {
                'frame_ops': ['current_high_water', 'capture_frame_op', 'enqueue_op', 'recycle_frame_packet'],
                'offscreen_target': ['begin_offscreen_target', 'end_color_pass'],
                'bindings': ['cached_bind_group', 'expire_cached_bind_groups'],
            }
            for filename, names in parts.items():
                (folder / (filename + '.inc')).write_text('\n'.join(function(common, n) for n in names))
            gpu = (renderer / 'lib/webgpu/gpu.cpp').read_text()
            (folder / 'present_bindings.inc').write_text(function(gpu, 'resample_present_bind_group'))
            exe = folder / 'test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG', '-I'+str(folder),
                str(ROOT / 'tests/native/frame_resource_lifetimes_selftest.cpp'), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shared_platform_sources_and_call_sites(self):
        for unit in ('include/aurora/gfx.hpp', 'lib/gfx/common.cpp', 'lib/gfx/common.hpp', 'lib/gfx/texture.hpp',
                     'lib/gfx/tex_copy_conv.cpp', 'lib/rmlui/WebGPURenderInterface.cpp'):
            self.assertEqual((ROOT/'build/android-aurora-source'/unit).read_text(),
                             (ROOT/'build/aurora-release-source'/unit).read_text(), unit)
        # gpu.cpp also contains intentionally different platform presentation policies.
        for name in ('resample_present_bind_group', 'create_copy_bind_group'):
            self.assertEqual(function((ROOT/'build/android-aurora-source/lib/webgpu/gpu.cpp').read_text(), name),
                             function((ROOT/'build/aurora-release-source/lib/webgpu/gpu.cpp').read_text(), name))
        common = (ROOT/'build/android-aurora-source/lib/gfx/common.cpp').read_text()
        self.assertNotIn('frame.ops', common)
        self.assertIn('static std::atomic_uint32_t g_frameIndex', common)
        self.assertIn('recycle_frame_packet(packet)', function(common, 'end_frame'))
        self.assertIn('recycle_frame_packet(frame)', function(common, 'begin_frame'))
        self.assertIn('g_snapshotPassDepth = {};', function(common, 'clear_caches'))
        self.assertIn('g_snapshotPassDepth = {};', function(common, 'shutdown'))
        upload = function(common, 'copy_staging_to_high_water')
        self.assertIn('for (const auto* upload : op.textureUploads)', upload)
        self.assertIn('frame.copied.textureUploadCount = highWater.textureUploadCount', upload)
        ui = (ROOT/'build/android-aurora-source/lib/rmlui/WebGPURenderInterface.cpp').read_text()
        self.assertIn('gfx::end_color_pass(true)', function(ui, 'EndFrame'))
        self.assertIn('gfx::end_color_pass()', function(ui, 'EndActivePass'))
        ao = (ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        self.assertNotIn('job.depth = job.cleanDepth.CreateView()', ao)
        self.assertNotIn('.CreateBindGroup(', ao)
        self.assertIn('job.cleanDepthView = nullptr', ao)
        mip = (ROOT/'build/android-aurora-source/lib/gfx/tex_copy_conv.cpp').read_text()
        self.assertIn('if (ref.generatedMipViews.empty())', function(mip, 'generate_mips'))
        self.assertNotIn('.CreateBindGroup(', function(mip, 'snapshot_depth'))


if __name__ == '__main__':
    unittest.main()
