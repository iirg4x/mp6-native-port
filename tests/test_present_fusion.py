"""Execute the actual fullscreen eligibility gate; GPU checks live in integration/."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]


class PresentFusion(unittest.TestCase):
    def test_whole_pixel_viewports_inside_the_surface_use_direct_resampling(self):
        source_gpu=(ROOT/'third_party/aurora/lib/webgpu/gpu.cpp').read_text()
        added=source_gpu+(ROOT/'third_party/aurora/lib/aurora.cpp').read_text()
        gate=re.search(r'bool can_resample_present_direct\(.*?\n}',source_gpu,re.S).group(0)
        source='''
#include <cassert>
#include <cmath>
#include <limits>
struct Viewport {float left,top,width,height;};
struct Surface {unsigned width,height;int format;};
struct Config {Surface surfaceConfiguration;};
Config g_graphicsConfig{{2960,1848,1}};
struct Size {unsigned width,height;};
struct Source {int format;Size size;};
Source input{1,{2960,1848}};
const Source& present_source(){return input;}
'''+gate+'''
int main(){
    assert(can_resample_present_direct({0,0,2960,1848}));
    // Whole-pixel letterbox offsets (content width rounded to 16) draw
    // directly when the source is the viewport's size.
    input.size={2948,1848};
    assert(can_resample_present_direct({6,0,2948,1848}));
    assert(can_resample_present_direct({0,0,2948,1848}));
    assert(!can_resample_present_direct({6,0,2948,1847}));
    input.size={2960,1648};
    assert(can_resample_present_direct({0,100,2960,1648}));
    input.size={5896,3696}; // SSAA source: rescaled, two passes
    assert(!can_resample_present_direct({6,0,2948,1848}));
    assert(can_resample_present_direct({0,0,2960,1848}));
    input.size={2960,1848};
    // Anything outside the surface, fractional or not finite keeps two passes.
    assert(!can_resample_present_direct({1,0,2960,1848}));
    assert(!can_resample_present_direct({0,1,2960,1848}));
    assert(!can_resample_present_direct({-1,0,2959,1848}));
    assert(!can_resample_present_direct({0.5f,0,2959,1848}));
    assert(!can_resample_present_direct({0,0,2959.5f,1848}));
    assert(!can_resample_present_direct({0,0,std::numeric_limits<float>::quiet_NaN(),1848}));
    assert(!can_resample_present_direct({std::numeric_limits<float>::infinity(),0,2960,1848}));
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
