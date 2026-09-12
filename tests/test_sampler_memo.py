"""Exercise the actual GX sampler memo body, including runtime invalidation."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]

class SamplerMemo(unittest.TestCase):
    def test_keys_slots_and_reset(self):
        source=(ROOT/'build/aurora-release-source/lib/gx/gx.cpp').read_text()
        self.assertEqual(source,(ROOT/'build/android-aurora-source/lib/gx/gx.cpp').read_text())
        subject=source[source.index('struct SamplerMemoKey {'):source.index('GXBindGroups build_bind_groups')]
        self.assertIn('sSamplerMemo = {};',source[source.index('void initialize()'):])
        self.assertIn('sSamplerMemo = {};',source[source.index('void shutdown()'):])
        with tempfile.TemporaryDirectory(prefix='sampler-memo-',dir=ROOT/'build') as tmp:
            directory=Path(tmp)
            (directory/'sampler_subject.inc').write_text(subject)
            exe=directory/'test.exe'
            result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',
                '-I'+str(directory),str(ROOT/'tests/native/sampler_memo_selftest.cpp'),
                '-o',str(exe)],capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__': unittest.main()
