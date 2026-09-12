"""Compare the actual matrix implementation to its pre-optimization body."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]

class MatrixConcat(unittest.TestCase):
    def test_original_arithmetic_and_aliases(self):
        unit='lib/dolphin/mtx/mtx.c'
        source=(ROOT/'build/aurora-release-source'/unit).read_text()
        self.assertEqual(source,(ROOT/'build/android-aurora-source'/unit).read_text())
        # The preserved scalar fallback is the reference for every alias mode.
        # Do not depend on an ignored local checkpoint to run this test.
        original=source[source.index('static inline void mtx_concat_scalar('):source.index('void C_MTXConcat(')]
        original=original.replace('static inline void mtx_concat_scalar(','void reference_concat(')
        candidate=source[source.index('static inline void mtx_concat_scalar('):source.index('void C_MTXConcatArray(')]
        with tempfile.TemporaryDirectory(prefix='matrix-concat-',dir=ROOT/'build') as tmp:
            directory=Path(tmp)
            (directory/'matrix_subject.inc').write_text(original+'\n'+candidate)
            exe=directory/'test.exe'
            result=subprocess.run([build.ZIG,'cc','-target','x86_64-windows-gnu','-O2','-DNDEBUG',
                '-DMP6_EXPERIMENT_VECTOR_CONCAT=1','-fno-strict-aliasing','-I'+str(directory),str(ROOT/'tests/native/matrix_concat_selftest.c'),
                '-o',str(exe)],capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout)

if __name__=='__main__': unittest.main()
