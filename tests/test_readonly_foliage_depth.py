"""Exercise the actual read-only pass admission and attachment descriptor."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]


class ReadonlyFoliageDepth(unittest.TestCase):
    def test_production_admission_and_descriptor(self):
        source = (ROOT/'build/android-aurora-source/lib/gfx/common.cpp').read_text()
        render = function(source, 'render')
        start = render.index('      depthStencilAttachment = {')
        end = render.index('      depthStencilAttachmentPtr', start)
        descriptor = ('wgpu::RenderPassDepthStencilAttachment descriptor(const Pass& passInfo) {\n'
                      '  const bool captureDepthSnapshot=false;\n'
                      '  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment;\n'
                      +render[start:end]+'  return depthStencilAttachment;\n}\n')
        with tempfile.TemporaryDirectory(prefix='foliage-readonly-', dir=ROOT/'build') as tmp:
            folder=Path(tmp)
            (folder/'subject.inc').write_text(function(source, 'create_pass_with_readonly_efb_depth')+'\n'+descriptor)
            exe=folder/'test.exe'
            result=subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I'+str(folder), str(ROOT/'tests/native/readonly_foliage_depth_selftest.cpp'),
                '-o', str(exe)], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_contract_and_fallback(self):
        source=(ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        begin=source[source.index('extern "C" int mp6_ao_foliage_begin'):source.index('extern "C" int mp6_ao_foliage_target_size')]
        self.assertIn('!gfx::ao_compatibility_mode() && gfx::create_pass_with_readonly_efb_depth(width,height)',begin)
        self.assertLess(begin.index('create_pass_with_readonly_efb_depth'),begin.index('FoliageSeedJob'))
        self.assertEqual(begin.count('mp6_fi_note_ao_foliage(camera,0);'),2)
        mask=(ROOT/'include/mp6_ao_foliage_draw.h').read_text()
        self.assertIn('GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE);',mask)


if __name__=='__main__':
    unittest.main()
