import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT),str(ROOT/'tests/integration')]
from tools import build
from frame_profile_source import HEADER, TRANSFORMS, instrument_ao, replace_once
from engine_benchmark_metrics import frame_profile


class FrameProfileTests(unittest.TestCase):
    def test_source_anchors_fail_closed(self):
        for text in ('','anchor anchor'):
            with self.assertRaises(ValueError): replace_once(text,'anchor','changed')
        self.assertEqual(replace_once('prefix anchor suffix','anchor','changed'),'prefix changed suffix')

    def test_current_source_transforms_are_diagnostic_only(self):
        for relative,(_,transform) in TRANSFORMS.items():
            path=ROOT/'build/aurora-release-source'/relative
            if not path.exists(): self.skipTest('configured local renderer unavailable')
            source=path.read_text()
            result=transform(source)
            self.assertIn('mp6_frame_profile',result)
            self.assertEqual(path.read_text(),source)
        ao=(ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        result=instrument_ao(ao)
        for draw in ('pass.Draw(3);','pass.SetPipeline(pipelines[i]);'):
            self.assertEqual(result.count(draw),ao.count(draw))

    def test_native_context_and_work_accounting(self):
        with tempfile.TemporaryDirectory(prefix='frame-profile-test-',dir=ROOT/'build') as folder:
            folder=Path(folder)
            (folder/'mp6_frame_profile.hpp').write_text(HEADER)
            source=folder/'test.cpp'
            source.write_text(r'''
#include "mp6_frame_profile.hpp"
#include <cassert>
int main() {
  using namespace mp6_frame_profile;
  draw(99,0,1); assert(current==nullptr);
  { FrontTimer ignored(FrontStage::Fifo); }
  assert(frontStats.calls[0]==0);
  frontEnabled=true;
  { FrontTimer outer(FrontStage::Fifo);
    { FrontTimer recursive(FrontStage::Fifo);
      FrontTimer drawTimer(FrontStage::Draw);
    }
  }
  assert(frontTop==nullptr && frontDepth[0]==0 && frontDepth[1]==0);
  assert(frontStats.calls[0]==2 && frontStats.calls[1]==1);
  assert(frontStats.inclusive[0]==frontStats.exclusive[0]+frontStats.inclusive[1]);
  assert(frontStats.inclusive[1]==frontStats.exclusive[1]);
  frontEnabled=false;
  observe_cp(0x50,0); // A first zero write is not a duplicate; warm outside the window.
  assert(frontStats.cpWrites[0]==0 && cpValid[0x50]);
  frontEnabled=true;
  observe_cp(0x50,0);observe_cp(0x50,1);observe_cp(0x50,1);
  assert(frontStats.cpWrites[0]==3 && frontStats.cpRepeats[0]==2);
  for(unsigned a:{0x60,0x70,0x77,0x80,0x87,0x90,0x97}) {
    observe_cp(a,0);observe_cp(a,0);
  }
  assert(frontStats.cpWrites[1]==2 && frontStats.cpRepeats[1]==1);
  for(unsigned i:{2,3,4}) assert(frontStats.cpWrites[i]==4 && frontStats.cpRepeats[i]==2);
  for(unsigned a:{0x30,0x40,0x51,0x78,0x88,0x98,0xa0,0xb0}) {
    observe_cp(a,99);observe_cp(a,99);assert(!cpValid[a]);
  }
  frontEnabled=false;
  std::array<uint8_t,34> texture{}; texture[0]=7;
  std::array<uint8_t,23> palette{}; palette[0]=255;
  observe_texture_metadata(texture.data(),false); observe_texture_metadata(palette.data(),true);
  assert(frontStats.textureLoads==0 && frontStats.paletteLoads==0);
  frontEnabled=true;
  observe_texture_metadata(texture.data(),false); observe_texture_metadata(palette.data(),true);
  texture[33]=9; palette[22]=3;
  observe_texture_metadata(texture.data(),false); observe_texture_metadata(palette.data(),true);
  observe_texture_metadata(texture.data(),false); observe_texture_metadata(palette.data(),true);
  assert(frontStats.textureLoads==3 && frontStats.textureRepeats==2);
  assert(frontStats.paletteLoads==3 && frontStats.paletteRepeats==2);
  texture[0]=255; observe_texture_metadata(texture.data(),false);
  assert(frontStats.textureLoads==3);
  frontEnabled=false;
  for(unsigned age=1800;age<5800;++age) {
    Stats s; s.enabled=true; s.boardAge=age; s.passes=1;
    { Context context(s); draw(100,150,2); copy(64);
      Stats ignored; { Context nested(ignored); draw(1,1,1); }
      assert(current==&s);
    }
    assert(current==nullptr);
    assert(s.draws==1 && s.vertices==100 && s.indices==150 && s.drawElements==300);
    assert(s.copies==1 && s.copyBytes==64);
    s.inventory.push_back("index=0 label=unit-test");
    s.front=frontStats;
    complete(s);
  }
}
''')
            exe=folder/'test.exe'
            subprocess.run([build.ZIG,'c++','-std=c++20','-O2',str(source),'-o',str(exe)],check=True,capture_output=True)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
            parsed=frame_profile(result.stderr)
            self.assertEqual(parsed['work_average_per_frame']['drawElements'],300)
            self.assertEqual(parsed['work_average_per_frame']['sourceVertices'],100)
            self.assertEqual(len(parsed['front_end_average']),10)
            self.assertEqual(parsed['cp_register_census']['VCD low']['repeated_writes_per_frame'],2)
            self.assertEqual(parsed['texture_load_census'],dict(texture=3,identical=2,palette=3,identicalPalette=2))
            with self.assertRaises(ValueError):
                frame_profile(result.stderr.replace('texture=3.000 identical=2.000','texture=1.000 identical=2.000'))
            for bad in (result.stderr.replace('[FRAME-CP] register=VAT A','[MISSING] register=VAT A'),
                        result.stderr.replace('writes=3.000 repeats=2.000','writes=1.000 repeats=2.000')):
                with self.assertRaises(ValueError): frame_profile(bad)
            with self.assertRaises(ValueError):
                frame_profile(result.stderr.replace('[FRAME-FRONT] stage=Storage copy','[MISSING] stage=Storage copy'))
            with self.assertRaises(ValueError): frame_profile(result.stderr.replace('frames=4000','frames=3999'))
            with self.assertRaises(ValueError): frame_profile(result.stderr+result.stderr)

if __name__=='__main__':unittest.main()
