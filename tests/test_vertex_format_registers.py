"""Production CP decoder/cache versus the same decoder without its skip guards."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'build/android-aurora-source'

def subjects(folder,reference_as_candidate=False):
    cp=(SOURCE/'lib/gx/command_processor.cpp').read_text()
    header=(SOURCE/'lib/gx/gx.hpp').read_text()
    start=cp.index('static inline bool repeated_vertex_format_register(')
    end=cp.index('// XF register handler',start)
    subject=cp[start:end]
    reference=subject[subject.index('static void handle_cp('):]
    reference,count=re.subn(r'    (?:  )?if \(repeated_vertex_format_register\([^\n]+\)\) return;\n','',reference)
    assert count==5
    (folder/'vertex_format_reference.inc').write_text(reference)
    (folder/'vertex_format_subject.inc').write_text(reference if reference_as_candidate else subject)
    start=header.index('struct VtxAttrFmt {')
    (folder/'vertex_formats.inc').write_text(header[start:header.index('struct PnMtx {',start)])
    start=header.index('  std::array<u32, 26> vertexFormatRegs{};')
    (folder/'vertex_format_cache_fields.inc').write_text(header[start:header.index('  void clearVtxSizeCache()',start)])

def compile_subject(folder,benchmark=False,reference_as_candidate=False):
    subjects(folder,reference_as_candidate)
    exe=folder/'vertex-formats.exe'
    command=[build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG','-I'+str(folder),'-I'+str(SOURCE/'include')]
    if benchmark: command+=['-DMP6_REG_BENCH']
    command += [str(ROOT/'tests/native/vertex_format_register_selftest.cpp'),'-o',str(exe)]
    result=subprocess.run(command,capture_output=True,text=True,timeout=90)
    if result.returncode: raise RuntimeError(result.stderr)
    return exe,command

class VertexFormatRegisters(unittest.TestCase):
    def test_actual_decoder_and_cache(self):
        with tempfile.TemporaryDirectory(prefix='vertex-format-registers-',dir=ROOT/'build') as tmp:
            exe,_=compile_subject(Path(tmp))
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_original_repeated_invalidation_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='vertex-format-old-',dir=ROOT/'build') as tmp:
            exe,_=compile_subject(Path(tmp),reference_as_candidate=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,1,result.stdout+result.stderr)
            self.assertIn('!b.stateDirty',result.stderr)

if __name__=='__main__': unittest.main()
