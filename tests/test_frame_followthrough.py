"""Regression tests for the actual callers missed by the earlier helper tests."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from tests.test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT / 'build/android-aurora-source'


class FrameFollowthrough(unittest.TestCase):
    def compile_run(self, fixture, parts):
        with tempfile.TemporaryDirectory(prefix='frame-followthrough-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            for name, text in parts.items():
                (folder/name).write_text(text)
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I'+str(folder), str(ROOT/'tests/native'/fixture), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_real_continuations_reuse_command_storage(self):
        common = (RENDERER/'lib/gfx/common.cpp').read_text()
        names = ('push_command', 'resolve_pass_into', 'replay_copy_clear',
                 'resume_efb_pass_loading', 'recycle_frame_packet', 'finish', 'depth_store_op')
        self.compile_run('frame_followthrough_selftest.cpp',
                         {'subject.inc': '\n'.join(function(common, n) for n in names)})

    def test_blocking_callers_observe_before_probing(self):
        common = (RENDERER/'lib/gfx/common.cpp').read_text()
        names = ('wait_for_gpu_progress', 'wait_for_staging_buffer',
                 'acquire_frame_slot', 'acquire_mapped_staging_buffer')
        self.compile_run('frame_wait_callers_selftest.cpp',
                         {'subject.inc': '\n'.join(function(common, n) for n in names)})

    def test_actual_surface_acquisition_and_present_branches(self):
        source = (RENDERER/'lib/aurora.cpp').read_text()
        start = source.index('    wgpu::Texture currentTexture;')
        end = source.index('    if (canPresent) {', start)
        acquire = source[start:end]
        start = source.index('    if (canPresent && g_surface) {', end)
        end = source.index('    gfx::after_submit();', start)
        self.compile_run('surface_followthrough_selftest.cpp',
                         {'subject.inc': 'void present_frame() {\n'+acquire+source[start:end]+'\n}'})

    def test_final_depth_request_gating(self):
        depth = (RENDERER/'lib/gfx/depth_peek.cpp').read_text()
        common = (RENDERER/'lib/gfx/common.cpp').read_text()
        self.compile_run('depth_request_followthrough_selftest.cpp',
            {'subject.inc': function(depth, 'needs_frame_snapshot')})
        render = function(common, 'render')
        check = render.index('depth_peek::needs_frame_snapshot()')
        store = render.index('depth_store_op(passInfo, captureDepthSnapshot)')
        encode = render.index('if (captureDepthSnapshot) {')
        self.assertLess(check, store)
        self.assertLess(store, encode)
        self.assertNotIn('if (passInfo.captureDepthSnapshot)', render)
        self.assertIn('pass.finalEfb = true;', function(common, 'finish'))
        # The following frame must clear depth before final depth can be discarded.
        self.assertIn('bool clearDepth = true;', common)
        self.assertIn('current_render_passes().emplace_back();', function(common, 'begin_frame'))
        for unit in ('lib/gfx/common.cpp', 'lib/aurora.cpp', 'lib/gfx/depth_peek.cpp',
                     'lib/gfx/depth_peek.hpp'):
            self.assertEqual((RENDERER/unit).read_text(),
                             (ROOT/'build/aurora-release-source'/unit).read_text(), unit)


if __name__ == '__main__':
    unittest.main()
