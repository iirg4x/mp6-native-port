"""AO contracts; GPU/visual evidence is collected by the isolated board runner."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


class AmbientOcclusion(unittest.TestCase):
    def test_mobile_shading_budget_preserves_aspect_and_small_targets(self):
        with tempfile.TemporaryDirectory(prefix='ao-resolution-', dir=ROOT / 'build') as temporary:
            for mobile in (False, True):
                exe=Path(temporary)/('mobile.exe' if mobile else 'desktop.exe')
                result=subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-O2','-UNDEBUG',
                    *(['-D__ANDROID__'] if mobile else []),
                    'tests/native/ao_resolution_selftest.c','-o',str(exe)],
                    cwd=ROOT,capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        renderer=(ROOT/'src/gx/ambient_occlusion.cpp').read_text()
        self.assertIn('mp6_ao_working_size(ctx.targetWidth, ctx.targetHeight, &aoWidth, &aoHeight)',renderer)
        self.assertIn('gfx::create_pass(targets.width,targets.height)',renderer)
        self.assertIn('.size = {fullWidth, fullHeight, 1}',renderer)

    def test_foreground_policy_includes_event_props_not_only_board_scenery(self):
        with tempfile.TemporaryDirectory(prefix='ao-foreground-', dir=ROOT / 'build') as temporary:
            exe = Path(temporary) / 'policy.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                'tests/native/ao_foreground_policy_selftest.c', '-o', str(exe)],
                cwd=ROOT, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        foliage = (ROOT / 'include/mp6_ao_foliage_draw.h').read_text()
        self.assertNotIn('mp6_ground_is_scenery', foliage)
        self.assertIn('mp6_ao_foreground_eligible(draw,face)', foliage)
        self.assertIn('!mp6_enh_ambient_occlusion()', foliage)

    def test_camera_mapping_and_headless_isolation(self):
        with tempfile.TemporaryDirectory(prefix='ao-view-', dir=ROOT / 'build') as temporary:
            for headless in (False, True):
                exe = Path(temporary) / ('headless.exe' if headless else 'windowed.exe')
                result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                    *(['-DMP6_HEADLESS_BUILD'] if headless else []),
                    'tests/native/ambient_occlusion_view_selftest.c',
                    'src/hsf/mp6_ambient_occlusion.c', 'src/enh/mp6_enhancements.c', '-o', str(exe)],
                    cwd=ROOT, capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shading_precedes_hud_and_is_in_each_real_camera(self):
        path = 'src/game/hsfman.c'
        original = (Path(build.DECOMP) / path).read_text(encoding='utf-8')
        current = apply_patches.apply_unified_diff(original,
                    (ROOT / 'compat/decomp' / (path + '.patch')).read_text())
        self.assertIn('        mp6_ao_camera_end(Hu3DCameraNo);\n        if(!NoSyncF)', current)
        self.assertLess(current.index('mp6_ao_camera_end(Hu3DCameraNo)'),
                        current.index('HuSprExec(HUSPR_DRAWNO_FRONT)'))

    def test_streaming_resource_lifetime_and_replay_boundaries(self):
        renderer = (ROOT / 'src/gx/ambient_occlusion.cpp').read_text()
        self.assertIn('candidate.frame != g_frame && candidate.available.load', renderer)
        self.assertIn('job->frame = g_frame', renderer)
        self.assertIn('gfx::synchronize()', renderer)
        self.assertIn('std::array<wgpu::Texture, 2> textures', renderer)
        self.assertIn('job.ao[(i - 1) % 2]', renderer)
        self.assertIn('.view = job.ao[i % 2]', renderer)
        self.assertIn('make_bind_group(ctx.device, job, job.ao[0], true)', renderer)
        self.assertIn('src/gx/ambient_occlusion.cpp', build.HOST_STATE_SECTION_SOURCES)
        replay = (ROOT / 'src/gx/frame_interp.c').read_text()
        self.assertEqual(replay.count('mp6_ao_begin_frame();'), 2)
        self.assertIn('s_replayAo[s_replayAoCount++].offset = out', replay)
        self.assertIn('if (aoIdx != cur->aoCount) return 0', replay)
        self.assertIn('mp6_ao_apply(&marker->view)', replay)
        self.assertIn('mp6_ao_capture_decals(marker->camera, marker->kind == 2)', replay)
        self.assertIn('#define FI_AO_MAX 96', replay)
        self.assertIn('for (auto &decal : g_decals) decal = {};', renderer)
        self.assertIn('decal.width == targets.width && decal.height == targets.height', renderer)

    def test_space_bracket_preserves_original_alpha_and_depth_behavior(self):
        path = 'src/board/masu.c'
        original = (Path(build.DECOMP) / path).read_text(encoding='utf-8')
        current = apply_patches.apply_unified_diff(original,
                    (ROOT / 'compat/decomp' / (path + '.patch')).read_text())
        begin = current.index('mp6_ao_decals(Hu3DCameraNo, FALSE)')
        end = current.index('mp6_ao_decals(Hu3DCameraNo, TRUE)')
        draw = current[begin:end]
        self.assertIn('GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE)', draw)
        self.assertIn('GXSetAlphaCompare(GX_GEQUAL, 1, GX_AOP_AND, GX_GEQUAL, 1)', draw)
        self.assertIn('GXCallDisplayList(masuDisplayList, masuDisplayListLen)', draw)
        self.assertLess(end, current.index('if (masuCapsuleDispF == FALSE'))
        self.assertEqual(current.count('mp6_ao_decals('), 2)

    def test_foreground_color_and_composite_blend_contract(self):
        # The GPU suite executes this blend against colored, overlapping leaves;
        # keep its Python pipeline in sync with the actual native integration.
        renderer = (ROOT / 'src/gx/ambient_occlusion.cpp').read_text()
        self.assertIn('blend.color.srcFactor = wgpu::BlendFactor::One;', renderer)
        self.assertIn('blend.color.dstFactor = wgpu::BlendFactor::SrcAlpha;', renderer)
        self.assertIn('blend.alpha.srcFactor = wgpu::BlendFactor::Zero;', renderer)
        self.assertIn('blend.alpha.dstFactor = wgpu::BlendFactor::One;', renderer)
        self.assertIn('const uint32_t emptyPixel=0;', renderer)
        foliage = (ROOT / 'include/mp6_ao_foliage_draw.h').read_text()
        # Only the continuous water receiver overrides texture alpha. Colored
        # foliage still retains its original TEV/filtered alpha contribution.
        ordinary, water = foliage.split('    if (mp6AoWaterDrawing) {', 1)
        water = water.split('static void SetHiliteTexMtx', 1)[0]
        self.assertNotIn('GXSetTevColorIn(', ordinary)
        self.assertNotIn('GXSetTevAlphaIn(', ordinary)
        self.assertIn('GXSetTevKAlphaSel(GX_TEVSTAGE0,GX_TEV_KASEL_1)', water)
        self.assertIn('GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0)', water)
        self.assertNotIn('GXSetZMode', water)
        self.assertIn('GX_BL_SRCALPHA,GX_BL_INVSRCALPHA', foliage)
        self.assertIn('GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE)', foliage)
        self.assertIn('FaceDraw(draw,saved->face);', foliage)


if __name__ == '__main__':
    unittest.main()
