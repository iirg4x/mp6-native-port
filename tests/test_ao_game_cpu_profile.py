import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT),str(ROOT/'tests/integration')]
from tools import build
from frame_profile_source import HEADER, instrument_hsfdraw


class AoGameCpuProfileTests(unittest.TestCase):
    def test_hsfdraw_transform_is_exact_and_keeps_early_return_body(self):
        source_path=ROOT/'build/patched-src/src/game/hsfdraw.c'
        source=source_path.read_text()
        transformed=instrument_hsfdraw(source)
        self.assertEqual(source.count('#include "mp6_ao_foliage_draw.h"'),1)
        self.assertEqual(transformed.count('#define mp6_ao_foliage_render mp6_ao_foliage_render_profiled_impl'),1)
        self.assertEqual(transformed.count('mp6_frame_profile_ao_foliage_render_begin();'),1)
        self.assertEqual(transformed.count('mp6_frame_profile_ao_foliage_render_end();'),1)
        self.assertEqual(transformed.count('mp6_frame_profile_ao_foliage_face_draw_begin();'),1)
        self.assertEqual(transformed.count('mp6_frame_profile_ao_foliage_face_draw_end();'),1)
        face_begin=transformed.index('mp6_frame_profile_ao_foliage_face_draw_begin();')
        face_end=transformed.index('mp6_frame_profile_ao_foliage_face_draw_end();')
        no_tex=transformed.index('void Hu3DTevStageNoTexSet(HU3D_DRAW_OBJ *drawObj, HSF_MATERIAL *matP)')
        tex=transformed.index('void Hu3DTevStageTexSet(HU3D_DRAW_OBJ *drawObj, HSF_MATERIAL *matP)')
        self.assertLess(face_begin,face_end)
        self.assertLess(face_end,no_tex)
        self.assertLess(no_tex,tex)
        header_body=(ROOT/'include/mp6_ao_foliage_draw.h').read_text()
        self.assertIn('if (mp6AoFoliageTick!=mp6_tick_count) { mp6AoFoliageCount=0; return; }',header_body)
        self.assertEqual(source,source_path.read_text())
        with self.assertRaises(ValueError):
            instrument_hsfdraw(source.replace('#include "mp6_ao_foliage_draw.h"','',1))
        with self.assertRaises(ValueError):
            instrument_hsfdraw(source+'\n#include "mp6_ao_foliage_draw.h"\n')
        with self.assertRaises(ValueError):
            instrument_hsfdraw(source.replace(
                'static void FaceDraw(HU3D_DRAW_OBJ *drawObj, HSF_FACE *face)\n{',
                'static void FaceDraw(HU3D_DRAW_OBJ *drawObj, HSF_FACE *face)\n{\n    return;'))

    def test_compiled_early_return_disabled_nested_and_thread_isolation(self):
        fake_header='''
void mp6_ao_foliage_render(int camera, int enabled)
{
    ++mp6TestRenderCalls;
    if (!enabled || camera==7) return;
    FaceDraw(&mp6TestDraw,&mp6TestFace);
}
'''
        fake_source='''
typedef struct { int value; } HU3D_DRAW_OBJ;
typedef struct { int value; } HSF_FACE;
typedef struct { int value; } HSF_MATERIAL;
typedef struct { int value; } HU3D_MODEL;
typedef unsigned u32;
static int mp6TestRenderCalls;
static HU3D_DRAW_OBJ mp6TestDraw;
static HSF_FACE mp6TestFace;
static void FaceDraw(HU3D_DRAW_OBJ *drawObj, HSF_FACE *face);
#include "mp6_ao_foliage_draw.h"
static void FaceDraw(HU3D_DRAW_OBJ *drawObj, HSF_FACE *face)
{
    (void)drawObj; (void)face;
    for (volatile unsigned i=0;i<250000;i++) mp6TestDraw.value+=(int)i&1;
}

void Hu3DTevStageNoTexSet(HU3D_DRAW_OBJ *drawObj, HSF_MATERIAL *matP) {
    (void)drawObj; (void)matP;
}

void Hu3DTevStageTexSet(HU3D_DRAW_OBJ *drawObj, HSF_MATERIAL *matP) {
    (void)drawObj; (void)matP;
}

void Hu3DMatLightSet(HU3D_MODEL *modelP, u32 flags, float hilitePower) {
    (void)modelP; (void)flags; (void)hilitePower;
}
'''
        bridge='''
#include "mp6_frame_profile.hpp"
extern "C" void mp6_frame_profile_ao_foliage_render_begin() noexcept {
    mp6_frame_profile::ao_foliage_game_begin(mp6_frame_profile::AoFoliageGameScopeKind::Render);
}
extern "C" void mp6_frame_profile_ao_foliage_render_end() noexcept {
    mp6_frame_profile::ao_foliage_game_end();
}
extern "C" void mp6_frame_profile_ao_foliage_face_draw_begin() noexcept {
    mp6_frame_profile::ao_foliage_game_begin(mp6_frame_profile::AoFoliageGameScopeKind::FaceDraw);
}
extern "C" void mp6_frame_profile_ao_foliage_face_draw_end() noexcept {
    mp6_frame_profile::ao_foliage_game_end();
}
'''
        main='''
#include "mp6_frame_profile.hpp"
#include <cassert>
#include <thread>
extern "C" void mp6_ao_foliage_render(int, int);
int main() {
    using namespace mp6_frame_profile;
    auto assert_balanced=[] {
        assert(aoFoliageGameScopeDepth==0);
        assert(aoFoliageReplayDepth==0);
        assert(aoFoliageGameScopeOverflow==0);
    };
    frontStats={}; frontEnabled=true;
    mp6_ao_foliage_render(7,1); // private implementation returns early.
    assert_balanced();
    assert(frontStats.aoFoliageRenderCalls==1 && frontStats.aoFoliageFaceDrawCalls==0);
    mp6_ao_foliage_render(1,0); // disabled path also returns before replay.
    assert_balanced();
    assert(frontStats.aoFoliageRenderCalls==2 && frontStats.aoFoliageFaceDrawCalls==0);
    mp6_ao_foliage_render(1,1);
    assert_balanced();
    assert(frontStats.aoFoliageRenderCalls==3 && frontStats.aoFoliageFaceDrawCalls==1);
    assert(frontStats.aoFoliageRenderInclusive>=frontStats.aoFoliageFaceDrawInclusive);
    assert(frontStats.aoFoliageRenderExclusive<=frontStats.aoFoliageRenderInclusive);
    const auto before=frontStats;
    frontEnabled=false;
    mp6_ao_foliage_render(1,1);
    assert_balanced();
    assert(frontStats.aoFoliageRenderCalls==before.aoFoliageRenderCalls);
    assert(frontStats.aoFoliageFaceDrawCalls==before.aoFoliageFaceDrawCalls);
    frontEnabled=true;
    std::thread worker([] {
        using namespace mp6_frame_profile;
        frontStats={}; frontEnabled=true;
        mp6_ao_foliage_render(1,1);
        assert(aoFoliageGameScopeDepth==0);
        assert(aoFoliageReplayDepth==0);
        assert(aoFoliageGameScopeOverflow==0);
        assert(frontStats.aoFoliageRenderCalls==1 && frontStats.aoFoliageFaceDrawCalls==1);
    });
    worker.join();
    assert_balanced();
    assert(frontStats.aoFoliageRenderCalls==before.aoFoliageRenderCalls);
    assert(frontStats.aoFoliageFaceDrawCalls==before.aoFoliageFaceDrawCalls);
    Stats report{}; report.enabled=true; report.boardAge=5799; report.front=frontStats;
    complete(report);
}
'''
        with tempfile.TemporaryDirectory(prefix='ao-game-profile-',dir=ROOT/'build') as folder:
            folder=Path(folder)
            (folder/'mp6_frame_profile.hpp').write_text(HEADER)
            (folder/'mp6_ao_foliage_draw.h').write_text(fake_header)
            (folder/'hsfdraw.c').write_text(instrument_hsfdraw(fake_source))
            (folder/'bridge.cpp').write_text(bridge)
            (folder/'main.cpp').write_text(main)
            obj=folder/'hsfdraw.o'
            subprocess.run([build.ZIG,'cc','-std=c11','-fno-sanitize=undefined','-I',str(folder),
                            '-c',str(folder/'hsfdraw.c'),'-o',str(obj)],
                           check=True,capture_output=True)
            exe=folder/'test.exe'
            try:
                result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-fno-sanitize=undefined','-pthread','-I',str(folder),
                                       str(folder/'bridge.cpp'),str(folder/'main.cpp'),str(obj),'-o',str(exe)],
                                      check=True,capture_output=True,text=True)
            except subprocess.CalledProcessError as error:
                self.fail(error.stderr[-4000:])
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
            self.assertIn('[FRAME-AO-GAME] renderInclusive=',result.stderr)
            self.assertIn('renderExclusive=',result.stderr)
            self.assertIn('faceDrawInclusive=',result.stderr)
            self.assertIn('faceDrawExclusive=',result.stderr)
            self.assertIn('renderCalls=',result.stderr)
            self.assertIn('faceDrawCalls=',result.stderr)


if __name__=='__main__':
    unittest.main()
