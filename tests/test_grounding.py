from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build, apply_patches
from tests.integration.measure_board_ground import triangles, height
from tests.source_tokens import compact_source

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
                    '-DMP6_GROUNDING_TEST',
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
        source=compact_source((ROOT/'src/hsf/mp6_grounding.c').read_text())
        self.assertNotIn('GwPlayer',source)
        self.assertNotIn('mbPlayer',source)
        self.assertNotIn('space_offset',source)
        self.assertIn(compact_source('mp6_enh_ambient_occlusion() == 0) return 0;'),source)
        self.assertNotIn('Hu3DModelPosSet',source)
        self.assertNotIn('mbPlayerPosSet',source)
        self.assertNotIn('mp6_host_section.h',source)
        rel='src/game/hsfman.c'
        current=apply_patches.apply_unified_diff((Path(build.DECOMP)/rel).read_text(),
            (ROOT/'compat/decomp'/f'{rel}.patch').read_text())
        self.assertNotIn('mp6_ground_',current) # main AND shadow submissions

    def test_receiver_decal_is_exactly_registered_and_deferred_before_transparency(self):
        """The opaque W01 receiver decal must be a precise draw-time exception.

        The native runtime fixture proves the object predicate. This source
        contract covers the decomp draw path that cannot be linked in the
        compact CPU test: the deferred decal sorts before ordinary XLU/NEAR
        draws after opaque geometry has been submitted, and the exact predicate is
        consulted before FaceDraw emits the display list. Shadow drawing keeps
        its original branch and never calls the receiver-decal predicate.
        """
        rel='src/game/hsfdraw.c'
        current=apply_patches.apply_unified_diff((Path(build.DECOMP)/rel).read_text(),
            (ROOT/'compat/decomp'/f'{rel}.patch').read_text())
        header=(ROOT/'include/mp6_grounding.h').read_text()
        self.assertRegex(header, r'\bint\s+mp6_ground_is_decal\s*\(')
        self.assertIn('mp6_ground_is_decal',current)

        objmesh=current[current.index('static void objMesh'):
                       current.index('BOOL ObjCullCheck')]
        decal=objmesh.index('mp6_ground_is_decal')
        # Use the actual ordinary XLU/NEAR condition, not the diagnostic
        # census copy of that condition earlier in objMesh.
        ordinary=objmesh.rfind('HU3D_CONST_XLU')
        solid=objmesh.index('ObjDraw(drawObject);')
        self.assertLess(ordinary,decal)
        self.assertLess(decal,solid)
        priority=objmesh.index('drawObject->z = 1.0e30f;',decal)
        reserve=objmesh.index('mp6_fi_capture_defer_push',decal)
        self.assertLess(priority,reserve)
        # Hu3DDrawPost sorts descending; this finite priority precedes all
        # ordinary negative XLU/NEAR sort keys, independent of source order.
        self.assertIn('if(DrawObjData[DrawObjNum[j]].z < z)',current)
        self.assertIn('mp6_fi_capture_defer_push',objmesh[decal:solid])
        self.assertIn('DrawObjIdx++',objmesh[decal:solid])
        self.assertNotIn('ObjDraw(drawObject);',objmesh[decal:reserve])
        decal_context=objmesh[max(0,decal-260):decal+160]
        self.assertRegex(decal_context,
                         r'(?:shadowModelDrawF\s*==\s*(?:FALSE|0)|!\s*shadowModelDrawF)')

        face=re.search(r'^static void FaceDraw\([^;]+?\)\s*\{.*?^\}',
                       current,re.M|re.S).group()
        z_no_write=face.index('GXSetZMode(i, GX_LEQUAL, GX_FALSE)')
        display_list=face.index('GXCallDisplayList')
        self.assertIn('mp6_ground_is_decal(modelP, objPtr)',face)
        self.assertLess(face.index('mp6_ground_is_decal(modelP, objPtr)'),z_no_write)
        self.assertLess(z_no_write,display_list)
        z_guard=face[max(0,z_no_write-700):z_no_write]
        self.assertIn('mp6_ground_is_decal(modelP, objPtr)',z_guard)
        self.assertNotRegex(face, r'objPtr->name\s*\).*strcmp|strcmp\s*\([^\n]*objPtr->name')

        shadow=re.search(r'^static void FaceDrawShadow\([^;]+?\)\s*\{.*?^\}',
                         current,re.M|re.S).group()
        self.assertNotIn('mp6_ground_is_decal',shadow)
        self.assertIn('GXSetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE)',shadow)

    def test_w01_sky_ao_protection_is_exact_terrain_identity(self):
        source=(ROOT/'src/hsf/mp6_grounding.c').read_text()
        source_tokens=compact_source(source)
        header=(ROOT/'include/mp6_grounding.h').read_text()
        foliage=(ROOT/'include/mp6_ao_foliage_draw.h').read_text()
        self.assertRegex(header, r'\bint\s+mp6_ground_is_background\s*\(')
        self.assertIn(compact_source('#define GROUND_BACKGROUND_MAX 4'),source_tokens)
        for name in ('sora1','sora2','sora3','cloud'):
            self.assertIn(compact_source('!strcmp(name,"%s")' % name),source_tokens)
        begin=source.index('int mp6_ground_is_background')
        end=source.index('\n}\n',begin)+2
        predicate=compact_source(source[begin:end])
        self.assertIn('mp6_enh_ambient_occlusion()==0',predicate)
        self.assertNotIn('active()',predicate) # lawn lift success is unrelated
        self.assertIn('m!=Hu3DData+terrainId',predicate)
        self.assertIn('m->hsf!=terrainData',predicate)
        self.assertIn('o!=identity->object',predicate)
        self.assertIn('o->mesh.vertex->data!=identity->source',predicate)
        self.assertIn('o->mesh.vertex->count!=identity->count',predicate)
        self.assertIn('ground_identity_owner_unchanged(identity,m)',predicate)
        self.assertIn('ground_identity_pose_unchanged(identity)',predicate)
        self.assertIn('backgroundCount=aoUnshadedCount=0',source_tokens)
        self.assertIn('register_background(id,o)',source_tokens)

        self.assertIn('int background=mp6_ground_is_background(draw->model,draw->object);',foliage)
        self.assertIn('int protect=water || background || unshaded || character;',foliage)
        self.assertIn('if (!protect && !mp6_ao_foreground_eligible(draw,face)) return;',foliage)
        self.assertIn('int batch, camera, water, background, unshaded, character;',foliage)
        self.assertIn('static int mp6AoWaterDrawing, mp6AoBackgroundDrawing;',foliage)
        self.assertIn('if (mp6AoWaterDrawing || mp6AoBackgroundDrawing)',foliage)
        coverage=foliage.split('if (mp6AoWaterDrawing || mp6AoBackgroundDrawing)',1)[1].split(
            '\n    }',1)[0]
        self.assertIn('GXSetColorUpdate(GX_FALSE);',coverage)
        self.assertIn('GXSetAlphaUpdate(GX_TRUE);',coverage)
        self.assertNotIn('if (mp6AoBackgroundDrawing)',coverage)
        self.assertIn('mp6AoBackgroundDrawing=saved->background;',foliage)
        self.assertIn('mp6AoBackgroundDrawing=0;',foliage)
        self.assertIn('GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE);',foliage)

    def test_sky_sample_tolerance_is_private_msaa_only_and_restored(self):
        foliage=(ROOT/'include/mp6_ao_foliage_draw.h').read_text()
        source=(ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        self.assertIn('g_foliageMsaaSeed=source.sampleCount>1;',source)
        self.assertIn('return g_foliageCamera>=0 && g_foliageMsaaSeed;',source)
        self.assertIn('const int receiverSampleBias=msaaSeed && !saved->character && (saved->background || saved->unshaded);',foliage)
        before=foliage.index('if (receiverSampleBias) GX2SetPolygonOffset(-2.0f,-0.5f,-2.0f,-0.5f,0.0f);')
        draw=foliage.index('FaceDraw(draw,saved->face);',before)
        after=foliage.index('if (receiverSampleBias) GX2SetPolygonOffset(0.0f,0.0f,0.0f,0.0f,0.0f);',draw)
        self.assertLess(before,draw)
        self.assertLess(draw,after)
        self.assertIn('GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE);',foliage)

    def test_tree_receiver_replay_keeps_rgb_and_occluder_depth(self):
        foliage=(ROOT/'include/mp6_ao_foliage_draw.h').read_text()
        self.assertIn('int unshaded=mp6_ground_is_ao_unshaded(draw->model,draw->object);',foliage)
        self.assertIn('int protect=water || background || unshaded || character;',foliage)
        self.assertIn('Hu3DCameraNo,water,',foliage)
        self.assertIn('background || character,unshaded,character}',foliage)
        mask=foliage.split('static void mp6AoFoliageMaskState(void)',1)[1].split(
            'static void SetHiliteTexMtx',1)[0]
        self.assertIn('GXSetColorUpdate(GX_TRUE);',mask)
        self.assertIn('GXSetAlphaUpdate(GX_FALSE);',mask)
        self.assertIn('GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE);',mask)
        self.assertNotIn('mp6AoUnshadedDrawing',mask) # no sky/water alpha/depth exclusion
        self.assertIn('if (mp6AoWaterDrawing || mp6AoBackgroundDrawing)',mask)
        replay=foliage.split('void mp6_ao_foliage_render',1)[1]
        self.assertLess(replay.index('mp6_fi_capture_auxiliary_begin'),
                        replay.index('FaceDraw(draw,saved->face);'))
        self.assertIn('mp6AoFoliageMatrices(draw);',replay)

    def test_w01_ground_artwork_is_bounded_registered_receiver(self):
        source=compact_source((ROOT/'src/hsf/mp6_grounding.c').read_text())
        self.assertIn(compact_source('#define GROUND_DECAL_MAX 16'),source)
        self.assertIn(compact_source('#define GROUND_DECAL_VERTEX_MAX 64'),source)
        self.assertIn(compact_source('#define GROUND_DECAL_OBJECT_FLAG 512u'),source)
        candidate=source[source.index(compact_source('static int ground_decal_candidate')):
                         source.index(compact_source('static void register_ground_decal'))]
        self.assertIn('strcmp(hsf->root->name,"b01_m002")',candidate)
        self.assertIn('strcmp(o->mesh.parent->name,"flw_low2")',candidate)
        self.assertIn('o->flags&GROUND_DECAL_OBJECT_FLAG',candidate)
        self.assertIn('o->mesh.face->count!=1',candidate)
        self.assertIn('HSF_FACE_QUAD',candidate)
        self.assertIn('!strcmp(bitmap->name,"s3b01t42")',candidate)
        self.assertIn('register_ground_decal(id,o,world)',source)
        self.assertIn('groundDecalCount=backgroundCount=aoUnshadedCount=0',source)
        register=source[source.index(compact_source('static void register_ground_decal')):
                       source.index(compact_source('static HuVecF *ground_decal_vertices'))]
        self.assertIn('PSMTXInverse(world,inverse)',register)
        self.assertIn('memcpy(decal->vertices,decal->identity.source',register)
        self.assertIn('mp6_gxarray_register(decal->vertices',register)
        self.assertIn('groundDecalCount++',register)
        draw=source[source.index(compact_source('static HuVecF *ground_decal_vertices')):
                   source.index(compact_source('static void scan'))]
        self.assertIn('lawnAnchor.lift',draw)
        self.assertIn('lawnAnchor.lift!=decal->lift',draw)
        self.assertNotIn('PSMTXInverse(',draw)
        self.assertNotIn('memcpy(decal->vertices',draw)
        self.assertIn('ground_identity_pose_unchanged(&decal->identity)',draw)
        predicate=source[source.index(compact_source('int mp6_ground_is_decal')):
                         source.index(compact_source('int mp6_ground_is_background'))]
        self.assertIn('groundDecalCount',predicate)
        self.assertIn('decal->identity.object',predicate)
        self.assertIn('mp6_ground_vertices(m,o)!=o->mesh.vertex->data',predicate)

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
