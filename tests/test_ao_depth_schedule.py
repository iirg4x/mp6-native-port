"""AO admission tests use production epoch, counter, and camera-hook bodies."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import build
from tests.test_frame_resource_lifetimes import function


class AoDepthScheduleTests(unittest.TestCase):
    def native(self,source):
        with tempfile.TemporaryDirectory(prefix='ao-depth-test-',dir=ROOT/'build') as folder:
            path=Path(folder)/'test.cpp';exe=Path(folder)/'test.exe'
            path.write_text(source)
            result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-I',str(ROOT/'include'),
                                   str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_real_camera_hooks_and_epoch_lifetime(self):
        source=(ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        hooks=source[source.index('extern "C" void mp6_ao_camera_begin'):
                     source.index('extern "C" int mp6_ao_foliage_begin')]
        self.native(r'''
#include <array>
#include <cassert>
#include "mp6_ao_depth_epoch.h"
static int enabled=2,drains=0;
static uint64_t serial=5;
static int mp6_enh_ambient_occlusion() { return enabled; }
namespace gfx { uint64_t efb_depth_write_serial() { ++drains; return serial; } }
std::array<Mp6AoDepthEpoch,16> g_depthEpochs{};
'''+hooks+r'''
int main() {
    assert(mp6_ao_camera_has_depth(0)); // no marker: preserve old behavior
    for(int camera=0;camera<16;++camera) {
        mp6_ao_camera_begin(camera);
        assert(!mp6_ao_camera_has_depth(camera));
        ++serial; // the clear quad
        mp6_ao_depth_cleared(camera);
        assert(!mp6_ao_camera_has_depth(camera));
        ++serial; // real geometry after the clear
        assert(mp6_ao_camera_has_depth(camera));
        mp6_ao_depth_cleared(camera);
        assert(!mp6_ao_camera_has_depth(camera));
    }
    serial=UINT64_MAX;mp6_ao_camera_begin(1);serial=0;
    assert(mp6_ao_camera_has_depth(1));
    enabled=0;int oldDrains=drains;
    mp6_ao_camera_begin(1);mp6_ao_depth_cleared(1);
    assert(!mp6_ao_camera_has_depth(1) && drains==oldDrains);
    enabled=1;assert(mp6_ao_camera_has_depth(1)); // mid-frame enable: conservative
    mp6_ao_camera_begin(1);assert(!mp6_ao_camera_has_depth(1));
    for(auto& epoch:g_depthEpochs) epoch={}; // same reset as each admitted frame
    assert(mp6_ao_camera_has_depth(1));
    assert(!mp6_ao_camera_has_depth(-1) && !mp6_ao_camera_has_depth(16));
    mp6_ao_camera_begin(-1);mp6_ao_depth_cleared(16);
}
''')

    def test_renderer_depth_predicate_and_serial(self):
        path=ROOT/'build/aurora-release-source/lib/gx/command_processor.cpp'
        if not path.exists(): self.skipTest('configured renderer unavailable')
        source=path.read_text()
        start=source.index('static uint64_t g_efbDepthWriteSerial')
        predicate=function(source,'note_efb_depth_write')
        stop=source.index(predicate,start)+len(predicate)
        self.native(r'''
#include <cstdint>
#include <cassert>
struct { bool depthCompare=true,depthUpdate=true; } g_gxState;
static bool offscreen=false;
namespace gfx { bool is_offscreen() { return offscreen; } }
'''+source[start:stop]+r'''
int main() {
    assert(efb_depth_write_serial()==0);
    for(int test=0;test<8;++test) {
        g_gxState.depthCompare=test&1;g_gxState.depthUpdate=test&2;offscreen=test&4;
        auto before=efb_depth_write_serial();note_efb_depth_write();
        assert(efb_depth_write_serial()-before==uint64_t(test==3));
    }
    g_gxState.depthCompare=g_gxState.depthUpdate=true;offscreen=false;
    auto before=efb_depth_write_serial();
    note_efb_depth_write();note_efb_depth_write(); // merged draws must not lose writes
    assert(efb_depth_write_serial()==before+2);
}
''')
        # Verify both submission paths actually call the tested helper.
        merge_start=source.index('  if (canMerge) {')
        merged=source[merge_start:source.index('static void handle_draw(u8',merge_start)]
        self.assertIn('note_efb_depth_write();\n    return;',merged)
        submitted=source[source.index('  // Build pipeline, bind groups, and push draw command'):]
        self.assertTrue(submitted.split('BindGroupRanges ranges{};')[0].endswith('  note_efb_depth_write();\n  '))
        indexed=source[source.index('} else if (subCmd == GX_AURORA_DRAW_INDEXED) {'):]
        self.assertIn('push_gx_draw(',indexed.split('{',1)[1].split('} else if')[0])

    def test_admission_only_and_clear_hooks(self):
        port=(ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        apply=port[port.index('extern "C" void mp6_ao_apply'):port.index('extern "C" void mp6_ao_shutdown')]
        self.assertNotIn('mp6_ao_camera_has_depth',apply) # FI replays admitted markers
        begin=port[port.index('extern "C" void mp6_ao_begin_frame'):port.index('extern "C" void mp6_ao_camera_begin')]
        self.assertIn('for (auto &epoch:g_depthEpochs) epoch={};',begin)
        adapter=(ROOT/'src/hsf/mp6_ambient_occlusion.c').read_text()
        self.assertLess(adapter.index('const int hasDepth='),adapter.index('mp6_ao_foliage_render(camera,hasDepth)'))
        self.assertIn('if (!hasDepth) return;',adapter)
        patch=(ROOT/'compat/decomp/src/game/hsfman.c.patch').read_text()
        self.assertIn('+        mp6_fi_capture_camera(Hu3DCameraNo);\n+        mp6_ao_camera_begin(Hu3DCameraNo);',patch)
        self.assertIn('+    GXEnd();\n+    mp6_ao_depth_cleared(Hu3DCameraNo);',patch)

if __name__=='__main__': unittest.main()
