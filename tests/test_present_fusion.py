"""Execute the actual fullscreen eligibility gate; GPU checks live in integration/."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]


class PresentFusion(unittest.TestCase):
    def test_only_exact_full_surface_uses_direct_resampling(self):
        patch=(ROOT/'compat/aurora/base/0031-fused-present-resample.patch').read_text()
        added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
        gate=re.search(r'bool can_resample_present_direct\(.*?\n}',added,re.S).group(0)
        source='''
#include <cassert>
#include <limits>
struct Viewport {float left,top,width,height;};
struct Surface {unsigned width,height;int format;};
struct Config {Surface surfaceConfiguration;};
Config g_graphicsConfig{{2960,1848,1}};
struct Source {int format;};
Source input{1};
const Source& present_source(){return input;}
'''+gate+'''
int main(){
    assert(can_resample_present_direct({0,0,2960,1848}));
    assert(!can_resample_present_direct({1,0,2960,1848}));
    assert(!can_resample_present_direct({0,1,2960,1848}));
    assert(!can_resample_present_direct({0,0,2959,1848}));
    assert(!can_resample_present_direct({0,0,2960,1847}));
    assert(!can_resample_present_direct({0,100,2960,1648}));
    assert(!can_resample_present_direct({0.5f,0,2960,1848}));
    assert(!can_resample_present_direct({0,0,2959.5f,1848}));
    assert(!can_resample_present_direct({0,0,std::numeric_limits<float>::quiet_NaN(),1848}));
    input.format=2;
    assert(!can_resample_present_direct({0,0,2960,1848}));
}
'''
        with tempfile.TemporaryDirectory(prefix='present-gate-',dir=ROOT/'build') as temporary:
            path=Path(temporary)/'test.cpp';exe=Path(temporary)/'test.exe'
            path.write_text(source)
            result=subprocess.run([build.ZIG,'c++','-O2','-UNDEBUG',str(path),'-o',str(exe)],
                                  capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('(!rmlBindGroup || rmlOverlay) && webgpu::can_resample_present_direct(viewport)',added)
        self.assertIn('else if (!directResample)',added)


if __name__=='__main__':unittest.main()
