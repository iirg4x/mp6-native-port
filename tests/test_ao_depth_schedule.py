"""AO admission tests use production epoch, counter, and camera-hook bodies."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import apply_patches, build
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
std::array<int,16> g_decals{},g_foliage{},queuedForeground{};
static int discarded=0;
void mp6_ao_camera_snapshot_reset(int camera) { (void)camera; }
void mp6_ao_foliage_render(int camera,int enabled) {
    assert(!enabled);queuedForeground[camera]=0;++discarded;
}
'''+hooks+r'''
int main() {
    int beforeInvalidDrains=drains;
    assert(!mp6_ao_camera_has_depth(0)); // no marker: fail closed
    assert(drains==beforeInvalidDrains); // invalid epochs do not query EFB serial
    for(int camera=0;camera<16;++camera) {
        mp6_ao_camera_begin(camera);
        assert(!mp6_ao_camera_has_depth(camera));
        ++serial; // the clear quad
        mp6_ao_depth_cleared(camera);
        assert(!mp6_ao_camera_has_depth(camera));
        ++serial; // real geometry after the clear
        assert(mp6_ao_camera_has_depth(camera));
        queuedForeground[camera]=7;g_decals[camera]=9;g_foliage[camera]=11;
        const int other=(camera+1)%16;
        queuedForeground[other]=13;g_decals[other]=17;g_foliage[other]=19;
        mp6_ao_depth_cleared(camera);
        assert(!mp6_ao_camera_has_depth(camera));
        assert(!queuedForeground[camera] && !g_decals[camera] && !g_foliage[camera]);
        assert(queuedForeground[other]==13 && g_decals[other]==17 && g_foliage[other]==19);
    }
    serial=UINT64_MAX;mp6_ao_camera_begin(1);serial=0;
    assert(mp6_ao_camera_has_depth(1));
    enabled=0;int oldDrains=drains;
    mp6_ao_camera_begin(1);mp6_ao_depth_cleared(1);
    assert(!mp6_ao_camera_has_depth(1) && drains==oldDrains);
    enabled=1;assert(!mp6_ao_camera_has_depth(1)); // mid-frame enable has no depth marker
    mp6_ao_camera_begin(1);assert(!mp6_ao_camera_has_depth(1));
    for(auto& epoch:g_depthEpochs) epoch={}; // same reset as each admitted frame
    beforeInvalidDrains=drains;
    assert(!mp6_ao_camera_has_depth(1));
    assert(drains==beforeInvalidDrains);
    assert(!mp6_ao_camera_has_depth(-1) && !mp6_ao_camera_has_depth(16));
    mp6_ao_camera_begin(0);
    mp6_ao_depth_epoch_consume(&g_depthEpochs[0]);
    int beforeConsumedDrains=drains;
    assert(!mp6_ao_camera_has_depth(0) && drains==beforeConsumedDrains);
    mp6_ao_camera_begin(0);
    mp6_ao_depth_epoch_exclude(&g_depthEpochs[0]);
    int beforeExcludedDrains=drains;
    assert(!mp6_ao_camera_has_depth(0) && drains==beforeExcludedDrains);
    int oldDiscarded=discarded;
    mp6_ao_camera_begin(-1);mp6_ao_depth_cleared(16);mp6_ao_depth_cleared(-1);
    assert(discarded==oldDiscarded);
}
''')

    def test_renderer_depth_predicate_and_serial(self):
        path=ROOT/'third_party/aurora/lib/gx/command_processor.cpp'
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
        self.assertLess(adapter.index('const int hasDepth='),adapter.index('mp6_ao_foliage_render(camera, 0)'))
        self.assertIn('if (!hasDepth)',adapter)
        self.assertIn('mp6_ao_camera_consume(camera);',adapter)
        patch=(ROOT/'compat/decomp/src/game/hsfman.c.patch').read_text()
        self.assertIn('+        mp6_fi_capture_camera(Hu3DCameraNo);\n+        mp6_ao_camera_begin(Hu3DCameraNo);',patch)
        # A clear terminates the old 3D depth epoch.  The pending AO pass must
        # be flushed before the clear, then the post-GXEnd hook must retire the
        # epoch.  Checking the order catches the subtle failure where the
        # clear invalidates the source before AO has consumed it.
        clear = patch[patch.index('+    mp6_ao_depth_about_to_clear(Hu3DCameraNo);')-400:]
        self.assertIn('mp6_ao_depth_about_to_clear(Hu3DCameraNo);', clear)
        self.assertLess(clear.index('mp6_ao_depth_about_to_clear(Hu3DCameraNo);'),
                        patch.index('+    GXEnd();\n+    mp6_ao_depth_cleared(Hu3DCameraNo);'))
        self.assertIn('mp6_ao_camera_end(camera);', adapter)
        self.assertIn('+    GXEnd();\n+    mp6_ao_depth_cleared(Hu3DCameraNo);',patch)
        # Hu3DZClear is a depth-destruction boundary, not a new camera
        # configuration.  Its pre-clear flush must retain the last model/view
        # snapshot even while the clear code rewrites projection state.
        original=(Path(build.DECOMP)/'src/game/hsfman.c').read_text(encoding='utf-8')
        current=apply_patches.apply_unified_diff(original,patch)
        # hsfman has a forward declaration as well as the definition; use the
        # function body so ordering checks cannot accidentally inspect the
        # prototype near the file header.
        clear_start=current.rindex('void Hu3DZClear(void)\n{')
        clear_end=current.index('void Hu3DZClearLayerSet',clear_start)
        clear_body=current[clear_start:clear_end]
        self.assertIn('mp6_ao_depth_about_to_clear(Hu3DCameraNo);',clear_body)
        self.assertNotIn('mp6_ao_camera_configured',clear_body)
        # Both layer-hook entry points can be Hu3DZClear.  They must retain
        # the saved geometry view for the pre-clear flush instead of replacing
        # it with the clear hook's projection.  Inspect the applied source so
        # a guard cannot exist only in patch context or on a declaration.
        pre_start=current.index('if(layerHook[Hu3DCameraNo][layer])')
        pre_end=current.index('if(layerNum[layer])',pre_start)
        post_start=current.index('if(layerHook[Hu3DCameraNo][layer+HU3D_LAYER_HOOK_POST])')
        post_end=current.index('mp6_ao_camera_end(Hu3DCameraNo);',post_start)
        for label, block in (('pre-layer',current[pre_start:pre_end]),
                             ('post-layer',current[post_start:post_end])):
            compact=''.join(block.split())
            guard='if(hookFunc!=(HU3D_LAYER_HOOK)Hu3DZClear){'
            self.assertIn(guard,compact,msg=label)
            guard_at=compact.index(guard)
            config_at=compact.index('mp6_ao_camera_configured(Hu3DCameraNo);')
            self.assertLess(guard_at,config_at,msg=label)
            self.assertGreater(compact.index('}',guard_at),config_at,msg=label)
            self.assertGreater(compact.rfind('hookFunc=layerHook',0,guard_at),-1,msg=label)

    def test_2d_scope_exclusion_is_explicit_and_cleared_for_new_3d(self):
        """Feedback/illustration passes cannot inherit the preceding 3D AO."""
        header = (ROOT/'include/mp6_ambient_occlusion.h').read_text()
        self.assertIn('void mp6_ao_camera_exclude(int camera);', header)
        port = (ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        self.assertIn('extern "C" void mp6_ao_camera_exclude(int camera)', port)
        # Exclusion is a scope boundary, not a global feature toggle.  The
        # depth-clear hook must reset it so later geometry in the same camera
        # can be shaded again.
        scope = port[port.index('extern "C" void mp6_ao_camera_begin'):
                     port.index('extern "C" int mp6_ao_camera_has_depth')]
        self.assertLess(scope.index('mp6_ao_camera_exclude'),
                        scope.index('extern "C" void mp6_ao_depth_cleared'))
        self.assertIn('mp6_ao_camera_exclude', scope)
        opening=(ROOT/'compat/decomp/src/REL/bootDll/opening.c.patch').read_text()
        mdsel=(ROOT/'compat/decomp/src/REL/mdseldll/mdsel.c.patch').read_text()
        mdparty=(ROOT/'compat/decomp/src/REL/mdpartydll/stage.c.patch').read_text()
        self.assertIn('mp6_ao_camera_exclude(Hu3DCameraNo);', opening)
        self.assertLess(opening.index('mp6_ao_camera_exclude(Hu3DCameraNo);'),
                        opening.index('mp6_widescreen_camera_native_target(OPENING_CAMERA_BIT, 1);'))
        for patch in (mdsel, mdparty):
            self.assertIn('mp6_ao_camera_end(Hu3DCameraNo);', patch)
            self.assertIn('mp6_ao_camera_exclude(Hu3DCameraNo);', patch)
            self.assertLess(patch.index('mp6_ao_camera_end(Hu3DCameraNo);'),
                            patch.index('mp6_ao_camera_exclude(Hu3DCameraNo);'))

if __name__=='__main__': unittest.main()
