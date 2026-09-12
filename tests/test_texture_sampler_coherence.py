"""Exercise BP sampler writes followed by the actual unchanged-image fast path."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'build/android-aurora-source'

def compile_subject(folder,old_policy=False):
    cp=(SOURCE/'lib/gx/command_processor.cpp').read_text()
    start=cp.index('      auto& slot = g_gxState.loadedTextures[mapping->texMapId];')
    end=cp.index('    } else {',start)
    bp=cp[start:end]
    if old_policy:
        for mode in ('mode0','mode1'):
            bp=bp.replace('        g_gxState.textures[mapping->texMapId].texObj.'+mode+' = value;\n','')
    (folder/'sampler_bp.inc').write_text(bp)
    gx=(SOURCE/'lib/gx/gx.cpp').read_text()
    start=gx.index('void resolve_sampled_textures(')
    (folder/'sampler_resolve.inc').write_text(gx[start:gx.index('static inline wgpu::BlendFactor',start)])
    header=(SOURCE/'lib/gfx/texture.hpp').read_text()
    start=header.index('struct GXTexObj_ {')
    (folder/'sampler_types.inc').write_text(header[start:])
    exe=folder/'sampler-coherence.exe'
    result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG','-DTARGET_PC',
        '-I'+str(folder),'-I'+str(SOURCE/'include'),
        str(ROOT/'tests/native/texture_sampler_coherence_selftest.cpp'),'-o',str(exe)],
        capture_output=True,text=True,timeout=90)
    if result.returncode: raise RuntimeError(result.stderr)
    return exe

class TextureSamplerCoherence(unittest.TestCase):
    def test_changes_keep_image_and_refresh_sampler(self):
        with tempfile.TemporaryDirectory(prefix='sampler-coherence-',dir=ROOT/'build') as folder:
            exe=compile_subject(Path(folder))
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_old_policy_reproduces_stale_sampler(self):
        with tempfile.TemporaryDirectory(prefix='sampler-coherence-old-',dir=ROOT/'build') as folder:
            exe=compile_subject(Path(folder),True)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,1,result.stdout+result.stderr)
            self.assertIn('Assertion failed',result.stderr)

if __name__=='__main__': unittest.main()
