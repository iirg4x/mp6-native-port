"""Real profiler percentile/report helpers; no GPU."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build
from tests.test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'lib/webgpu/gpu_prof.cpp'
SOURCE = ROOT/'third_party/aurora'/TARGET


class GpuTimingWindows(unittest.TestCase):
    def test_actual_percentiles_and_window_validity(self):
        source = SOURCE.read_text()
        harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
constexpr uint32_t HistorySize=120, AURORA_GPU_STAT_ROWS=8;
struct Sample { double span=0,between=0; std::array<double,AURORA_GPU_STAT_ROWS> rows{}; };
std::array<Sample,HistorySize> history{};
std::array<std::array<char,64>,AURORA_GPU_STAT_ROWS> names{};
uint32_t samples=0,dropped=0,diagnosticWindowDropped=0,nameCount=0;
'''
        harness += function(source, 'window_percentile')+'\n'+function(source, 'emit_window_report')
        harness += r'''
int main() {
  std::array<double,HistorySize> values{};
  assert(window_percentile(values,0,false)==0);
  values[0]=4;values[1]=1;values[2]=3;values[3]=2;
  assert(window_percentile(values,4,false)==2.5);
  assert(window_percentile(values,4,true)==4);
  assert(window_percentile(values,3,false)==3);
  assert(window_percentile(values,1,true)==4);
  emit_window_report();
  samples=HistorySize;nameCount=1;std::strcpy(names[0].data(),"XeGTAO main");
  for(uint32_t i=0;i<HistorySize;++i) {
    history[i].span=120-i;history[i].between=(120-i)*.01;
    history[i].rows[0]=(120-i)*.5;
  }
  emit_window_report();
  dropped=3;emit_window_report();
  assert(diagnosticWindowDropped==3);
  emit_window_report();
}
'''
        with tempfile.TemporaryDirectory(prefix='gpu-window-', dir=ROOT/'build') as temporary:
            folder=Path(temporary)
            cpp=folder/'test.cpp'
            cpp.write_text(harness)
            exe=folder/'test.exe'
            result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',str(cpp),'-o',str(exe)],
                                  capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
        headers=re.findall(r'^\[MP6-GPU-WINDOW\] (.*)$',result.stdout,re.M)
        self.assertEqual(len(headers),4,result.stdout)
        self.assertTrue(headers[0].startswith('status=incomplete count=0 dropped=0'))
        self.assertTrue(headers[1].startswith('status=valid count=120 dropped=0'))
        self.assertIn('gpu_span_median=60.500 gpu_span_p95=114.000',headers[1])
        self.assertIn('between_median=0.605 between_p95=1.140',headers[1])
        self.assertTrue(headers[2].startswith('status=dropped count=120 dropped=3'))
        self.assertTrue(headers[3].startswith('status=valid count=120 dropped=0'))
        self.assertIn('stage="XeGTAO main" median=30.250 p95=57.000',result.stdout)

    def test_opt_in_only(self):
        source=SOURCE.read_text()
        consume=function(source,'consume')
        self.assertEqual(consume.count('emit_window_report();'),1)
        self.assertIn('if (diagnostic.load() && ++diagnosticFrames % HistorySize == 0)',consume)
        self.assertIn('diagnosticWindowDropped = 0;',function(source,'reset_history'))


if __name__ == '__main__':
    unittest.main()
