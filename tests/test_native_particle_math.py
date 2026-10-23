"""Execute native replacements of recovered paired-single particle helpers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class ParticleMath(unittest.TestCase):
    def test_quad_operations_and_native_math_header(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        rel = 'src/board/effect.c'
        source = apply_patches.apply_unified_diff((decomp / rel).read_text(),
            (ROOT / 'compat/decomp' / (rel + '.patch')).read_text(), rel)
        helpers = '\n'.join(function_definition_body(source, name) for name in (
            'ParticleQuadTransform', 'ParticleQuadClear', 'ParticleQuadTexCopy', 'ParticleVecCopy'))
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            subject = directory / 'particle.c'
            subject.write_text('''#include <math.h>
#include "dolphin/math.h"
#include <assert.h>
#include <string.h>
typedef struct { float x, y, z; } Vec, HuVecF;
typedef struct { float x, y; } HuVec2f;
''' + helpers + '''
int main(void) {
    Vec source[4] = {{-1,2,3},{4,-5,6},{7,8,-9},{10,11,12}};
    Vec out[4], reference[4], inplace[4], pos = {13,-14,15};
    for (int n=0;n<100;++n) {
        float scale = (n-50)*0.03125f;
        for (int i=0;i<4;++i) {
            reference[i].x=fmaf(source[i].x,scale,pos.x);
            reference[i].y=fmaf(source[i].y,scale,pos.y);
            reference[i].z=fmaf(source[i].z,scale,pos.z);
        }
        ParticleQuadTransform(source,out,scale,&pos);
        assert(memcmp(reference,out,sizeof(out))==0);
        memcpy(inplace,source,sizeof(source));
        ParticleQuadTransform(inplace,inplace,scale,&pos);
        assert(memcmp(reference,inplace,sizeof(inplace))==0);
    }
    HuVec2f pair={-0.0f,0.0f};
    ParticleQuadClear(&pair,out);
    float *values=(float *)out;
    for (int i=0;i<12;++i) assert(values[i]==0 && !!signbit(values[i])==(i%2==0));
    HuVec2f uv[5]={{1,2},{3,4},{5,6},{7,8},{0,0}}, saved[4];
    memcpy(saved,uv,sizeof(saved));
    ParticleQuadTexCopy(uv,uv+1);
    assert(memcmp(saved,uv+1,sizeof(saved))==0);
    ParticleVecCopy(&source[0],&out[0]);
    assert(memcmp(source,out,sizeof(Vec))==0);
    ParticleVecCopy(&out[0],&out[0]);
    assert(memcmp(source,out,sizeof(Vec))==0);
    assert(fmodf(5.5f,2.0f)==1.5f && fabsf(-2.0f)==2.0f);
    return 0;
}
''')
            exe = directory / 'particle.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                str(subject), '-o', str(exe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
