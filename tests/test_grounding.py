from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build, apply_patches
from tests.integration.measure_board_ground import triangles, height

ROOT=Path(__file__).resolve().parents[1]

class Grounding(unittest.TestCase):
    def test_actual_runtime_preserves_game_state_jumps_and_platforms(self):
        with tempfile.TemporaryDirectory(prefix='grounding-runtime-',dir=ROOT/'build') as tmp:
            rel='src/game/hsfman.c'
            current=apply_patches.apply_unified_diff((Path(build.DECOMP)/rel).read_text(),
                (ROOT/'compat/decomp'/f'{rel}.patch').read_text())
            blocks=re.findall(r'Mtx temp;\s*Mtx final;.*?Hu3DDraw\(modelP, final, &modelP->scale\);',
                              current,re.S)
            self.assertEqual(len(blocks),1)
            (Path(tmp)/'grounding_render_path.h').write_text(
                'static void production_model_draw(HU3D_MODEL *modelP) {\n'
                'int i=(int)(modelP-Hu3DData);\n'+blocks[0]+'\n}\n')
            for headless in (False,True):
                exe=Path(tmp)/('headless.exe' if headless else 'windowed.exe')
                result=subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-O2','-UNDEBUG',
                    *(['-DMP6_HEADLESS_BUILD'] if headless else []),
                    '-I',tmp,'tests/native/grounding_runtime_selftest.c','src/hsf/mp6_grounding.c','-o',str(exe)],
                    cwd=ROOT,capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_static_floor_intersection(self):
        with tempfile.TemporaryDirectory(prefix='grounding-',dir=ROOT/'build') as tmp:
            exe=Path(tmp)/'test.exe'
            result=subprocess.run([build.ZIG,'cc','-O2','-UNDEBUG',
                'tests/native/grounding_math_selftest.c','-o',str(exe)],
                cwd=ROOT,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(subprocess.run([str(exe)],timeout=10).returncode,0)

    def test_quad_and_strip_match_submitted_gx_order(self):
        vertices=[[0,0,0],[0,0,1],[1,0,0],[1,0,1],[2,0,0]]
        mesh={'vertices':vertices,'faces':[[3,0,1,2,3]]}
        self.assertEqual(list(triangles(mesh)),[[vertices[i] for i in (0,2,3)],
                                               [vertices[i] for i in (0,3,1)]])
        mesh['faces']=[[4,0,1,2,3,4]]
        self.assertEqual(list(triangles(mesh)),[[vertices[i] for i in idx]
                         for idx in [(0,2,1),(2,1,3),(1,3,4)]])
        self.assertAlmostEqual(height(list(triangles(mesh))[0],.2,.2),0)

    def test_render_only_and_platform_exclusion(self):
        source=(ROOT/'src/hsf/mp6_grounding.c').read_text()
        self.assertNotIn('GwPlayer',source)
        self.assertNotIn('mbPlayer',source)
        self.assertNotIn('space_offset',source)
        self.assertIn('mp6_enh_ambient_occlusion() == 0) return 0;',source)
        self.assertNotIn('Hu3DModelPosSet',source)
        self.assertNotIn('mbPlayerPosSet',source)
        self.assertNotIn('mp6_host_section.h',source)
        rel='src/game/hsfman.c'
        current=apply_patches.apply_unified_diff((Path(build.DECOMP)/rel).read_text(),
            (ROOT/'compat/decomp'/f'{rel}.patch').read_text())
        self.assertNotIn('mp6_ground_',current) # main AND shadow submissions

    def test_every_board_actor_event_and_space_uses_authored_placement(self):
        for rel in ('src/board/capspecial.c','src/board/player.c','src/board/opening.c',
                    'src/board/masu.c','src/board/effect.c','src/board/capevent.c'):
            with self.subTest(source=rel):
                current=apply_patches.apply_unified_diff((Path(build.DECOMP)/rel).read_text(),
                    (ROOT/'compat/decomp'/f'{rel}.patch').read_text())
                self.assertNotIn('mp6_ground_',current)
        # Decal AO rejection remains: removing the placement hack must not
        # bring back space outlines, alter their alpha test or turn AO off.
        self.assertIn('mp6_ao_decals', (ROOT/'compat/decomp/src/board/masu.c.patch').read_text())

if __name__=='__main__': unittest.main()
