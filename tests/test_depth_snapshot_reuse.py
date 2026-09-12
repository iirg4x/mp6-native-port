"""Exercise the production renderer's resolve/continuation implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from test_frame_resource_lifetimes import function
ROOT=Path(__file__).resolve().parents[1]

class DepthSnapshotReuse(unittest.TestCase):
    def test_actual_resolve_and_continuation(self):
        source=(ROOT/'build/android-aurora-source/lib/gfx/common.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='depth-reuse-',dir=ROOT/'build') as tmp:
            folder=Path(tmp)
            # Extract this fixture's subjects, not unrelated APIs subsequently
            # inserted between them. Borrowed foliage depth has its own oracle.
            names=('end_offscreen','create_pass','ao_compatibility_mode',
                   'efb_depth_write_serial','resolve_pass','resume_efb_pass_loading')
            (folder/'depth_snapshot_subject.inc').write_text(
                '\n'.join(function(source,name) for name in names))
            task_start=source.index('bool push_encoder_task(')
            task_end=source.index('\ntemplate <>',task_start)
            (folder/'encoder_task_subject.inc').write_text(source[task_start:task_end])
            exe=folder/'test.exe'
            for defines in [[],['-D__ANDROID__']]:
                result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',*defines,'-I'+str(folder),
                    str(ROOT/'tests/native/depth_snapshot_reuse_selftest.cpp'),'-o',str(exe)],capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stderr)
                for vendor in ('0', '0x13b5', '0x5143', '0x10de', '0x1002'):
                    result=subprocess.run([str(exe),vendor],capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('.resolveTarget = passInfo.deferColorResolve ? nullptr : passInfo.resolveView,',source)
        pool=source[source.index('static PassSnapshotEntry& acquire_pass_snapshot'):source.index('static FramePacket& current_frame_packet')]
        self.assertIn('pool.entries[pool.used++]',pool)
        self.assertIn('wgpu::TextureUsage::RenderAttachment',pool)
        self.assertIn('wgpu::TextureUsage::CopySrc',pool)
        finish=source[source.index('void finish()'):source.index('void end_frame(')]
        self.assertIn('enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass)',finish)
        execute=source[source.index('static void execute_encoder_task(wgpu::CommandEncoder& cmd, const EncoderTask& task) {'):]
        for member in ('depth = task.depth','targetWidth = task.targetSize.width',
                       'targetHeight = task.targetSize.height','sampleCount = task.sampleCount'):
            self.assertIn(member,execute)

if __name__=='__main__': unittest.main()
