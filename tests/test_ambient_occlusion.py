"""AO contracts; GPU/visual evidence is collected by the isolated board runner."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import apply_patches, build
from tests.ao_shader_source import shader_source

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
        self.assertIn('.size = {depthWidth, depthHeight, 1}',renderer)
        self.assertIn('const bool reduced = !upstream && mp6_ao_reduced_depth();',renderer)

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

    def test_fi_ao_markers_preserve_scene_boundaries_and_ignore_replay(self):
        """Run the production marker bodies against a tiny retained stream."""
        source = (ROOT / 'src/gx/frame_interp.c').read_text()
        start = source.index('void mp6_fi_note_ao(')
        end = source.index('void mp6_fi_stream_note_pos_mtx', start)
        marker_bodies = source[start:end]
        with tempfile.TemporaryDirectory(prefix='ao-fi-markers-', dir=ROOT / 'build') as temporary:
            path = Path(temporary) / 'markers.c'
            exe = Path(temporary) / 'markers.exe'
            path.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mp6_ambient_occlusion.h"
#define FI_AO_MAX 96
typedef struct {
    uint32_t offset;
    int kind;
    int camera;
    Mp6AoView view;
} FiAoMarker;
typedef struct {
    FiAoMarker ao[FI_AO_MAX];
    uint32_t aoCount;
    uint32_t size;
} FiStream;
static FiStream s_streams[2];
static int s_cur = -1;
static int s_inReplay;
static void fi_capture_fail(void) { s_cur = -1; }
''' + marker_bodies + r'''
int main(void) {
    Mp6AoView view = {{1, 2, 3, 4}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1}, 2};
    FiAoMarker before[FI_AO_MAX];
    s_cur = 0; s_streams[0].size = 64;
    mp6_fi_note_ao(&view);
    mp6_fi_note_ao_decals(2, 0);
    mp6_fi_note_ao_foliage(2, 0);
    mp6_fi_note_ao_foliage(2, 1);
    assert(s_streams[0].aoCount == 4);
    assert(s_streams[0].ao[0].kind == 0 && s_streams[0].ao[0].view.camera == 2);
    assert(s_streams[0].ao[1].kind == 1 && s_streams[0].ao[2].kind == 3 && s_streams[0].ao[3].kind == 4);
    assert(s_streams[0].ao[0].offset == 64 && s_streams[0].ao[3].offset == 64);
    memcpy(before, s_streams[0].ao, sizeof(before));
    s_inReplay = 1;
    mp6_fi_note_ao(&view); mp6_fi_note_ao_decals(2, 1); mp6_fi_note_ao_foliage(2, 1);
    s_inReplay = 0;
    assert(s_streams[0].aoCount == 4 && memcmp(before, s_streams[0].ao, sizeof(before)) == 0);
    s_streams[0].size = 128;
    view.camera = 3;
    mp6_fi_note_ao(&view);
    assert(s_streams[0].aoCount == 5 && s_streams[0].ao[4].offset == 128 && s_streams[0].ao[4].view.camera == 3);
    s_cur = -1;
    mp6_fi_note_ao_decals(3, 0); mp6_fi_note_ao_foliage(3, 0);
    assert(s_streams[0].aoCount == 5);
    puts("FI AO marker boundaries and replay immutability: PASS");
    return 0;
}
''')
            result = subprocess.run([build.ZIG, 'cc', '-std=c11', '-O2', '-UNDEBUG',
                '-I' + str(ROOT / 'include'), str(path), '-o', str(exe)],
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
        self.assertIn('if (aoIdx != cur->aoCount) return fi_replay_build_fail()', replay)
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
        # Only continuous water and exact sky panels override texture alpha. Colored
        # foliage still retains its original TEV/filtered alpha contribution.
        ordinary, water = foliage.split('    if (mp6AoWaterDrawing || mp6AoBackgroundDrawing) {', 1)
        water = water.split('static void SetHiliteTexMtx', 1)[0]
        self.assertNotIn('GXSetTevColorIn(', ordinary)
        self.assertNotIn('GXSetTevAlphaIn(', ordinary)
        self.assertIn('GXSetTevKAlphaSel(GX_TEVSTAGE0,GX_TEV_KASEL_1)', water)
        self.assertIn('GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0)', water)
        self.assertNotIn('GXSetZMode', water)
        self.assertIn('GX_BL_SRCALPHA,GX_BL_INVSRCALPHA', foliage)
        self.assertIn('GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE)', foliage)
        self.assertIn('FaceDraw(draw,saved->face);', foliage)

    def test_water_coverage_is_removed_before_ao_depth_preparation(self):
        # Execute the C++ source builder so its no-decal and MSAA replacements
        # cannot silently remove the water exclusion or leave dead decal reads.
        for msaa, decals in ((False, False), (False, True), (True, False), (True, True)):
            with self.subTest(msaa=msaa, decals=decals):
                source = shader_source(prepared_depth=True, attachment_depth=True,
                                       multisampled_depth=msaa, decals=decals)
                preparation = source.split('@fragment fn fs_decal_depth', 1)[1].split('struct FoliageSeed', 1)[0]
                self.assertIn('clamp(at, vec2i(0), vec2i(textureDimensions(foliageColor)) - 1)', preparation)
                exclusion = 'if (textureLoad(foliageColor, maskAt, 0).a > 0.5) { return 0.0; }'
                self.assertIn(exclusion, preparation)
                self.assertLess(preparation.index(exclusion), preparation.index('let current ='))
                # Only mask alpha controls occluder eligibility. Ordinary
                # premultiplied foreground RGB must not erase land depth.
                self.assertNotIn('.rgb', preparation)
                self.assertIn('textureLoad(sceneDepth, at, 0);', preparation)
                if decals:
                    self.assertIn('prepare_depth(select(current, before, current == after && before != after))', preparation)
                else:
                    self.assertIn('return prepare_depth(current);', preparation)
                    self.assertNotIn('textureLoad(decalBefore', preparation)
                    self.assertNotIn('textureLoad(decalAfter', preparation)
        raw = shader_source()
        preparation = raw.split('@fragment fn fs_decal_depth', 1)[1].split('struct FoliageSeed', 1)[0]
        self.assertNotIn('foliageColor', preparation)
        self.assertIn('return select(current, before, current == after && before != after);', preparation)

    def test_water_preparation_bindings_preserve_other_pipeline_contracts(self):
        renderer = (ROOT / 'src/gx/ambient_occlusion.cpp').read_text()
        self.assertIn('bool waterDepthMask = false', renderer)
        self.assertIn('if (waterDepthMask && i == baseCount) entries[i].binding = 5;', renderer)
        self.assertIn('make_layout(device, decals ? 2 : 0, true, msaa, true, canonicalDepth)', renderer)
        self.assertIn('make_layout(device,0,true,seedDepthSamples>1)', renderer)
        self.assertIn('make_layout(device, half ? 5 : 4)', renderer)
        preparation = renderer.split('const auto &pipeline = attachment_depth_pipeline', 1)[1].split('if (upstream)', 1)[0]
        water = '.binding = 5, .textureView = job.foliage ? job.foliage : g_emptyFoliage'
        self.assertIn(water, preparation)
        self.assertLess(preparation.index(water), preparation.index('.binding = 3'))
        self.assertIn('uint32_t entryCount = 3;', preparation)
        self.assertIn('if (decals)', preparation)
        self.assertIn('entries[entryCount++] = {.binding = 3', preparation)
        self.assertIn('entries[entryCount++] = {.binding = 4', preparation)
        self.assertIn('if (job.canonicalDepth)', preparation)
        self.assertIn('entries[entryCount++] = {.binding = 7', preparation)
        self.assertIn('.entryCount = entryCount', preparation)
        self.assertIn('static_assert(sizeof(Params) == 80)', renderer)

    def test_water_debug_view_does_not_hide_cleared_water_depth(self):
        builder = (ROOT / 'tests/integration/build_board_qa.py').read_text()
        diagnostic = builder.split("water = shader.replace(", 1)[1].split(
            "legacy = replace_once(shader", 1)[0]
        self.assertIn('textureLoad(foliageColor,maskAt,0).a>0.0', diagnostic)
        self.assertIn('if (!waterPixel && linear_depth(pixel.xy/p.sizes.xy)==0.0)', diagnostic)
        self.assertIn('return vec4f(0,0,0,1);', diagnostic)
        self.assertIn('select(0.0,1.0,waterPixel)', diagnostic)


if __name__ == '__main__':
    unittest.main()
