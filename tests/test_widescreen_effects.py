"""Screen-domain coverage without changing native texture buffer dimensions."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


def patched(rel):
    dep = Path(apply_patches.require_pinned_decomp())
    text = (dep/rel).read_text(encoding='utf-8')
    patch = ROOT/'compat/decomp'/(rel+'.patch')
    return apply_patches.apply_unified_diff(text,patch.read_text(encoding='utf-8')) if patch.exists() else text


def function(text, name):
    return re.search(r'^(?:static )?(?:void|BOOL|float) '+name+r'\([^;]+?\)\n\{.*?^\}',text,re.M|re.S).group()


class WidescreenEffects(unittest.TestCase):
    def test_m632_extends_only_visible_box_rim_before_hooks_and_collision(self):
        source = patched('src/REL/m632dll/game.c')
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/m632dll/game.c').read_text()
        marker = '    mp6_widescreen_extend_sloped_rim(lbl_1_bss_0.sceneModels[1], "stageBa", 1, 769.4330f);\n'
        self.assertEqual(source.count(marker), 1)
        joint_motion = re.search(r'\bjointMotion\s*=\s*Hu3DJointMotion\s*\(', source)
        self.assertIsNotNone(joint_motion)
        self.assertLess(source.index(marker), joint_motion.start())
        self.assertLess(source.index(marker), source.index('(&lbl_1_bss_0.collisionModel)[mapCount] ='))
        theater = function(source, 'mp6_m632_theater_margins')
        self.assertIn('DATANUM(DATA_m632, 5)', theater)
        self.assertIn('mp6_widescreen_static_margins(model, surfaces,', theater)
        self.assertNotIn('sceneModels', theater)
        self.assertLess(source.index('    mp6_m632_theater_margins();'), joint_motion.start())
        source = source.replace('\n/* The opening theater is skinned. Keep that owner and its deformation buffers\n'
                                ' * untouched; an independent, non-animated copy draws only appended margins. */\n'
                                + theater + '\n', '')
        source = source.replace('    mp6_m632_theater_margins();\n\n', '')
        source = source.replace('#include "mp6_widescreen.h"\n', '')
        source = source.replace('    /* Continue only the box\'s outer sloped walls; the playfield stays native. */\n'+marker+'\n', '')
        source = source.replace('void fn_1_3510(MGACTOR *actorP, intptr_t param)',
                                'void fn_1_3510(MGACTOR *actorP, int param)')
        self.assertEqual(source, original)

    def test_cold_disabled_backdrops_register_and_restore_without_reload(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        keep = {'mp6_ws_model_live', 'mp6_ws_accum_bbox_r', 'mp6_ws_apply_model_from_native',
                'mp6_ws_extrude_generic', 'mp6_widescreen_reapply', 'mp6_widescreen_forget_model'}
        prefixes = ('mp6_ws_model_', 'mp6_ws_repeat_', 'mp6_ws_selective_',
                    'mp6_ws_reposition_', 'mp6_ws_border_', 'mp6_ws_split_',
                    'mp6_widescreen_extrude_model', 'mp6_widescreen_split_separate_x')
        # Exercise real registration, baselines and the per-frame dispatcher.
        # Appended-face writers have their own real-buffer fixtures below.
        subject = re.sub(r'^[a-zA-Z_][\w *]*\b(\w+)\([^;]*?\)\n\{.*?^\}',
            lambda m: m.group() if m.group(1) in keep or m.group(1).startswith(prefixes) else '',
            source, flags=re.M|re.S)
        with tempfile.TemporaryDirectory(prefix='wide-cold-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'cold_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                '-I'+tmp, str(ROOT/'tests/native/widescreen_cold_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_filesel_badge_exit_keeps_native_home_and_tracks_live_width(self):
        scene = patched('src/REL/fileseldll/filesel.c')
        name = patched('src/REL/fileseldll/filename.c')
        self.assertIn('mp6_filesel_badge_slide_x(lbl_1_bss_68[3].x, scale)',
                      function(scene, 'fn_1_2654'))
        self.assertIn('L_95A8:\n    /* Re-derive the hidden badge position when the window is resized. */\n'
                      '    mp6_filesel_badge_park();', name)
        # The return animation derives its delta from the real parked position.
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/fileseldll/filesel.c').read_text()
        self.assertEqual(function(scene, 'fn_1_2A40'), function(original, 'fn_1_2A40'))
        with tempfile.TemporaryDirectory(prefix='wide-badge-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'badge_subject.inc').write_text(function(scene, 'mp6_filesel_badge_slide_x'))
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-UNDEBUG', '-I'+tmp,
                str(ROOT/'tests/native/filesel_badge_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_board_circle_wipe_keeps_native_corner_clearance_timing(self):
        board=patched('src/board/wipe.c')
        self.assertIn('imageWork->imageNo == 0 ? WipeCircleHalfSize(imageWork->size) : 0.5f * imageWork->size',
                      function(board,'WipeImageDraw'))
        with tempfile.TemporaryDirectory(prefix='wide-iris-',dir=ROOT/'build') as tmp:
            directory=Path(tmp)
            (directory/'iris_subject.inc').write_text(function(board,'WipeCircleHalfSize'))
            exe=directory/'test.exe'
            result=subprocess.run([build.ZIG,'cc','-UNDEBUG','-I'+tmp,
                str(ROOT/'tests/native/widescreen_iris_selftest.c'),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_wipes_keep_fixed_capture_storage_and_native_emblem_proportions(self):
        game=patched('src/game/wipe.c')
        board=patched('src/board/wipe.c')
        self.assertEqual(function(game,'WipeCaptureFrame'),function(board,'WipeCaptureFrame'))
        for source in (game,board):
            self.assertNotIn('Hu3DFbCopyExec(',source)
        for name,count in (('WipeCrossFade',1),('WipeDissolve',2),
                           ('WipeViewShift',1),('WipeWave',1)):
            self.assertEqual(function(game,name).count('WipeCaptureFrame('),count)
        for name in ('WipeShapeUpdate','WipeGridUpdate','WipePaperUpdate'):
            self.assertEqual(function(board,name).count('WipeCaptureFrame('),1)
        wave=function(game,'WipeWave')
        self.assertEqual(wave.count('GXPosition2u16(mp6_widescreen_render_width(),'),2)
        # Allocators and texture consumers keep their original storage contract.
        dep=Path(apply_patches.require_pinned_decomp())
        for rel,source in (('src/game/wipe.c',game),('src/board/wipe.c',board)):
            original=(dep/rel).read_text()
            for api in ('GXGetTexBufferSize','Hu3DTexLoad'):
                self.assertEqual(re.findall(api+r'\([^;]+',source),re.findall(api+r'\([^;]+',original))
        self.assertNotIn('HuMemDirectFree(wipeData.image',game)
        subject='\n'.join(function(game,n) for n in ('WipeCaptureFrame','WipeImageRelease',
                                                   'WipeCreate','WipeImage','WipeLoadDraw'))
        with tempfile.TemporaryDirectory(prefix='wide-wipes-',dir=ROOT/'build') as tmp:
            directory=Path(tmp)
            (directory/'wipe_subject.inc').write_text(subject)
            exe=directory/'test.exe'
            result=subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-UNDEBUG','-I'+tmp,
                str(ROOT/'tests/native/widescreen_wipe_selftest.c'),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_w01_extends_only_non_overlapping_outer_background_before_ao_snapshot(self):
        source=patched('src/REL/w01Dll/world01.c')
        calls=re.findall(r'^    mp6_widescreen_extend_terrain_outer_sides\((.*)\);$',source,re.M)
        self.assertEqual(calls,[f'mbObjModelIDGet(modelIds[0]), "{name}", 0.0f, 2.0f, 1'
                               for name in ('haikei15','sora2')])
        self.assertIn('haikei10 and haikei17 are interior\n     * lawn plates', source)
        self.assertNotIn('"haikei10", 0.0f, 2.0f, 1);', source)
        self.assertNotIn('"haikei17", 0.0f, 2.0f, 1);', source)
        self.assertLess(source.rindex('mp6_widescreen_extend_terrain_outer_sides('),
                        source.index('mp6_ground_w01_begin('))
        self.assertNotIn('mp6_widescreen_extend_terrain_sides(',source)

    def test_m602_card_atlas_keeps_all_three_native_views_and_copy_bounds(self):
        scene = patched('src/REL/m602dll/stage.c')
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/m602dll/stage.c').read_text()
        marker = 'mp6_widescreen_camera_native_target(lbl_1_data_124[stageIndex].cameraBit, 1);'
        body = function(scene, 'fn_1_179C')
        self.assertLess(body.index('Hu3DCameraCreate('), body.index(marker))
        self.assertLess(body.index(marker), body.index('Hu3DCameraViewportSet('))
        scene = scene.replace('#include "mp6_widescreen.h"\n', '')
        scene = scene.replace('        /* These three views feed a 576x288 card atlas, not the window. */\n', '')
        self.assertEqual(scene.replace('        '+marker+'\n', ''), original)

    def test_storybook_texture_pass_stays_native_and_restores_screen_camera(self):
        scene = patched('src/REL/bootDll/opening.c')
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/bootDll/opening.c').read_text()
        for name, enabled in (('PreDrawHook', 1), ('PostDrawHook', 0)):
            body = function(scene, name)
            marker = f'mp6_widescreen_camera_native_target(OPENING_CAMERA_BIT, {enabled});'
            self.assertEqual(body.count(marker), 1)
            self.assertLess(body.index(marker), body.index('Hu3DCameraPerspectiveSet('))
            if name == 'PreDrawHook':
                # The page illustration is intentionally outside scene AO;
                # keep that exclusion while comparing the camera body.
                self.assertEqual(body.count('mp6_ao_camera_exclude(Hu3DCameraNo);'), 1)
                body = re.sub(r'    /\* The character cutouts.*?\*/\n', '', body, flags=re.S)
                body = body.replace('    mp6_ao_camera_exclude(Hu3DCameraNo);\n', '')
            body = re.sub(r'    /\* (Page contents|The book)[^\n]*\*/\n', '', body)
            self.assertEqual(body.replace('    '+marker+'\n', ''), function(original, name))
        self.assertEqual(function(scene, 'FilterDraw'), function(original, 'FilterDraw'))
        self.assertEqual(re.findall(r'GXGetTexBufferSize\([^;]+', scene),
                         re.findall(r'GXGetTexBufferSize\([^;]+', original))

    def test_m602_extends_all_shelf_layers_and_floor_only(self):
        source = patched('src/REL/m602dll/effect.c')
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/m602dll/effect.c').read_text()
        self.assertEqual(source.count('mp6_widescreen_extend_edge('), 2)
        self.assertIn('lbl_1_bss_23C[0], "kyaku", 2, 800.0f', source)
        self.assertIn('lbl_1_bss_23C[2], "kyaku", 2, 800.0f', source)
        self.assertIn('lbl_1_bss_23C[1], "ground", 1', source)
        source = source.replace('#include "mp6_widescreen.h"\n', '')
        source = source.replace('    /* Continue the audience shelves past their exposed front ends. */\n', '')
        source = re.sub(r'^    mp6_widescreen_extend_(edge|sides)\([^;]+;\n', '', source, flags=re.M)
        self.assertEqual(source, original)

    def test_m657_timers_follow_split_centers_without_stretch(self):
        source = patched('src/REL/m657Dll/score.c')
        subject = function(source, 'mp6_m657_score_positions')
        for name in ('fn_1_4004', 'fn_1_42F4'):
            self.assertIn('mp6_m657_score_positions(scoreWork);', function(source, name))
        with tempfile.TemporaryDirectory(prefix='wide-scores-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'score_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-UNDEBUG', '-I'+str(directory),
                str(ROOT/'tests/native/widescreen_score_selftest.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_m638_callouts_keep_native_outer_margins_on_resize(self):
        source = patched('src/REL/m638Dll/game.c')
        subject = function(source, 'mp6_m638_prompt_positions')
        for name in ('fn_1_4E48', 'fn_1_5378'):
            self.assertIn('mp6_m638_prompt_positions(scene);', function(source, name))
        with tempfile.TemporaryDirectory(prefix='wide-prompts-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'prompt_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-UNDEBUG', '-I'+str(directory),
                str(ROOT/'tests/native/widescreen_prompt_selftest.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_m635_extends_adjoining_day_or_night_terrain_without_moving_native_faces(self):
        source = patched('src/REL/m635dll/graphics.c')
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/m635dll/graphics.c').read_text()
        self.assertEqual(source.count('mp6_widescreen_extend_terrain_sides('), 1)
        self.assertEqual(source.count('mp6_widescreen_extend_floor('), 1)
        self.assertIn('nightMode ? "green_n" : "green"', source)
        source = source.replace('#include "mp6_widescreen.h"\n', '')
        source = source.replace('    /* Continue both background perimeters; the garden and props stay native. */\n'
            '    mp6_widescreen_extend_floor(modelId, nightMode ? "green_n" : "green", 1.0f, 0.0f, 0.0f);\n'
            '    mp6_widescreen_extend_terrain_sides(modelId, nightMode ? "ground02_n" : "ground02", 0.0f, 0);\n\n', '')
        self.assertEqual(source, original)

    def test_model_lifetime_handles_exact_arena_address_reuse(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        # Keep the real registry layouts, find/alloc, invalidation and teardown.
        # Drop unrelated rendering functions so no fake renderer is linked.
        keep = {'mp6_ws_model_live', 'mp6_widescreen_forget_model',
                'mp6_widescreen_floor_border_fill_dup_kill_all'}
        keep.update('mp6_ws_'+kind+'_'+op for kind in
                    ('model', 'repeat', 'selective', 'border', 'split', 'reposition', 'extend')
                    for op in ('find', 'alloc'))
        subject = re.sub(r'^[a-zA-Z_][\w *]*\b(\w+)\([^;]*?\)\n\{.*?^\}',
                         lambda m: m.group() if m.group(1) in keep else '', source, flags=re.M|re.S)
        with tempfile.TemporaryDirectory(prefix='wide-lifetime-', dir=ROOT/'build') as tmp:
            (Path(tmp)/'lifetime_subject.inc').write_text(subject)
            exe = Path(tmp)/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG',
                '-I'+tmp,
                str(ROOT/'tests/native/widescreen_lifetime_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        kill = function(patched('src/game/hsfman.c'), 'Hu3DModelKill')
        self.assertLess(kill.index('mp6_widescreen_forget_model(modelId);'), kill.index('if(!hsf)'))

    def test_backdrop_center_uses_transformed_vertices_not_cumulative_cull_bounds(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        start = source.index('static void mp6_ws_accum_bbox_r(')
        end = source.index('\n}', source.index('static BOOL mp6_ws_model_center(')) + 2
        extra_start = source.index('typedef struct {\n    const char *name;\n    HuVecF otherMin')
        extra_end = source.index('\n}', source.index('float mp6_widescreen_model_center_x_widened(')) + 2
        with tempfile.TemporaryDirectory(prefix='wide-bounds-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'bounds_subject.inc').write_text(source[start:end]+'\n'+source[extra_start:extra_end])
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG', '-I'+str(directory),
                str(ROOT/'tests/native/widescreen_bounds_selftest.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_textured_boundary_extension_preserves_native_surface(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        types = source[source.index('typedef enum {\n    MP6_WS_EXTEND_TILES'):source.index('#define MP6_WS_EXTEND_MAX')]
        subject = source[source.index('/* Boundary-edge extension:'):source.index('void mp6_widescreen_extend_ring(')]
        # This slice predates the shape-mirror section in the source file but
        # extends past it.  The edge fixture has no shape registry, so remove
        # those four real functions rather than accidentally compiling a
        # second dispatcher that references production globals.
        for name in ('mp6_ws_extend_shape_apply', 'mp6_ws_extend_shape_faces',
                     'mp6_widescreen_extend_shape_mirror', 'mp6_widescreen_shape_update'):
            subject = subject.replace(function(source, name), '')
        # The shared ring registrar is intentionally part of this slice: the
        # edge fixture also exercises the opt-in ring mode. Its writer lives
        # just above the boundary marker, so include that isolated dependency
        # explicitly in the extracted TU.
        ring_writer_start = source.index('static void mp6_ws_extend_write_faces_ring(')
        ring_writer_end = source.index('\n/* --------------------------------------------------------------------------\n * Public registration entry points', ring_writer_start)
        ring_writer = source[ring_writer_start:ring_writer_end]
        ring_apply_start = source.index('static void mp6_ws_extend_ring_apply(')
        ring_apply_end = source.index('\n/* --------------------------------------------------------------------------\n * PILLAR apply', ring_apply_start)
        ring_apply = source[ring_apply_start:ring_apply_end]
        # The production dispatcher covers every registered extension, but
        # this extracted edge TU only needs the ring branch. Keep the fixture
        # independent of unrelated apply helpers while retaining real ring
        # reapplication behavior.
        ring_dispatch = (
            'static void mp6_ws_extend_apply(MP6WSExtendEntry *e, float k)\n'
            '{\n    mp6_ws_extend_ring_apply(e, k);\n}\n')
        subject = ring_apply + '\n' + ring_dispatch + '\n' + ring_writer + '\n' + subject
        subject = subject.replace(function(source, 'mp6_widescreen_static_margins'), '')
        self.assertIn('void mp6_widescreen_extend_strip_rim(', subject)
        self.assertIn('HSF_FACE_TRISTRIP', subject)
        self.assertIn('rimApplied && e->lastRimScale == k', subject)
        with tempfile.TemporaryDirectory(prefix='wide-edge-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'edge_types.inc').write_text(types)
            (directory/'edge_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG', '-I'+str(directory),
                str(ROOT/'tests/native/widescreen_edge_selftest.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_material_filtered_terrain_extension_uses_mixed_tri_quad_fixture(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        types = source[source.index('typedef enum {\n    MP6_WS_EXTEND_TILES'):source.index('#define MP6_WS_EXTEND_MAX')]
        edge_match = re.search(
            r'^static void mp6_ws_extend_edge_apply\(MP6WSExtendEntry \*e, float k\)\n\{',
            source, re.M)
        self.assertIsNotNone(edge_match)
        edge_start = edge_match.start()
        edge = source[edge_start:source.index('\nstatic void mp6_ws_extend_rect(', edge_start)]
        terrain_match = re.search(
            r'^static int mp6_ws_floor_corners\(HSF_OBJECT \*o, HSF_FACE \*f, float y\)\n\{',
            source, re.M)
        self.assertIsNotNone(terrain_match)
        terrain_start = terrain_match.start()
        terrain = source[terrain_start:source.index('\nvoid mp6_widescreen_extend_floor_at(', terrain_start)]
        for name in ('mp6_ws_extend_shape_apply', 'mp6_ws_extend_shape_faces',
                     'mp6_widescreen_extend_shape_mirror', 'mp6_widescreen_shape_update'):
            terrain = terrain.replace(function(source, name), '')
        self.assertIn('mp6_widescreen_extend_terrain_outer_sides_material(', terrain)
        self.assertIn('mp6_widescreen_extend_wall_outer_sides(', terrain)
        rect_start = source.index('static void mp6_ws_extend_rect(')
        rect = source[rect_start:source.index('\nvoid mp6_widescreen_extend_sides(', rect_start)]
        static_start = source.index('void mp6_widescreen_static_margins(')
        static_margins = source[static_start:source.index('\nvoid mp6_widescreen_extend_floor_at(', static_start)]
        self.assertIn('mp6_widescreen_static_margins(', static_margins)
        reflect_apply = function(source, 'mp6_ws_extend_reflect_faces_apply')
        reflect_writer = function(source, 'mp6_ws_extend_write_faces_reflect')
        reflect_range = function(source, 'mp6_widescreen_extend_reflected_face_range')
        self.assertIn('MP6_WS_EXTEND_REFLECT_FACES', types)
        self.assertIn('0, 2, 1, 3', reflect_writer)
        self.assertIn('reflected range crosses seam plane', reflect_range)
        subject = (function(source, 'mp6_ws_edge_coord') + '\n' + edge + '\n' + rect +
                   '\n' + terrain + '\n' + reflect_apply + '\n' + reflect_writer +
                   '\n' + reflect_range + '\n' + function(source, 'mp6_widescreen_extend_billboard_mirror') +
                   '\n' + function(source, 'mp6_widescreen_extend_floor_at') +
                   '\n' + function(source, 'mp6_widescreen_extend_floor'))
        with tempfile.TemporaryDirectory(prefix='wide-extrude-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'extrude_types.inc').write_text(types)
            (directory/'extrude_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                '-I'+tmp, str(ROOT/'tests/native/widescreen_extrude_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_mgmrenshodll_registers_only_the_authored_upper_strip_rims(self):
        source = patched('src/REL/mgmrenshodll/course.c')
        self.assertEqual(source.count('mp6_widescreen_extend_strip_rim('), 2)
        self.assertIn('if (modelIndex == 0)', source)
        self.assertIn('"grid537", 500.0f, 550.0f', source)
        names = ('grid267', 'grid274', 'grid275', 'grid276', 'grid277', 'grid278',
                 'grid279', 'grid280', 'grid281', 'grid282', 'grid283')
        for name in names:
            self.assertEqual(source.count('"' + name + '"'), 1)
        self.assertIn('} else if (modelIndex == 1)', source)
        calls = source.index('mp6_widescreen_extend_strip_rim(')
        camera = source.index('Hu3DModelCameraSet(', calls)
        self.assertLess(calls, camera)

    def test_curved_floor_extension_is_background_only(self):
        scene = patched('src/REL/m633dll/input_model.c')
        self.assertIn('mp6_widescreen_extend_floor(*stageObject->mdlId, "633stageA", 170.0f, 0.0f, 0.0f);', scene)
        self.assertEqual(scene.count('mp6_widescreen_extend_floor('), 1)
        self.assertIn('HuDataSelHeapReadNum(DATANUM(DATA_m633, 0),', scene)

    def test_m630_extends_floor_and_native_height_tint_together(self):
        scene = patched('src/REL/m630dll/game.c')
        self.assertIn('mp6_widescreen_extend_material_sides(model, "m630_yuka01_jimen", 7, 1);', scene)
        self.assertIn('mp6_widescreen_extend_color_rim(model, "m630_yuka01_orange",', scene)
        self.assertIn('mp6_widescreen_extend_backdrop_strip(model, "m630_ushiro", 2.0f);', scene)
        # No lowering, whole-floor scale or player/collision changes.
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/m630dll/game.c').read_text()
        scene = scene.replace('#include "mp6_widescreen.h"\n', '')
        scene = re.sub(r'        if \(i == 1\) \{\n            /\* The 30%-opaque.*?^        \}\n', '', scene, flags=re.M|re.S)
        scene = scene.replace('        if (i == 1)\n            mp6_widescreen_extend_backdrop_strip(model, "m630_ushiro", 2.0f);\n', '')
        self.assertEqual(scene, original)

    def test_m662_continues_hills_and_both_atlas_walls_only(self):
        relative = 'src/REL/m662dll/puzzle.c'
        source = patched(relative)
        setup = function(source, 'fn_1_8C08')
        self.assertIn('static int mp6_m662_hill_surface(const char *name)\n'
                      '{\n    return strcmp(name, "bg_field") == 0;\n}', source)
        for side, boundary in (('L', '612.94458f'), ('R', '-612.94458f')):
            self.assertIn(f'"hei_{side}", 2, {boundary}, 1200.0f, 1.0f);', setup)
        self.assertEqual(setup.count('mp6_widescreen_extrude_model_selective_horizontal('), 1)
        self.assertEqual(setup.count('mp6_widescreen_extend_tiled_sloped_rim('), 2)
        self.assertNotIn('mp6_widescreen_extend_terrain_sides(', setup)
        # Strip only the four presentation registrations. Loading order,
        # motions, transforms, activation bounds and collision stay native.
        setup, count = re.subn(r'        if \(modelIndex == (?:3|5)\) \{\n'
                               r'            /\* (?:This|Follow).*?^        \}\n',
                               '', setup, flags=re.M|re.S)
        self.assertEqual(count, 2)
        setup, count = re.subn(r'        if \(modelIndex == [78]\)\n'
                               r'            mp6_widescreen_extend_tiled_sloped_rim\([^\n]+\n[^\n]+\);\n',
                               '', setup)
        self.assertEqual(count, 2)
        original = (Path(apply_patches.require_pinned_decomp())/relative).read_text()
        self.assertEqual(setup, function(original, 'fn_1_8C08'))

    def test_m662_wall_front_profile_clears_the_wide_frustum(self):
        import math
        # Captured camera and all five near-profile rails, not just a bbox.
        camera = (0, 1083.86499, 2755.63525)
        forward = (0, 214-camera[1], -446-camera[2])
        length = math.sqrt(sum(v*v for v in forward))
        forward = tuple(v/length for v in forward)
        rails = (
            ((-84.71054,233.121643,612.456604), (-51.095577,205.971649,302.305389)),
            ((8.719114,0,612.922791), (37.062241,0,306.489288)),
            ((36.056130,202.029648,612.917114), (64.762924,177.879639,303.345825)),
            ((24.988760,162.270432,612.823242), (53.516987,143.371155,304.241577)),
            ((31.569681,162.282486,612.944580), (60.495991,142.867981,303.941376)),
        )
        old_visible = 0
        for k in (1, 1.325, 1.75):
            extent = math.tan(math.radians(17.5499992)/2)*1.33*k
            for p, q in rails:
                span = p[2]-q[2]
                origin = (p[0]-675, p[1], p[2]*1.2-528)
                slope = ((p[0]-q[0])/span, (p[1]-q[1])/span, 1.2)
                def edge_plane(reach):
                    point = tuple(v+reach*(k-1)*d for v,d in zip(origin,slope))
                    depth = sum(f*(v-c) for f,v,c in zip(forward,point,camera))
                    return point[0]+extent*depth  # <=0 is outside the left edge
                self.assertLess(edge_plane(1200), 0)
                old_visible += edge_plane(620.790473) > 0
        self.assertGreater(old_visible, 0)  # whole native span still leaves an end-cap onscreen

    def test_m675_registers_measured_scenery_without_changing_prop_logic(self):
        relative = 'src/REL/m675Dll/gameplay.c'
        source = patched(relative)
        original = (Path(apply_patches.require_pinned_decomp())/relative).read_text()
        setup = function(source, 'fn_1_7684')
        self.assertIn('"obj24", 1, 2,\n                -1356.09485f, 13000.0f, 1.0f);', setup)
        self.assertIn('mp6_widescreen_extend_billboard_mirror(model, "yama", -1);', setup)
        self.assertIn('mp6_widescreen_extend_shape_mirror(model, "umi");', setup)
        self.assertNotIn('mp6_m675_side_haze', source)
        self.assertNotIn('Hu3DHookFuncCreate', setup)
        self.assertNotIn('mirror_shape', source)
        self.assertIn('"siba3", 0.0f, 2.0f, TRUE);', setup)
        self.assertNotIn('"sunahama"', setup)
        setup, count = re.subn(r'        if \(param->modelResource == 19\) \{\n.*?^        \}\n',
                               '', setup, flags=re.M|re.S)
        self.assertEqual(count, 1)
        self.assertEqual(setup, function(original, 'fn_1_7684'))

    def test_m675_all_seawall_profile_rails_clear_the_wide_frustum(self):
        import math
        camera = (-3.77205992, 644.288635, 1882.66284)
        target = (-3.77205992, 465.179199, -95.8640976)
        forward = tuple(t-c for t,c in zip(target,camera))
        length = math.sqrt(sum(v*v for v in forward))
        forward = tuple(v/length for v in forward)
        rails = ((2538.79712,107.086296), (2538.79712,87.0862808),
                 (2953.0979,107.086296), (2953.0979,87.0862808),
                 (4015.38696,-342.913727), (4015.38696,-92.19944),
                 (4015.38696,87.0862808), (-2619.11694,87.0862808))
        exposed = 0
        for k in (1, 1.325, 1.75):
            for x, y in rails:
                def edge(reach):
                    p = (x*.162-3077.28027, y+92.19944,
                         (-1356.09485+reach*(k-1))*.162-2060.32812)
                    d = tuple(v-c for v,c in zip(p,camera))
                    depth = sum(a*b for a,b in zip(d,forward))
                    return d[0] + math.tan(math.pi/8)*1.2*k*depth
                self.assertLess(edge(13000), 0)
                exposed += edge(0) > 0
        self.assertGreater(exposed, 0)
        # Eight source-length atlas segments cover the greatest displacement.
        self.assertLess(13000*.75, 8*1234.566)

    def test_m659_bottom_reach_covers_native_camera_without_reframing(self):
        import math
        scene = patched('src/REL/m659Dll/scene.c')
        self.assertIn('mp6_widescreen_extend_color_rim_bottom(model, "642nightSKYa1",\n'
                      '                                          0.0f, 21.9824f, -18000.0f);', scene)
        # Captured HSF transform: local Y -> 3*Y-7000, plane Z ->
        # 19767.638 + surfaceDepth + 21.715. Opening depth is the worst case.
        def dot(a, b): return sum(x*y for x,y in zip(a,b))
        def cross(a,b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
        def unit(v): return tuple(x/math.sqrt(dot(v,v)) for x in v)
        required = []
        for ship_x in (-1200, 0, 1200):
            forward = unit((-ship_x,-5600,17650))
            right = unit(cross(forward,(0,1,0)))
            up = cross(right,forward)
            for aspect in (0.6, 0.8, 1.05):
                for side in (-1,1):
                    ray = tuple(f-u+side*aspect*r for f,u,r in zip(forward,up,right))
                    depth = 19767.6377+7500+21.7149+650
                    local_y = (600+ray[1]*depth/ray[2]+7000)/3
                    required.append(local_y)
                    self.assertGreater(local_y, -18000)  # appended edge over-covers
        self.assertLess(min(required), -13804.7529)  # negative control exposes a strip
        from tests.integration.build_widescreen_qa import m659_gradient_probe
        probe = m659_gradient_probe(scene)
        self.assertEqual(probe.count('MP6_WS_QA_ORIGINAL_GRADIENT'), 1)
        self.assertIn('mp6_widescreen_extend_color_rim(model, "642nightSKYa1", 2, 0.0f, 21.9824f);', probe)
        # Negative control changes only this registration, not camera/game state.
        before, after = probe.split('    if (getenv("MP6_WS_QA_ORIGINAL_GRADIENT"))', 1)
        tail = after.split('                                              0.0f, 21.9824f, -18000.0f);', 1)[1]
        self.assertEqual(before[len('#include <stdlib.h>\n'):] +
                         '    mp6_widescreen_extend_color_rim_bottom(model, "642nightSKYa1",\n'
                         '                                          0.0f, 21.9824f, -18000.0f);' + tail, scene)

    def test_untextured_rim_real_growth_and_native_gradient_preservation(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        types = source[source.index('typedef enum {\n    MP6_WS_EXTEND_TILES'):source.index('#define MP6_WS_EXTEND_MAX')]
        grow = source[source.index('static BOOL mp6_ws_extend_grow('):source.index('/* A radial gap is useful only')]
        edge = source[source.index('/* Boundary-edge extension:'):source.index('void mp6_widescreen_extend_ring(')]
        for name in ('mp6_ws_extend_shape_apply', 'mp6_ws_extend_shape_faces',
                     'mp6_widescreen_extend_shape_mirror', 'mp6_widescreen_shape_update'):
            edge = edge.replace(function(source, name), '')
        # The boundary slice now contains the shared ring registrar, whose
        # body is irrelevant to this color-only fixture. Keep this extracted
        # TU focused and drop the registrar rather than pulling its ring face
        # writer (which the color fixture never exercises).
        ring_start = source.index('static void mp6_ws_extend_ring_mode(')
        ring_end = source.index('\nvoid mp6_widescreen_extend_ring(', ring_start)
        edge = edge.replace(source[ring_start:ring_end], '')
        edge = edge.replace(function(source, 'mp6_widescreen_static_margins'), '')
        with tempfile.TemporaryDirectory(prefix='wide-color-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'edge_types.inc').write_text(types)
            (directory/'color_subject.inc').write_text(grow+'\n'+edge)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG', '-I'+str(directory),
                str(ROOT/'tests/native/widescreen_color_selftest.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_tiled_side_capacity_and_outward_slope_guard_are_bounded(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        rect = function(source, 'mp6_ws_extend_rect_mode')
        wrapper = function(source, 'mp6_widescreen_extend_tiled_rim_sides')
        # M675's measured 34-edge profile expands to 272 eight-sided tiles;
        # the bounded registry must admit it without making the generic path
        # unbounded.  The wrapper must opt into both-side, sloped, tiled mode.
        self.assertIn('#define MP6_WS_EXTEND_EDGE_MAX 288', source)
        self.assertIn('TRUE, FALSE', wrapper)
        self.assertIn('TRUE, tolerance, TRUE', wrapper)
        self.assertIn('pending.edgeCount + 8 > MP6_WS_EXTEND_EDGE_MAX', rect)
        self.assertIn('float direction = copysignf(1.0f, reach);', rect)
        self.assertIn('edge.posSlope[j].x *= direction;', rect)
        self.assertIn('edge.posSlope[j].y *= direction;', rect)
        self.assertIn('edge.posSlope[j].z *= direction;', rect)

    def test_cave_components_preserve_all_platform_contact_rings(self):
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        begin = source.index('#define MP6_WS_COMPONENT_VERTICES')
        end = source.index('void mp6_widescreen_extrude_model_selective_horizontal(')
        selective = source[begin:end]
        locate = source[source.index('static int mp6_ws_mesh_is_cluster_target('):source.index('/* --------------------------------------------------------------------------\n * TILES apply')]
        with tempfile.TemporaryDirectory(prefix='wide-components-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'components_subject.inc').write_text(selective+'\n'+locate)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG', '-I'+str(directory),
                str(ROOT/'tests/native/widescreen_components_selftest.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        scene = patched('src/REL/m668DLL/callbacks.c')
        self.assertEqual(scene.count('mp6_widescreen_extrude_outer_components('), 1)
        self.assertIn('work->effect[0].model, "nami", 910.0f', scene)
        self.assertEqual(scene.count('mp6_widescreen_extrude_model_selective_horizontal('), 1)
        self.assertIn('work->actorModel[0], "yougan", 1', scene)
        self.assertIn('work->actorModel[1], "yougan_b", 0', scene)
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/m668DLL/callbacks.c').read_text()
        self.assertEqual(re.findall(r'.*collisionModel.*', scene), re.findall(r'.*collisionModel.*', original))
        self.assertEqual(re.findall(r'.*work->sceneModel\[1\].*', scene), re.findall(r'.*work->sceneModel\[1\].*', original))

    def test_lift_leapers_extends_brick_boundaries_not_platforms_or_collision(self):
        scene = patched('src/REL/m618dll/game.c')
        calls = re.findall(r'portWideWallCreate\(([^;]+)\);', scene)
        self.assertEqual(len(calls), 5)
        for call in calls:
            self.assertTrue(call.startswith('i, '), call)
            self.assertIn('_kabe0', call)
        self.assertEqual(scene.count('portWideBackdropCreate('), 4)
        self.assertIn('DATANUM(DATA_m618, 79), "mekureru00", -1, 0.0f, 0.0f', scene)
        self.assertIn('"ita01", 1, 0.0f, 0.0f', scene)
        self.assertIn('"4st_haikei01", 2,', scene)
        self.assertIn('"4st_haikei02", 2,', scene)
        original = (Path(apply_patches.require_pinned_decomp())/'src/REL/m618dll/game.c').read_text()
        self.assertEqual(function(scene, 'fn_1_4BE0').replace('    portWideWallsUpdate();\n', ''),
                         function(original, 'fn_1_4BE0'))
        self.assertEqual(re.findall(r'.*collisionModels.*', scene), re.findall(r'.*collisionModels.*', original))
        with tempfile.TemporaryDirectory(prefix='wide-lift-', dir=ROOT/'build') as tmp:
            exe = Path(tmp)/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG',
                str(ROOT/'tests/native/m618_widescreen_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_tiled_sprite_band_resize_ownership_and_rollback(self):
        with tempfile.TemporaryDirectory(prefix='wide-sprite-', dir=ROOT/'build') as tmp:
            exe = Path(tmp)/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG',
                str(ROOT/'tests/native/widescreen_sprite_selftest.c'),
                str(ROOT/'src/hsf/mp6_widescreen_sprite.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        source = patched('src/REL/m656dll/game.c')
        self.assertIn('mp6_widescreen_sprite_band(work->sprGroup, 11, 0, 64.0f, 0.0f, 608.0f)', source)
        self.assertIn('index < HuSprGrpData[work->sprGroup].sprNum', function(source, 'fn_1_65E0'))

    def test_space_backdrops_do_not_widen_gameplay_models(self):
        source = patched('src/REL/m651dll/players.c')
        self.assertIn('mp6_widescreen_extrude_model_xy(lbl_1_bss_64.sceneModels[4]);', source)
        source = patched('src/REL/m656dll/game.c')
        self.assertIn('mp6_widescreen_extrude_model_repeat(work->model38);', source)

    def test_m659_extends_only_the_sky_gradient_border(self):
        relative = 'src/REL/m659dll/scene.c'
        source = patched(relative)
        call = ('    mp6_widescreen_extend_color_rim_bottom(model, "642nightSKYa1",\n'
                '                                          0.0f, 21.9824f, -18000.0f);\n')
        create = function(source, 'fn_1_51F0')
        self.assertLess(create.index('model = obj->mdlId[3] = Hu3DModelCreate('), create.index(call.strip()))
        self.assertLess(create.index(call.strip()), create.index('Hu3DJointMotion('))
        extension = ('    /* The original gradient ends at local Y=-13804.753, projecting to row\n'
                     '     * 461.56 at the opening depth (18.44 native pixels short of the bottom).\n'
                     '     * The bottom frustum reaches Y=-17660.68 with lateral ship motion at\n'
                     '     * 21:9; -18000 adds overscan. Continue its colors even at native aspect. */\n' + call)
        self.assertEqual(source.count(extension), 1)
        original = (Path(apply_patches.require_pinned_decomp())/relative).read_text()
        self.assertEqual(source.replace('#include "mp6_widescreen.h"\n', '').replace(extension, ''), original)

    def test_actual_scene_captures_use_full_frame_and_native_buffers(self):
        mdsel = patched('src/REL/mdseldll/mdsel.c')
        source = function(patched('src/game/hsfanim.c'),'CopyWaterFb') + '\n' + function(
            patched('src/REL/fileseldll/filesel.c'),'fn_1_7DEC') + '\n' + function(
            mdsel,'mp6_mdsel_capture')
        with tempfile.TemporaryDirectory(prefix='wide-copy-',dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'capture_subject.inc').write_text(source)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-UNDEBUG','-I'+str(directory),
                str(ROOT/'tests/native/widescreen_capture_selftest.c'),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        filesel = patched('src/REL/fileseldll/filesel.c')
        hook = function(filesel,'fn_1_7E58')
        self.assertIn('cam->fov, cam->aspect,',hook)
        self.assertIn('/ mp6_widescreen_render_width();',hook)
        self.assertIn('lbl_1_bss_C, 640, 480, GX_TF_RGB565',hook)
        self.assertIn('mp6_mdsel_capture(lbl_1_bss_28);',function(mdsel,'fn_1_2A78'))
        original=(Path(apply_patches.require_pinned_decomp())/'src/REL/mdseldll/mdsel.c').read_text()
        for api in ('GXGetTexBufferSize','GXInitTexObj'):
            self.assertEqual(re.findall(api+r'\([^;]+',mdsel),re.findall(api+r'\([^;]+',original))

    def test_mdmic_and_mdsing_feedback_captures_use_live_frame_and_single_ao_exclusion(self):
        mdmic = patched('src/REL/mdmicdll/effects.c')
        mdsing = patched('src/REL/mdsingdll/stage.c')
        for source in (mdmic, mdsing):
            self.assertIn('#include "mp6_widescreen.h"', source)
            self.assertIn('#include "mp6_ambient_occlusion.h"', source)
        subject = '\n'.join((
            function(mdmic, 'fn_1_35B6C'),
            function(mdmic, 'fn_1_366CC'),
            function(mdsing, 'fn_1_2D170'),
            function(mdsing, 'fn_1_2DCD0'),
        ))
        # The mdsing hook is dormant from its current caller; this still runs
        # the exact extracted body so its future activation cannot regress the
        # capture contract silently.
        with tempfile.TemporaryDirectory(prefix='wide-feedback-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'feedback_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG',
                '-I'+str(directory), str(ROOT/'tests/native/widescreen_feedback_selftest.c'),
                '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_custom_sprite_clip_and_map_share_centered_canvas(self):
        score = function(patched('src/game/gamemes.c'),'ScoreBoxDraw')
        self.assertIn('mp6SprScissorSet(sprite);',score)
        self.assertNotIn('GXSetScissor(',score)
        board = patched('src/board/scroll.c')
        self.assertIn('-mp6_widescreen_half_width_delta()',function(board,'MapDraw'))
        self.assertIn('576.0f + mp6_widescreen_half_width_delta()',function(board,'MapDraw'))
        # Projected map markers are still native logical units, not widened twice.
        self.assertNotIn('mp6_widescreen_',function(board,'MapPathDraw'))

    def test_m668_screen_composites_keep_allocations_and_particle_proportions(self):
        source = patched('src/REL/m668DLL/callbacks.c')
        for name in ('fn_1_23B4','fn_1_27D0','fn_1_2FA0','fn_1_3CE4'):
            effect = function(source,name)
            self.assertIn('GXSetViewport((0.0f), (0.0f), mp6_widescreen_render_width(),',effect)
            self.assertIn('GXSetScissor(0U, 0U, mp6_widescreen_render_width(), 480U);',effect)
        for name in ('fn_1_27D0','fn_1_3CE4'):
            effect = function(source,name)
            self.assertIn('GXSetTexCopySrc(0U, 0U, mp6_widescreen_render_width(), 480U);',effect)
            self.assertIn('GXSetTexCopyDst(640U, 480U, GX_TF_RGB565, 0U);',effect)
        effect = function(source,'fn_1_3CE4')
        self.assertIn('halfWidth = particleSize / mp6_widescreen_scale_factor();',effect)
        self.assertNotIn('particleX + particleSize',effect)
        self.assertIn('particleY + particleSize',effect)


if __name__ == '__main__':
    unittest.main()
