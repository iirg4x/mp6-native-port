"""Compile-time menu visibility and player-copy contracts, not Android device tests."""
from pathlib import Path
import re
import hashlib
import json
import struct
import subprocess
import tempfile
import unittest
from tools import build
from tests.source_tokens import compact_source

ROOT = Path(__file__).resolve().parents[1]


def native_only(source):
    """Drop #ifdef __EMSCRIPTEN__ blocks (and their #else halves stay)."""
    out, skipping, depth = [], False, 0
    for line in source.splitlines():
        stripped = line.strip()
        if not skipping and stripped.startswith('#ifdef __EMSCRIPTEN__'):
            skipping, depth = True, 0
            continue
        if skipping:
            if stripped.startswith('#if'):
                depth += 1
            elif stripped.startswith('#endif'):
                if depth == 0:
                    skipping = False
                    continue
                depth -= 1
            elif stripped.startswith('#else') and depth == 0:
                skipping = False
            continue
        out.append(line)
    return '\n'.join(out)


class PlayerSettings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/gx/ui/settings.cpp').read_text(encoding='utf-8')
        # Test the actual platform branches with a real C++ preprocessor.
        # Header declarations are irrelevant to row visibility.
        source = re.sub(r'^#include .*$', '', source, flags=re.M)
        cls.variants = {}
        for platform, defines in [('desktop', ['-DMP6_AURORA_LIVE_AA']),
                                  ('android', ['-D__ANDROID__'])]:
            result = subprocess.run([build.ZIG, 'c++', '-E', '-P', '-x', 'c++',
                                     *defines, '-'], input=source, text=True,
                                    capture_output=True, cwd=ROOT, check=True)
            cls.variants[platform] = result.stdout

    def test_desktop_actions_never_appear_on_android(self):
        for text in ['Window Mode', 'Window Size', '.key = "Display"',
                     'Open Save Folder', 'Quick Save (F5)', 'Quick Load (F8)',
                     'Ultra (SSAA']:
            self.assertIn(text, self.variants['desktop'])
            self.assertNotIn(text, self.variants['android'])
        self.assertIn('Drag with one finger', self.variants['android'])
        for text in ['Touch Controls', 'Control Opacity', 'Floating Sticks', 'Edit Layout']:
            self.assertIn(text, self.variants['android'])
            self.assertNotIn(text, self.variants['desktop'])

    def test_options_have_one_logical_home(self):
        expected = {'picture': ['Preset', 'Widescreen', 'Unlocked FPS', 'Anti-Aliasing',
                              'Shadow Quality', 'Ambient Occlusion'],
                    'sound': ['Master Volume', 'More Sound Effects'],
                    'extras': ['Fast Forward', 'Quick Party', 'Free Camera', 'Extra Memory', 'Graphics Driver']}
        for platform, source in self.variants.items():
            # VSync is desktop-only: Android and the web use a fixed present mode.
            expected['picture'] = expected['picture'][:6] + (['VSync'] if platform == 'desktop' else [])
            tabs = dict(re.findall(r'void SettingsWindow::build_(\w+)\((.*?)(?=\nvoid SettingsWindow::|\nbool SettingsWindow::|$)',
                                   source, re.S))
            for tab, keys in expected.items():
                for key in keys:
                    self.assertIn(f'.key = "{key}"', tabs[tab])
                    self.assertEqual(source.count(f'.key = "{key}"'), 1)

    def test_launch_override_mask_is_cached_without_caching_live_preferences(self):
        for source in self.variants.values():
            update = source.split('void SettingsWindow::update()', 1)[1]
            mask = re.search(r'static const unsigned launchDisabledFlags\s*=([^;]+);', update)
            self.assertIsNotNone(mask)
            self.assertEqual(mask.group(1).count('std::getenv('), 14)
            self.assertIn('source.disabledFlags = launchDisabledFlags;', update)
            self.assertIn('source.aa = config.aa;', update)
            self.assertIn('source.masterVolume = config.masterVolume;', update)
            self.assertIn('source == mStorySource', update)

    def test_redesign_preserves_original_setting_inventory(self):
        original = {'Preset', 'Widescreen', 'Shadow Quality', 'Anti-Aliasing',
                    'Ambient Occlusion', 'Unlocked FPS', 'Show FPS Counter',
                    'Master Volume', 'More Sound Effects', 'Fast Forward',
                    'Quick Party', 'Free Camera', 'Unlock All', 'Extra Memory', 'Graphics Driver'}
        for platform, source in self.variants.items():
            platform_keys = ({'Window Mode', 'Window Size', 'Display', 'VSync'} if platform == 'desktop'
                             else {'Show Touch Controls', 'Control Size', 'Control Opacity',
                                   'Floating Sticks'})
            self.assertEqual(set(re.findall(r'\.key\s*=\s*"([^"]+)"', source)),
                             original | platform_keys)

    def test_no_raw_paths_or_developer_help(self):
        for source in self.variants.values():
            literals = ' '.join(re.findall(r'"(?:[^"\\]|\\.)*"', source))
            for obsolete in ['Free-Run Tick', 'Content Root', 'Clear Path',
                             'frame boundary', 'texel', 'environment lever',
                             'live stacks', 'retail heaps', 'byte-identical']:
                self.assertNotIn(obsolete, literals)

    def test_story_art_is_transparent_and_separate(self):
        from PIL import Image
        for name in ['brighton', 'twila', 'cloud']:
            with Image.open(ROOT / 'res/menu' / f'story-{name}.png') as image:
                self.assertEqual(image.mode, 'RGBA')
                self.assertLessEqual(max(image.size), 2048)
                self.assertEqual(image.getchannel('A').getextrema(), (0, 255))

    def test_layout_is_native_not_flattened_art(self):
        source = (ROOT / 'src/gx/ui/settings.cpp').read_text(encoding='utf-8')
        css = (ROOT / 'res/rml/settings.rcss').read_text(encoding='utf-8')
        for component in ['story-book', 'book-cover', 'chapter-heading', 'story-cast',
                          'story-preview', 'settings-footer']:
            self.assertIn(component, source)
        self.assertIn('mp6scene://live', source)
        self.assertIn('GetDensityIndependentPixelRatio', source)
        self.assertNotIn('.storybook.compact', css)
        self.assertNotIn('card-art', source)
        self.assertNotIn('settings-storybook-approved.png', source)
        self.assertNotIn('color: inherit', css)
        self.assertNotIn('animation-delay', source)

    def test_save_actions_include_the_slot_number(self):
        source = self.variants['desktop']
        self.assertIn('Save to Slot {}', source)
        self.assertIn('Load Slot {}', source)

    def test_live_preview_borrows_the_current_gpu_scene(self):
        # Native code only: the web build's UI texture check reads back on purpose.
        patch = native_only((ROOT / 'third_party/aurora/lib/rmlui/WebGPURenderInterface.cpp').read_text())
        self.assertIn('m_frameSeedView', patch)
        self.assertIn('mp6scene://live', patch)
        self.assertNotIn('CopyTextureToBuffer', patch)
        self.assertNotIn('MapAsync', patch)

    def test_storybook_batches_in_painter_order_without_a_launcher_cap(self):
        view = compact_source((ROOT / 'src/gx/ui/storybook_view.hpp').read_text())
        submit = view.split(compact_source('void submit('), 1)[1].split(
            compact_source('Rml::Texture texture('), 1
        )[0]
        render_draw = view.split(compact_source('void render_draw('), 1)[1].split(
            compact_source('void compile_draw('), 1
        )[0]
        self.assertLess(submit.index('v.position=project'), submit.index('flush_batch()'))
        self.assertIn('texture==mBatchTexture', submit)
        self.assertIn('if(shader){', submit)
        self.assertIn('if(draw.shader)', render_draw)
        self.assertIn('draw.geometry.Render(mOffset,draw.texture,*draw.shader)', render_draw)
        self.assertIn('draw.geometry.Render(mOffset,draw.texture)', render_draw)
        self.assertIn('render_draw(draws.back())', view)
        self.assertIn('offset+index', submit)
        self.assertNotIn('std::sort', submit)
        self.assertLess(view.index(compact_source('flush_batch(); // Submit before restoring')),
                        view.index('SetScissorRegion(oldScissor)'))
        launcher = compact_source((ROOT / 'src/gx/ui/launcher_core.cpp').read_text()).split(
            compact_source('extern "C" int mp6_launcher_run_menu('), 1
        )[1]
        self.assertNotIn('16666667', launcher)
        self.assertNotIn('nextFrameNs', launcher)
        self.assertIn('mp6_host_sleep_ns(1000000ull)', launcher)

    def test_approved_storybook_assets_are_the_current_preview_assets(self):
        preview = ROOT / 'docs/design/storybook-generated'
        manifest = json.loads((preview / 'manifest.json').read_text())
        for name, asset in manifest['assets'].items():
            if name in {'cloud', 'cloud-bank', 'cloud-round'}:
                continue  # Replaced by the preview's code-native cloud export.
            source = preview / asset['file']
            native = ROOT / 'res/menu/storybook' / f'{name}.png'
            self.assertEqual(hashlib.sha256(source.read_bytes()).digest(),
                             hashlib.sha256(native.read_bytes()).digest(), name)
        self.assertEqual((preview / 'assets/TitanOne-Regular.ttf').read_bytes(),
                         (ROOT / 'res/fonts/TitanOne-Regular.ttf').read_bytes())

    def test_live_scene_alpha_is_not_ui_coverage(self):
        patch = ''.join((ROOT / 'third_party/aurora' / name).read_text() for name in (
            'lib/rmlui/WebGPURenderInterface.cpp', 'lib/rmlui/pipeline.cpp'))
        self.assertIn('SetupRenderState(translation, textureData->m_scene', patch)
        self.assertIn('select(texel.a, 1.0, uniforms.translation.z > 0.5)', patch)

    def test_launcher_turn_and_framed_chapter_icons(self):
        source = compact_source((ROOT / 'src/gx/ui/storybook_view.hpp').read_text())
        launcher = compact_source((ROOT / 'src/gx/ui/prelaunch.cpp').read_text())
        self.assertIn('storybook_geometry::launcher_logo(float(d.x),float(d.y))', source)
        self.assertIn('media_frame(mount.paper);strip(image.x,image.y,image.w,image.h,logo', source)
        self.assertIn('media_corners(mount.paper)', source)
        self.assertNotIn('610*mAxisScale.x/d.x', source)
        self.assertIn('settings->turn_from_launcher(*mStorybook)', launcher)
        self.assertIn('mPrevious=source.mState', source)
        self.assertIn('storybook_geometry::chapter_tab_offset', source)
        self.assertIn('state.chapter=="Picture"&&state.inGame', source)
        self.assertNotIn('texture("game-preview")', source)
        self.assertIn('"chapter-picture"', source)
        self.assertIn('media_frame(photo)', source)
        self.assertIn('media_frame(mount.paper)', source)
        self.assertIn('media_corners(photo)', source)
        self.assertIn('media_corners(mount.paper)', source)
        self.assertIn('storybook_geometry::preview_photo(mAxisScale.x,mAxisScale.y,320,274)', source)
        mount = source.split(compact_source('void media_frame('), 1)[1].split(
            compact_source('void page('), 1
        )[0]
        self.assertNotIn('0xb88a33', mount)  # No rigid gold wall-frame rim.
        self.assertNotIn('0xf6cf66', mount)

    def test_native_book_preserves_each_paper_face_and_short_transitions(self):
        source = compact_source((ROOT / 'src/gx/ui/storybook_view.hpp').read_text())
        for name in ['book-left', 'book-right', 'paper-left', 'paper-right']:
            self.assertIn('"' + name + '"', source)
            self.assertTrue((ROOT / 'res/menu/storybook' / (name + '.png')).is_file())
        # The left print and the right print must not be substituted/mirrored.
        self.assertIn('art(mSide?"paper-right":"paper-left"', source)
        self.assertNotIn('0,mSide==0,false', source)
        self.assertIn('introOutroTimeScale=2.0', source)
        for key, expected in [('turnDuration', .28), ('entranceDuration', .68), ('exitDuration', .40)]:
            value = float(re.search(key + r'=(\.\d+)', source).group(1))
            self.assertEqual(value, expected)
        for expression in ['entranceDuration=.68*introOutroTimeScale', 'exitDuration=.40*introOutroTimeScale',
                           'presentation_progress()*entranceDuration<.30*introOutroTimeScale',
                           'mPresentation.target(false,motion,clock(),entranceDuration,exitDuration)',
                           'mPresentation.target(true,motion,clock(),entranceDuration,exitDuration)']:
            self.assertIn(expression, source)
        self.assertIn('mLeftReveal=host_reveal(.29,.26)', source)
        self.assertIn('mRightReveal=host_reveal(.40,.26)', source)
        self.assertIn('storybook_geometry::host_spring(reveal(delay,duration))', source)
        self.assertIn('p=anchor+(p-anchor)*spring', source)
        self.assertIn('p.y+=70*(1-spring)', source)
        self.assertIn('mAxisScale={size.x/width,size.y/height}', source)
        self.assertIn('storybook_geometry::preview_photo(mAxisScale.x,mAxisScale.y,sceneWidth,sceneHeight)', source)

    def test_scenery_continues_behind_the_book(self):
        source = compact_source((ROOT / 'src/gx/ui/storybook_view.hpp').read_text())
        preview = ROOT / 'docs/design/storybook-generated'
        self.assertNotRegex(source, r'\btext\(caption,')
        self.assertIn(compact_source('float angle=0'), source)
        self.assertIn('x+=std::sin(t*.24f+phase)*2', source)
        self.assertIn('angle=std::sin(t*.24f+phase)*.18f', source)

        def native_points(name, fields):
            body = re.search(r'constfloat' + name + r'\[\]\[' + str(fields) + r'\]=\{(.*?)\};', source, re.S).group(1)
            return [tuple(float(v.strip().rstrip('f')) for v in row.split(','))
                    for row in re.findall(r'\{([^{}]+)\}', body)]

        balloons, stars = native_points('balloons', 5), native_points('sky', 3)
        self.assertEqual(len(balloons), 12)
        self.assertEqual(len(stars), 23)
        for points, xmin, xmax in [(balloons, 100, 770), (stars, 880, 1610)]:
            interior = [p for p in points if xmin < p[0] < xmax and 120 < p[1] < 850]
            self.assertGreaterEqual(len(interior), 8)
            # Both columns and both height bands under the normally opaque book
            # contain decorations; a border-only layout cannot satisfy this.
            for left in [True, False]:
                for top in [True, False]:
                    self.assertTrue(any((p[0] < (xmin+xmax)/2) == left and (p[1] < 485) == top
                                        for p in interior))
        browser_balloons = [tuple(map(float, row)) for row in re.findall(
            r'\{x: ([\d.]+), y: ([\d.]+), w: ([\d.]+), color: \'#[0-9a-f]+\', phase: ([\d.]+), speed: ([\d.]+)\}',
            (preview / 'backdrop.js').read_text())]
        browser_sky = (preview / 'preview.js').read_text().split('const skyStars = [', 1)[1].split('];', 1)[0]
        browser_stars = [tuple(map(float, row)) for row in re.findall(
            r'\[([\d.]+), ([\d.]+), ([\d.]+), \'#[0-9a-f]+\'\]', browser_sky)]
        self.assertEqual(balloons, browser_balloons)
        self.assertEqual(stars, browser_stars)
        self.assertIn('i<std::size(sky)', source)
        self.assertIn('i<std::size(balloons)', source)
        self.assertLess(source.index('scenery(t);'), source.index('tabs(t);'))
        self.assertLess(source.index('tabs(t);'), source.index('binding();'))

    def test_storybook_resources_are_warmed_before_interaction(self):
        ui = (ROOT / 'src/gx/ui/ui.cpp').read_text()
        settings = (ROOT / 'src/gx/ui/settings.cpp').read_text()
        view = compact_source((ROOT / 'src/gx/ui/storybook_view.hpp').read_text())
        init = ui.split('bool initialize() noexcept', 1)[1].split('void shutdown()', 1)[0]
        self.assertLess(init.index('load_font("fonts/TitanOne-Regular.ttf")'),
                        init.index('SettingsWindow::preload_resources()'))
        self.assertLess(init.index('SettingsWindow::preload_resources()'), init.index('sInitialized = true'))
        preload = settings.split('void SettingsWindow::preload_resources()', 1)[1].split('SettingsWindow::SettingsWindow(', 1)[0]
        for operation in ['StorybookView::preload_resources(*context)',
                          'create_render_texture(1,1,false)', 'geometry.Render({0,0},texture)',
                          'encoder.Finish()', 'g_queue.Submit', 'render_worker::synchronize()',
                          'SettingsWindow warmup(true)', 'context->Update()']:
            self.assertIn(operation, preload)
        for forbidden in ['->Show(', '.show(', 'push_document(', 'cfg_save(', '->enter(',
                          'GetCurrentTexture(', 'g_surface.Present(']:
            self.assertNotIn(forbidden, preload)
        self.assertIn('texture.GetDimensions();', view)
        self.assertNotIn('texture("day-mask")', view)
        self.assertIn('CompileShader("luminance-mask"', view)
        self.assertIn('sprite:storybook_art::sprites', view)
        self.assertIn('engine->GenerateString', view)

    def test_storybook_is_full_window_and_inline_only(self):
        patch = ''.join((ROOT / 'third_party/aurora' / name).read_text() for name in (
            'lib/aurora.cpp', 'lib/rmlui.cpp', 'lib/rmlui.hpp', 'include/aurora/rmlui.hpp'))
        self.assertIn('data-full-window-ui', patch)
        self.assertIn('rmlFullWindow', patch)
        self.assertIn('scene_dimensions()', patch)
        settings = (ROOT / 'src/gx/ui/settings.cpp').read_text()
        self.assertIn('SetAttribute("data-full-window-ui"', settings)
        self.assertIn('mStorybook->enter(cfg().menuMotion != 0 && (!mPrelaunch || mInGame))', settings)
        self.assertIn('(mPrelaunch && !mInGame) || !mStorybook', settings)
        select = (ROOT / 'src/gx/ui/select_button.cpp').read_text()
        self.assertIn('cmd == NavCommand::Confirm', select)
        self.assertIn('mRoot->Closest(".native-storybook")', select)
        self.assertIn('mStep(1)', select)

    def test_sky_rays_stay_inside_the_translating_mask(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            exe = Path(temporary) / 'storybook_geometry_selftest.exe'
            subprocess.run([build.ZIG, 'c++', '-std=c++17', '-O2', '-UNDEBUG',
                            'tests/native/storybook_geometry_selftest.cpp', '-o', str(exe)],
                           cwd=ROOT, check=True, capture_output=True)
            result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors='replace'))
        source = compact_source((ROOT / 'src/gx/ui/storybook_view.hpp').read_text())
        self.assertIn('storybook_geometry::sky_ray_fixed(a,width,height,mAxisScale.x,mAxisScale.y)', source)
        self.assertNotIn('A day-and-night adventure', source)


if __name__ == '__main__':
    unittest.main()
