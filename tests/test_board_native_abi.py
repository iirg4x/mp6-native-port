"""Exercise the actual patched board curves and native event object layouts."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


def patched(relative):
    source = (Path(build.DECOMP) / relative).read_text(encoding='utf-8')
    patch = ROOT / 'compat/decomp' / (relative + '.patch')
    return apply_patches.apply_unified_diff(source, patch.read_text()) if patch.exists() else source


def function(source, name):
    match = re.search(r'^(?:static inline |static )?(?:float|void) ' + name +
                      r'\([^;]+?\)\n\{.*?^\}', source, re.M | re.S)
    if match is None:
        raise AssertionError('Missing source function: ' + name)
    return match.group()


class BoardNativeABI(unittest.TestCase):
    def test_motion_loader_preserves_camera_and_morph_track_index(self):
        source = (ROOT / 'src/hsf/hsf_load_native.c').read_text()
        load = source[source.index('static void LoadMotion('):]
        start = load.index('        case HSF_TRACK_TRANSFORM:')
        end = load.index('        case HSF_TRACK_CLUSTER:', start)
        definitions = '\n'.join(line.split('/*', 1)[0] for line in source.splitlines()
                                if line.startswith('#define FTRACK_'))
        subject = definitions + '''
static void subject_load(LoadCtx *ctx, const u8 *trec, HSF_TRACK *out) {
    u16 rawTarget = be16(trec + FTRACK_TARGET);
    switch (trec[FTRACK_TYPE]) {
''' + load[start:end] + '\n    }\n}\n'
        self.compile_run(subject, 'hsf_camera_track_selftest.c')
        self.compile_run(subject.replace('out->index = be16(trec + FTRACK_UNION2);', ''),
                         'hsf_camera_track_selftest.c', expect_success=False)

    def test_results_registers_hor_plus_camera_floor_and_theater_border(self):
        source=patched('src/REL/mdpresultdll/mdpresult.c')
        party=patched('src/REL/mdpartydll/mdparty.c')
        camera=re.findall(r'mp6_widescreen_camera_widen\([^;]+;',source)
        self.assertEqual(len(camera),1)
        self.assertIn('mp6_widescreen_camera_widen(',party)
        self.assertRegex(camera[0],r'\(1, 30\.0f, 10\.0f, 10000\.0f,\s*1\.2f, 480\.0f\)')
        floor=re.findall(r'mp6_widescreen_extrude_model_border_xz\([^;]+;',source)
        self.assertEqual(floor,['mp6_widescreen_extrude_model_border_xz(obj->mdlId[0], "yuka");'])
        self.assertEqual(source.count('mp6_widescreen_extend_results_set(obj->mdlId[0]);'),1)
        # Results asset 0 also contains the stage and decorations. Duplicating
        # or scaling the whole model would change those, not just the floor.
        self.assertNotIn('mp6_widescreen_floor_border_fill_dup(',source)
        self.assertNotIn('Hu3DCameraPerspectiveSet(',source)

    def test_results_theater_pins_edges_without_moving_native_art(self):
        source=(ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        names=('mp6_ws_pillar_near','mp6_ws_pillar_face_corners',
               'mp6_ws_pillar_emission_slot','mp6_ws_pillar_stage_quad',
               'mp6_ws_results_border_edges','mp6_ws_extend_write_faces_results',
               'mp6_ws_extend_results_apply')
        subject='\n'.join(re.search(r'^static (?:BOOL|s16|void) '+name+r'\([^;]+?\)\n\{.*?^\}',
                                    source,re.M|re.S).group() for name in names)
        self.compile_run(subject,'results_theater_selftest.c')
        self.compile_run(subject.replace('800.0f * (k > 1.0f ? k - 1.0f : 0.0f)', '0.0f'),
                         'results_theater_selftest.c',expect_success=False)
        self.compile_run(subject.replace('{b.st, a.st, a.st, b.st}', '{0, 0, 0, 0}'),
                         'results_theater_selftest.c',expect_success=False)
        registrar=function(source,'mp6_widescreen_extend_results_set')
        for name in ('obj28','obj29','stage_base','stage_base2'):
            self.assertIn('"'+name+'"',registrar)
        self.assertIn('mp6_ws_extend_results_apply(e, k);',source)

    def test_cross_copy_wipe_resamples_live_width_into_original_allocation(self):
        source=patched('src/game/wipe.c')
        body=re.search(r'^static BOOL WipeCrossFade\(void\)\n\{.*?^\}',source,re.M|re.S).group()
        self.assertIn('WipeCaptureFrame(wipeData.image[0]);',body)
        subject=function(source,'WipeCaptureFrame')+'\nstatic void subject_capture(void) {\n'
        subject+='    WipeCaptureFrame(wipeData.image[0]);\n}\n'
        self.compile_run(subject,'wipe_cross_copy_selftest.c')
        self.compile_run(subject.replace('mp6_widescreen_render_width()','640'),
                         'wipe_cross_copy_selftest.c',expect_success=False)
        broken_destination = re.sub(
            r'HU_FB_WIDTH\s*/\s*2\s*,\s*HU_FB_HEIGHT\s*/\s*2',
            'mp6_widescreen_render_width()/2, HU_FB_HEIGHT/2', subject)
        self.assertNotEqual(broken_destination, subject)
        self.compile_run(broken_destination, 'wipe_cross_copy_selftest.c', expect_success=False)
        self.assertRegex(body, r'GXGetTexBufferSize\(\s*HU_FB_WIDTH\s*/\s*2\s*,\s*HU_FB_HEIGHT\s*/\s*2\s*,')
        self.assertRegex(body, r'Hu3DTexLoad\(\s*wipeData\.image\[0\]\s*,\s*HU_FB_WIDTH\s*/\s*2\s*,\s*HU_FB_HEIGHT\s*/\s*2\s*,')

    def test_crack_sqrt_uses_native_double_intrinsic(self):
        source=patched('src/board/capsule.c')
        declaration=re.search(r'^extern double __frsqrte\([^;]+;',source,re.M).group()
        subject=declaration+'\n'+function(source,'CapEffCrackSqrt')+'\n'+function(source,'CapEffCrackSqrtStore')
        self.compile_run(subject,'capsule_crack_math_selftest.c')

    def test_results_custom_sprite_scissor_tracks_widescreen(self):
        source=patched('src/game/sprput.c')
        subject=function(source,'mp6SprScissorSet')
        self.compile_run(subject,'sprite_scissor_selftest.c')
        self.compile_run(subject.replace('float dx = 0.5f * (width - HU_FB_WIDTH);',
                                        'float dx = 0;'),
                         'sprite_scissor_selftest.c',expect_success=False)
        self.assertIn('mp6SprScissorSet(sp);',source)

    def test_last5_schedule_uses_match_round_not_day_cycle(self):
        body = function(patched('src/board/board.c'), 'mbMain')
        start = body.index('    if (GwSystem.turnPlayerNo == 0 && !turnInterruptF && GwSystem.turnMax')
        end = body.index('    mbMusBoardPlay();', start)
        subject = 'static void check_last5(int turnInterruptF) {\n' + body[start:end] + '\n}'
        self.compile_run(subject, 'last5_schedule_selftest.c')
        self.compile_run(subject.replace('GwSystem.turnMax - GwSystem.turnNo < 5', 'GwSystem.turnNo >= 2'),
                         'last5_schedule_selftest.c', expect_success=False)

    def test_snpc_effects_use_live_widescreen_board_camera(self):
        board_camera = function(patched('src/board/camera.c'), 'CameraOMExec')
        calls = re.findall(r'Hu3DCamera(?:PerspectiveSet|ViewportSet)\([^;]+;', board_camera)
        self.assertEqual(len(calls), 2)
        self.assertNotIn('Hu3DCameraScissorSet', board_camera)
        self.compile_run('static void subject(MBCAMERA *cameraP) {\n' + '\n'.join(calls) + '\n}',
                         'snpc_widescreen_selftest.c')
        snpc = patched('src/board/snpc.c')
        self.assertIn('GX_TG_POS, GX_TEXMTX4', function(snpc, 'FadeMatHook'))
        self.assertNotRegex(snpc, r'GXSet(?:Viewport|Scissor)|MTX(?:Ortho|Perspective)')
        self.assertFalse((ROOT/'src/os/board_constants.c').exists())

    def test_player_reactions_hold_after_loop_and_blend(self):
        motion = patched('src/game/hsfmotion.c')
        # Whole definitions, including backslash-continued lines.
        defs = '\n'.join(re.findall(r'^#define HU3D_MOTATTR_(?:[^\n]*\\\n)*[^\n]*', motion, re.M))
        playback = defs + '\n' + '\n'.join(function(motion, name) for name in
                                          ('Hu3DMotionShiftSet', 'Hu3DMotionNext'))
        def subject(source):
            event = re.search(r'^static int ev_CapDonkeyStart\([^;]+?\)\n\{.*?^\}',
                              source, re.M | re.S).group()
            call = re.search(r'mbPlayerMotionShiftSet\(playerNo, 9,[^;]+;', event).group()
            return playback + '\nstatic void subject_dk_reaction(int playerNo) {\n' + call + '\n}'
        original = (Path(build.DECOMP) / 'src/board/capspecial.c').read_text(encoding='utf-8')
        # The recovered owner now supplies the correct one-shot itself.
        self.compile_run(subject(original), 'player_motion_selftest.c')
        self.compile_run(subject(original).replace(
            'mbPlayerMotionShiftSet(playerNo, 9, 0.0f, 8.0f, 0);',
            'mbPlayerMotionShiftSet(playerNo, 9, 0.0f, 8.0f, HU3D_MOTATTR_LOOP);'),
            'player_motion_selftest.c', expect_success=False)
        self.compile_run(subject(patched('src/board/capspecial.c')), 'player_motion_selftest.c')

    def test_shop_carousel_parks_panels_outside_live_canvas(self):
        source = patched('src/board/shopevent.c')
        subject = function(source, 'ev_ShopDescPosSet')
        self.compile_run(subject, 'shop_widescreen_selftest.c')
        broken = subject.replace('offset * (mp6_widescreen_scale_factor() - 1.0f)', '0.0f')
        self.compile_run(broken, 'shop_widescreen_selftest.c', expect_success=False)
        self.assertEqual(source.count('ev_ShopDescPosSet(descWinId['), 3)

    def test_boo_fade_resets_untextured_material_state(self):
        original = (Path(build.DECOMP) / 'src/board/capspecial.c').read_text(encoding='utf-8')
        def subject(source):
            layout = re.search(r'typedef struct \w+ \{[^{}]*\} TERESA_FADE_WORK;',source,re.M|re.S).group()
            body = function(source,'ev_CapTeresaFadeMatHook')
            # Exercise the actual early-return material paths; the existing
            # one-texture fade path remains in the GPU gameplay test.
            body = body[:body.index('    GXSetNumTexGens(2);')]
            body += '}\n' * (body.count('{') - body.count('}'))
            return layout+'\nstatic TERESA_FADE_WORK *teresaFadeWork;\n'+body
        self.compile_run(subject(original),'teresa_material_selftest.c',expect_success=False)
        self.compile_run(subject(patched('src/board/capspecial.c')),'teresa_material_selftest.c')

    def test_boo_capture_and_projection_track_live_camera(self):
        source=patched('src/board/capspecial.c')
        body=function(source,'ev_CapTeresaFadeMatHook')
        capture=body[body.index('    if (!work->copyF)'):body.index('    if (material->attrNum')]
        projection=body[body.index('    camera = &Hu3DCamera'):body.index('    PSMTXInverse')]
        subject='static void subject_capture(Work *work) {\n'+capture+'}\n'
        subject+='static void subject_project(void) { Camera *camera; float fov; Mtx perspective;\n'+projection+'}\n'
        self.compile_run(subject,'boo_widescreen_selftest.c')
        self.compile_run(subject.replace('mp6_widescreen_render_width()','640'),
                         'boo_widescreen_selftest.c',expect_success=False)
        self.compile_run(subject.replace('camera->aspect > 0.0f ? camera->aspect : lbl_802C4368','lbl_802C4368'),
                         'boo_widescreen_selftest.c',expect_success=False)

    def test_board_minigames_use_the_game_reload(self):
        # board/mgcall.c hands off to instdll and the board reloads after the
        # results overlay, where mbMain's own end-of-match check runs. The
        # stub-era continue-in-place patch to board.c is gone.
        self.assertFalse((ROOT / 'compat/decomp/src/board/board.c.patch').exists())
        self.assertIn('src/board/mgcall.c', dict(build.board_sources()))

    def test_duel_keeps_the_games_own_no_minigame_roll(self):
        # With no duel minigame to roll (or two computers and computer games
        # hidden), the game settles the duel with its own weighted roll. No
        # port gate may skip the duel before stakes are set.
        body = function(patched('src/board/capspecial.c'), 'mbev_CapKettou')
        self.assertNotIn('mbMgRouletteNumGet(6) == 0', body)
        original = function((Path(build.DECOMP) / 'src/board/capspecial.c').read_text(encoding='utf-8'),
                            'mbev_CapKettou')
        self.assertIn('mbMgRouletteNumGet(6) <= 0', original)

    def test_short_archive_reader_big_endian_index(self):
        original = (Path(build.DECOMP) / 'src/game/data.c').read_text(encoding='utf-8')
        def subject(source):
            # data.c's own size/mask macros come along with the function.
            defs = '\n'.join(re.findall(r'^#define DATA_\w+.*$', source, re.M))
            return defs + '\n' + re.search(r'^void \*HuDataReadNumHeapShortForce\([^;]+?\)\n\{.*?^\}',
                                           source,re.M|re.S).group()
        self.compile_run(subject(original), 'data_short_read_selftest.c', expect_success=False)
        self.compile_run(subject(patched('src/game/data.c')), 'data_short_read_selftest.c')

    def test_gx_array_lifetime_and_binding_replacement(self):
        source = (ROOT / 'src/gx/aurora_bridge.c').read_text(encoding='utf-8')
        start = source.index('typedef struct {\n    GXAttr attr;\n    void  *data;')
        end = source.index('/* Retained-frame interpolation identity tap.', start)
        self.compile_run(source[start:end], 'gx_array_lifetime_selftest.c')

    def test_animation_registry_restores_with_game_arena(self):
        self.compile_run('', 'anim_registry_selftest.c')

    def test_capsule_particle_draw_spans(self):
        event = patched('src/board/capevent.c')
        capsule = patched('src/board/capsule.c')
        pieces = []
        for source, tag in [(event, 'CapEffGlowParticleData'),
                            (event, 'CapEffRayParticleWork'),
                            (capsule, 'CapEffCrackData_s'),
                            (capsule, 'CapEffData_s')]:
            pieces.append(re.search(r'typedef struct ' + tag + r' \{.*?^\} \w+;',
                                    source, re.M | re.S).group())
        pieces.append('''
typedef struct {int num; CAPEFFGLOWPARTICLEWORK *data; HuVecF *vertices; HuVec2f *texCoords;} QA_GLOW;
typedef struct {int num,vtxNum; CAP_EFF_CRACK_DATA *data; HuVecF *vtx; HuVec2f *st;} QA_CRACK;
typedef struct {int num; CAP_EFF_DATA *data; HuVecF *vertex; HuVec2f *st;} QA_EFFECT;
''')
        for source, draw, signature, count in [
            (event, 'mbev_CapEffRayDraw', 'qa_ray(CAPEFFRAYPARTICLEWORK *particleWorkP)', 2),
            (event, 'ev_CapEffDraw', 'qa_glow(QA_GLOW *workP)', 3),
            (capsule, 'CapEffCrackDraw', 'qa_crack(QA_CRACK *work)', 3),
            (capsule, 'CapEffDraw', 'qa_effect(QA_EFFECT *effP)', 3),
        ]:
            body = function(source, draw)
            self.assertNotIn('GXSetArray(', body, draw)
            calls = re.findall(r'mp6_gxarray_bind_span\([^;]+;', body)
            self.assertEqual(len(calls), count, draw)
            pieces.append('static void ' + signature + '{\n' + '\n'.join(calls) + '\n}')
        self.compile_run('\n\n'.join(pieces), 'capsule_particle_spans_selftest.c')

    def test_last5_fade_uses_model_positions_without_mesh_uvs(self):
        original = (Path(build.DECOMP) / 'src/board/snpc.c').read_text(encoding='utf-8')
        def subject(source):
            layout = re.search(r'typedef struct MBOBJFADEWORK \{.*?^\} MBOBJFADEWORK;',
                               source, re.M | re.S).group()
            return layout + '\n' + function(source, 'FadeMatHook')
        # SNPC recovery on main now owns the position-input fix. Keep a
        # negative mutation so this test still proves the old crash path.
        self.compile_run(subject(original).replace('GX_TG_POS', 'GX_TG_TEX0'),
                         'last5_fade_selftest.c', expect_success=False)
        self.compile_run(subject(original), 'last5_fade_selftest.c')
        self.compile_run(subject(patched('src/board/snpc.c')), 'last5_fade_selftest.c')

    def test_graphics_cache_generation_preserves_legacy_cache(self):
        source = (ROOT / 'src/gx/aurora_bridge.c').read_text(encoding='utf-8')
        subject = re.search(r'^char \*mp6_bridge_gpu_cache_path\([^;]+?\)\n\{.*?^\}',
                            source, re.M | re.S).group()
        self.compile_run(subject, 'gpu_cache_path_selftest.c')
        main = (ROOT / 'src/main_native.c').read_text(encoding='utf-8')
        self.assertIn('config.cachePath = gpuCachePath;', main)
        self.assertRegex(main, r'if \(gpuCachePath == NULL\) \{[^}]+return 2;')

    def compile_run(self, subject, test, expect_success=True):
        with tempfile.TemporaryDirectory(prefix='board-native-abi-', dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            (directory / 'board_subject.inc').write_text(subject, encoding='utf-8')
            exe = directory / 'board-abi.exe'
            result = subprocess.run(
                [build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-fno-strict-aliasing',
                 '-UNDEBUG', '-Werror=incompatible-function-pointer-types',
                 '-I', str(directory), str(ROOT / 'tests/native' / test), '-o', str(exe)],
                cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            if expect_success:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PASS', result.stdout)
            else:
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('FAIL', result.stdout)

    def test_curve_callbacks_integrals_and_inverse(self):
        source = patched('src/REL/w01Dll/world01.c')
        self.assertNotIn('(W01CurveEval)(u32)', source)
        self.assertNotIn('(*W01CurveEval)()', source)
        math = (Path(build.DECOMP) / 'src/board/math.c').read_text(encoding='utf-8')
        pieces = [function(math, name) for name in ['mbBezierCalcSlope', 'mbHermiteCalcSlope']]
        # The recovered curve helpers live in the module's shared header.
        common = patched('include/REL/w01Dll/common.h')
        typedef = re.search(r'typedef float \(\*W01CurveEval\).*?;', source + common, re.S).group()
        pieces.append(typedef)
        def defined(name):
            try:
                return function(source, name)
            except AssertionError:
                return function(common, name)
        pieces += [defined(name) for name in [
            'fn_1_14A90', 'mp6W01BezierEval', 'fn_1_14BF0', 'fn_1_14108',
            'fn_1_142B0', 'fn_1_144C0', 'fn_1_147DC', 'w01CurveLen2',
            'w01CurveT', 'w01CurveLen', 'W01HermiteIntegrate', 'W01CurveNewton']]
        self.compile_run('\n\n'.join(pieces), 'board_curves_selftest.c')

    def test_capsule_views_and_full_donkey_copy(self):
        source = patched('src/board/capspecial.c')
        self.assertNotRegex(source, r'\(u8\s*\*\)work\s*\+\s*0x(?:BAC|B74)')
        self.assertNotIn('0xBF4', source)
        self.assertNotIn('work->guideObj', source)
        self.assertIn('work->eventData[i + 2] = ids[i];', source)
        self.assertIn('ids[i] = work->eventData[i + 2];', source)
        declarations = []
        for tag in ['EvCapWork', 'CapWorkFlag', 'CapWork']:
            declarations.append(re.search(r'typedef struct ' + tag + r' \{.*?^\} \w+;',
                                          source, re.M | re.S).group())
        owner = patched('src/board/capevent.c')
        owner_layout = re.search(r'typedef struct CapWork \{.*?^\} CAPWORK;', owner, re.M | re.S).group()
        # The owners name the payload's first word differently (_unkB6C,
        # effectModelId, unusedWorkValueB6C); the selftest checks its offset.
        def payload_named(layout):
            return re.sub(r'(CAPWORKFLAG flags;[^\n]*\n\s*int )\w+;', r'\1_unkB6C;', layout, count=1)
        declarations.append(payload_named(owner_layout).replace('struct CapWork', 'struct OwnerCapWork')
                            .replace('} CAPWORK;', '} OWNER_CAPWORK;'))
        copy = re.search(r'    omObj->data = HuMemDirectMallocNum\([^\n]+\n'
                         r'    memcpy\(omObj->data, work, [^\n]+', source).group()
        declarations.append('static void subject_copy(OMOBJ *omObj, CAPWORK *work) {\n' + copy + '\n}')
        self.compile_run('\n\n'.join(declarations), 'capsule_work_selftest.c')
        # Every other capsule owner must agree, not just the allocator.
        for name in ('capmove', 'capthrow', 'captrap'):
            other = patched('src/board/'+name+'.c')
            layout = re.search(r'typedef struct(?: \w+)? \{[^{}]*\} CAPWORK;',
                               other, re.M | re.S).group()
            layout = re.sub(r'typedef struct(?: \w+)? \{',
                            'typedef struct OwnerCapWork {', layout, count=1)
            layout = payload_named(layout.replace('} CAPWORK;', '} OWNER_CAPWORK;'))
            self.compile_run('\n\n'.join(declarations[:3]+[layout, declarations[-1]]),
                             'capsule_work_selftest.c')

    def test_capspecial_native_api_matches_recovered_owners(self):
        source = patched('src/board/capspecial.c')
        header = (ROOT/'include/mp6_board_compat.h').read_text()
        self.assertIn('int (*beginHook)(int, int)', header)
        self.assertIn('typedef int (*TERESA_STEAL_BEGIN_HOOK)(int, int);', source)
        self.assertIn('extern void CharEffectHipDropCreate(s16 charNo, HuVecF *pos);', source)
        self.assertIn('void CharEffectHipDropCreate(s16 charNo, HuVecF *pos)',
                      patched('src/game/charman.c'))
        self.assertFalse((ROOT/'src/os/board_constants.c').exists())
        self.assertIn('const float lbl_802C4368 = 1.2f;', source)

    def test_capspecial_dice_callback_matches_caller_type(self):
        source = patched('src/board/capspecial.c')
        callback = re.search(r'^static u16 ev_CapKoopaDicePadBtnHook\([^;]+?\)\n\{.*?^\}',
                             source, re.M | re.S).group()
        self.assertNotRegex(source, r'\(u16\s*\(\*\)\(int\)\)ev_CapKoopaDicePadBtnHook')
        self.compile_run(callback, 'capspecial_dice_hook_selftest.c')

    def test_dice_number_destruction_detaches_reused_object_slots(self):
        def subject(source):
            stop = re.search(r'^BOOL mbDiceNumStopCheck\([^;]+?\)\n\{.*?^\}',
                             source, re.M | re.S).group()
            return function(source, 'mbDiceNumObjKill')+'\n'+stop
        original=(Path(build.DECOMP)/'src/board/dice.c').read_text()
        self.compile_run(subject(original), 'dice_number_lifetime_selftest.c', expect_success=False)
        self.compile_run(subject(patched('src/board/dice.c')), 'dice_number_lifetime_selftest.c')


if __name__ == '__main__':
    unittest.main()
