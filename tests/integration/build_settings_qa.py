"""Isolated hidden, muted native menu interaction/capture harness."""
from pathlib import Path
import os
import sys
import hashlib

ROOT = Path(__file__).resolve().parents[2]


def _option_value(name):
    """Read one path-valued harness option without consuming build.py flags."""
    values = []
    for index, argument in enumerate(sys.argv[1:], 1):
        if argument == name:
            if index + 1 >= len(sys.argv) or sys.argv[index + 1].startswith('--'):
                raise SystemExit(f'{name} requires a path')
            values.append(sys.argv[index + 1])
        elif argument.startswith(name + '='):
            value = argument[len(name) + 1:]
            if not value:
                raise SystemExit(f'{name} requires a path')
            values.append(value)
    if len(values) > 1:
        raise SystemExit(f'{name} may be specified only once')
    return values[0] if values else None


def _resolve_input_path(raw, option):
    if raw is None:
        return None
    path = Path(raw).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    path = path.resolve()
    if not path.is_file():
        raise SystemExit(f'{option} does not name a file: {path}')
    return path


def _write_bytes_if_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def _prepare_storybook_header(output, supplied):
    """Copy a supplied StorybookView and its generated local dependencies.

    The copy is intentionally inside the QA output tree.  Source overlays
    include this absolute copy, so a ``#include "storybook_view.hpp"`` in the
    original source directory cannot accidentally pull the production header
    into one TU while another sees the candidate.  Its local helper and art
    headers are copied beside it because the candidate's quoted includes are
    resolved relative to its own directory.
    """
    if supplied is None:
        return None, None
    header_dir = output / 'storybook-header'
    header = header_dir / 'storybook_view.hpp'
    data = supplied.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    _write_bytes_if_changed(header, data)
    for name in ('storybook_art_data.hpp', 'storybook_geometry.hpp', 'storybook_sky_mesh.hpp'):
        source = ROOT / 'src/gx/ui' / name
        if not source.is_file():
            raise SystemExit(f'cannot stage StorybookView dependency: {source}')
        _write_bytes_if_changed(header_dir / name, source.read_bytes())
    return header, digest


def _rewrite_storybook_source(source, output, header, digest, flags, obj):
    """Overlay one TU so every Storybook-dependent definition uses one copy."""
    filename = Path(source).name
    include_anchor = '#include "storybook_view.hpp"'
    # settings/prelaunch include the class directly. launcher_core/ui only
    # carry the indirect SettingsWindow/Prelaunch users; force the same class
    # definition into them too so a candidate layout cannot be mixed with a
    # production definition in the final QA link.
    indirect = {'launcher_core.cpp', 'ui.cpp'}
    text = Path(source).read_text(encoding='utf-8')
    direct = include_anchor in text
    if not direct and filename not in indirect:
        return source, flags, obj

    include_path = header.resolve().as_posix()
    if direct:
        text = text.replace(include_anchor, f'#include "{include_path}"')
    else:
        text = f'#include "{include_path}"\n' + text
    marker = f'// MP6 QA StorybookView header sha256: {digest}\n'
    if marker not in text:
        text = marker + text

    destination = output / filename
    destination.write_text(text, encoding='utf-8')
    source_flags = list(flags)
    header_include_dir = str(header.parent)
    if not any(source_flags[index:index + 2] == ['-I', header_include_dir]
               for index in range(max(0, len(source_flags) - 1))):
        # settings_qa.hpp is injected into launcher_core.cpp in preview mode
        # and includes StorybookView itself. Its quoted include is resolved
        # from tests/integration before ordinary -I roots, so this staged
        # directory must precede the production src/gx/ui root for that
        # indirect path as well.
        source_flags = ['-I', header_include_dir] + source_flags
    ui_include = str(ROOT / 'src/gx/ui')
    if not any(source_flags[index:index + 2] == ['-I', ui_include]
               for index in range(max(0, len(source_flags) - 1))):
        source_flags += ['-I', ui_include]
    return str(destination), source_flags, str(output / 'obj' / Path(obj).name)


def _qa_output_path(raw, default):
    """Resolve a QA output without permitting production-tree overwrites."""
    if raw is None:
        return default
    output = Path(raw).expanduser().resolve()
    build_root = (ROOT / 'build').resolve()
    try:
        output.relative_to(build_root)
    except ValueError:
        raise SystemExit(f'--output must stay under {build_root}: {output}')
    if output == build_root:
        raise SystemExit(f'--output cannot replace the build root: {output}')

    protected = (
        ROOT / 'build/release',
        ROOT / 'build/release-headless',
        ROOT / 'build/obj-release',
        ROOT / 'build/aurora-release',
        ROOT / 'third_party',
        ROOT / 'src',
        ROOT / 'src/gx/ui',
    )
    for path in protected:
        protected_path = path.resolve()
        if output == protected_path or protected_path in output.parents:
            raise SystemExit(f'--output points into a protected production tree: {output}')
    return output


def main():
    touch = '--touch' in sys.argv
    preview = '--preview' in sys.argv
    perf = '--perf' in sys.argv
    supplied_header = _resolve_input_path(_option_value('--storybook-header'),
                                          '--storybook-header')
    explicit_output = _option_value('--output')
    os.environ.setdefault('MP6_DISC_ROOT', str(ROOT / 'build/disc-cache/orig/GP6E01'))
    os.environ.setdefault('MP6_DECOMP_INC_DATA', str(ROOT / 'build/disc-cache/split/include'))
    sys.path.insert(0, str(ROOT / 'tools'))
    import build
    default_output = ROOT / ('build/settings-qa-perf' if perf else 'build/settings-qa-preview' if preview else 'build/settings-qa-touch' if touch else 'build/settings-qa')
    output = _qa_output_path(explicit_output, default_output)
    output.mkdir(parents=True, exist_ok=True)
    storybook_header, storybook_header_hash = _prepare_storybook_header(output, supplied_header)
    original_configure, original_collect = build.configure_windows, build.collect_units
    original_objects = None

    def configure(configuration):
        nonlocal original_objects
        original_configure(configuration)
        original_objects = Path(build.OBJ_DIR)
        build.BUILD_DIR = str(output)
        build.OBJ_DIR = str(output / 'obj')

    def collect(*args, **kwargs):
        units = []
        for source, flags, name, flavor in original_collect(*args, **kwargs):
            obj = original_objects / name
            filename = Path(source).name
            if filename == 'main_native.c' and preview:
                text = Path(source).read_text(encoding='utf-8')
                anchor = 'launcherMode = mp6_launcher_decide_mode(hasNumericArg, hasInputScript, &showMenu);'
                assert text.count(anchor) == 1
                text = text.replace(anchor, anchor + '\n        launcherMode = 1; showMenu = 1;')
                source = str(output / filename)
                Path(source).write_text(text, encoding='utf-8')
                obj = output / 'obj' / name
            elif filename == 'audio_out_sdl.c':
                text = Path(source).read_text(encoding='utf-8')
                text += '\nfloat mp6_settings_qa_game_gain(void) { return g_stream ? SDL_GetAudioStreamGain(g_stream) : -1.f; }\n'
                source = str(output / filename)
                Path(source).write_text(text, encoding='utf-8')
                flags = [*flags, '-I', str(ROOT / 'src/audio')]
                obj = output / 'obj' / name
            elif filename == 'launcher_core.cpp':
                flags = [*flags, '-I', str(ROOT / 'src/gx/ui')]
                if perf:
                    flags = [*flags, '-DMP6_STORYBOOK_PERF_QA']
                if touch:
                    flags = [*flags, '-DMP6_SETTINGS_TOUCH_QA']
                text = Path(source).read_text(encoding='utf-8')
                anchor = 'g_uiReady = mp6::ui::initialize();'
                assert text.count(anchor) == 1
                text = text.replace(anchor, '''{
        auto* context=aurora::rmlui::get_context();
        const int documents=context->GetNumDocuments();
        auto* focus=context->GetFocusElement();
        const auto dimensions=context->GetDimensions();
        const float density=context->GetDensityIndependentPixelRatio();
        unsigned char before[sizeof(g_cfg)];
        std::memcpy(before,&g_cfg,sizeof(g_cfg));
        const auto start=SDL_GetTicksNS();
        g_uiReady = mp6::ui::initialize();
        printf("[SETTINGS-PERF] startup-prewarm %.3f ms\\n",double(SDL_GetTicksNS()-start)/1e6);
        if(std::memcmp(before,&g_cfg,sizeof(g_cfg)) || documents!=context->GetNumDocuments() ||
           focus!=context->GetFocusElement() || dimensions!=context->GetDimensions() ||
           density!=context->GetDensityIndependentPixelRatio() || mp6::ui::any_document_visible()) {
            fprintf(stderr,"[SETTINGS-QA] FAIL startup warmup changed config, documents or focus\\n");
            std::exit(9);
        }
        printf("[SETTINGS-QA] PASS startup warmup preserves config, documents and focus\\n");
    }''')
                anchor = 'extern "C" int mp6_launcher_run_menu(void *sdlWindowPtr)\n{'
                assert text.count(anchor) == 1
                header = (ROOT / 'tests/integration/settings_qa.hpp').as_posix()
                probe_hash = hashlib.sha256(Path(header).read_bytes() +
                    (ROOT / 'tests/integration/storybook_qa.hpp').read_bytes() +
                    (ROOT / 'tests/integration/storybook_perf.hpp').read_bytes()).hexdigest()
                if preview:
                    text = text.replace(anchor, anchor + '\n    g_window = (SDL_Window *)sdlWindowPtr; return 0;')
                    anchor = 'extern "C" void mp6_launcher_frame_overlay(void)\n{'
                    assert text.count(anchor) == 1
                    text = text.replace(anchor, f'#include "{header}"\n#include "lib/gx/fifo.hpp"\n#include "lib/gfx/common.hpp"\n#include "lib/window.hpp"\nextern "C" float mp6_settings_qa_game_gain(void);\n' + anchor + '''
    extern long mp6_tick_count;
    if (g_gameMenu && (mp6_tick_count == 1800 || mp6_tick_count == 2245)) {
        mp6::ui::cfg().menuMotion=0; // Isolate layout; motion has its own native harness.
        g_gameMenu->show();
        printf("[SETTINGS-QA] live game preview opened at %ld\\n", mp6_tick_count);
    }
    if (mp6_tick_count == 2230) { mp6::ui::cfg().masterVolume=35; mp6_audio_set_master_gain(.35f); }
    if (mp6_tick_count == 2235) g_gameMenu->hide(false);
    // Resize the actual game target while the same menu remains open. Borrowed
    // scene handles and photo fitting must follow, without reopening the menu.
    if (mp6_tick_count == 1850 || mp6_tick_count == 1950 || mp6_tick_count == 2050 || mp6_tick_count == 2150) {
        // Permit a wide test window even with the game explicitly set to 4:3.
        // This changes only the hidden harness window, not game aspect policy.
        SDL_SetWindowAspectRatio(g_window, 0.f, 0.f);
        SDL_SetWindowSize(g_window, mp6_tick_count == 2150 ? 2560 : mp6_tick_count == 1950 ? 1840 : 1280,
                                   mp6_tick_count == 2150 ? 1440 : mp6_tick_count == 2050 ? 720 : mp6_tick_count == 1850 ? 960 : 790);
        aurora::rmlui::set_ui_scale(mp6_tick_count == 2150 ? 2.f : 1.f);
    }
''')
                    anchor = '\n}\n\n/* Pre-shutdown'
                    assert text.count(anchor) == 1
                    text = text.replace(anchor, '''
    if (mp6_tick_count == 1830 || mp6_tick_count == 2231 || mp6_tick_count == 2246) {
        settings_qa_require(g_gameMenuMusic && g_gameMenuMusic->playing() && g_gameMenuMusic->track_count()==6 && g_gameMenuMusic->queued_frames()>0,
            "in-game settings play linked-disc music");
        const auto idleTrack=g_gameMenuMusic->current_track();
        settings_qa_require(idleTrack==2||idleTrack==3||idleTrack==4||idleTrack==6||idleTrack==7||idleTrack==92,
            "in-game settings select an approved idle track");
        settings_qa_require(mp6_settings_qa_game_gain()==0.f,"game audio muted only during menu music");
    }
    if (mp6_tick_count == 2236) {
        settings_qa_require(!g_gameMenuMusic,"closing settings releases menu music");
        settings_qa_require(std::abs(mp6_settings_qa_game_gain()-.35f)<.001f && mp6::ui::cfg().masterVolume==35,
            "closing settings restores the current master volume");
    }
    static aurora::webgpu::TextureWithSampler captureTarget;
    static aurora::webgpu::TextureWithSampler freshnessTarget;
    static Rml::Vector2i freshnessViewport;
    if (mp6_tick_count == 1830 || mp6_tick_count == 1920 || mp6_tick_count == 2020 || mp6_tick_count == 2120 || mp6_tick_count == 2220) {
        // Match Aurora's real end-frame order: finish queued GX draws before
        // opening a UI color pass. Otherwise the capture ends the game's pass
        // while its FIFO still contains draws (a harness-only crash).
        aurora::gx::fifo::drain();
        aurora::gfx::finish();
        auto *context = Rml::GetContext(0);
        const auto size = context->GetDimensions();
        if (mp6_tick_count == 1830) freshnessViewport = size;
        const auto windowSize = aurora::window::get_window_size();
        settings_qa_require(size.x == int(windowSize.native_fb_width) && size.y == int(windowSize.native_fb_height),
            "live menu fills native window independently of game viewport");
        printf("[SETTINGS-QA] live-size %ld window=%ux%u presentation=%dx%d\\n", mp6_tick_count,
            windowSize.native_fb_width, windowSize.native_fb_height, size.x, size.y);
        const auto sceneSize = aurora::rmlui::scene_dimensions();
        printf("[SETTINGS-QA] scene-size %ld %dx%d\\n", mp6_tick_count, sceneSize.x, sceneSize.y);
        mp6::ui::StorybookView *book = nullptr;
        for(int i=0;i<context->GetNumDocuments();++i) {
            Rml::ElementList views;
            context->GetDocument(i)->QuerySelectorAll(views,"storybook-view");
            for(auto *view:views)if(view->IsVisible())book=static_cast<mp6::ui::StorybookView *>(view);
        }
        settings_qa_require(book!=nullptr,"live preview layout is available");
        const auto layout=book->preview_layout(float(sceneSize.x),float(sceneSize.y),"Live game preview");
        const float scale=std::min(size.x/1672.f,size.y/941.f);
        const float gap=(layout.caption.y-layout.mount.y-layout.mount.h)*scale;
        settings_qa_require(gap>=14*scale+1.5f,"live caption clears the entire curved mount");
        settings_qa_require(layout.photo.x>=925.9f&&layout.photo.x+layout.photo.w<=1506.1f&&
            layout.photo.y>=151.9f&&layout.photo.y+layout.photo.h<=426.1f,"live photo stays inside padded paper");
        printf("[SETTINGS-QA] preview-layout %ld mount=%.3f,%.3f,%.3f,%.3f caption=%.3f,%.3f,%.3f,%.3f gap=%.3f\\n",
            mp6_tick_count,layout.mount.x,layout.mount.y,layout.mount.w,layout.mount.h,
            layout.caption.x,layout.caption.y,layout.caption.w,layout.caption.h,gap);
        captureTarget = aurora::webgpu::create_render_texture(size.x, size.y, false);
        auto *renderer = static_cast<aurora::rmlui::WebGPURenderInterface *>(Rml::GetRenderInterface());
        renderer->BeginFrame(captureTarget, aurora::webgpu::present_source(), aurora::rmlui::BaseLayerContent::Transparent);
        context->Render();
        renderer->EndFrame();
    }
    // Keep one second capture on the original viewport, before the first
    // resize at tick 1850.  The game advances normally between tick 1830 and
    // this frame, so the borrowed photo can change while retained geometry is
    // reused.  This probe only renders into an offscreen target and does not
    // mutate simulation or menu state.
    if (mp6_tick_count == 1840) {
        aurora::gx::fifo::drain();
        aurora::gfx::finish();
        auto *context = Rml::GetContext(0);
        const auto size = context->GetDimensions();
        settings_qa_require(size.x == freshnessViewport.x && size.y == freshnessViewport.y,
            "live preview freshness keeps the original viewport");
        mp6::ui::StorybookView *book = nullptr;
        for (int i = 0; i < context->GetNumDocuments(); ++i) {
            Rml::ElementList views;
            context->GetDocument(i)->QuerySelectorAll(views, "storybook-view");
            for (auto *view : views) if (view->IsVisible())
                book = static_cast<mp6::ui::StorybookView *>(view);
        }
        settings_qa_require(book != nullptr, "live preview freshness storybook");
        const auto reuseBefore = book->geometry_reuse_count();
        const auto renderBefore = book->render_call_count();
        freshnessTarget = aurora::webgpu::create_render_texture(size.x, size.y, false);
        auto *renderer = static_cast<aurora::rmlui::WebGPURenderInterface *>(Rml::GetRenderInterface());
        renderer->SetWindowSize(size);
        renderer->BeginFrame(freshnessTarget, aurora::webgpu::present_source(), aurora::rmlui::BaseLayerContent::Transparent);
        context->Render();
        renderer->EndFrame();
        const auto reuseAfter = book->geometry_reuse_count();
        const auto renderAfter = book->render_call_count();
        settings_qa_require(reuseAfter > reuseBefore,
            "live preview freshness reuses retained geometry");
        settings_qa_require(renderAfter >= renderBefore + 1,
            "live preview freshness renders the changed borrowed frame");
        printf("[SETTINGS-QA] live-refresh tick=%ld viewport=%dx%d geometry_reuses=%zu->%zu render_calls=%zu->%zu\\n",
            mp6_tick_count, size.x, size.y, reuseBefore, reuseAfter, renderBefore, renderAfter);
        fflush(stdout);
    }
    if (mp6_tick_count == 1841) {
        settings_qa_capture(freshnessTarget, "live-preview-refresh.ppm");
        freshnessTarget = {};
    }
    if (mp6_tick_count == 1831 || mp6_tick_count == 1921 || mp6_tick_count == 2021 || mp6_tick_count == 2121 || mp6_tick_count == 2221) {
        settings_qa_capture(captureTarget, mp6_tick_count == 1831 ? "live-preview-wide.ppm" :
            mp6_tick_count == 1921 ? "live-preview.ppm" : mp6_tick_count == 2021 ? "live-preview-ultrawide.ppm" :
            mp6_tick_count == 2121 ? "live-preview-720p.ppm" : "live-preview-hidpi.ppm");
        captureTarget = {};
    }
}

/* Pre-shutdown''')
                else:
                    text = text.replace(anchor, f'// QA probe {probe_hash}\n#include "{header}"\n' + anchor + '\n    return mp6_settings_qa_run(sdlWindowPtr);')
                source = str(output / filename)
                Path(source).write_text(text, encoding='utf-8')
                obj = output / 'obj' / name
            elif filename == 'settings.cpp' and touch:
                # Render the actual Android menu branches on the host. No
                # Android build macros leak into SDL/platform headers.
                text = Path(source).read_text(encoding='utf-8')
                text = text.replace('#ifdef __ANDROID__', '#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)')
                text = text.replace('#ifndef __ANDROID__', '#if !defined(__ANDROID__) && !defined(MP6_SETTINGS_TOUCH_QA)')
                model = (ROOT / 'src/android/touch_pad_model.hpp').as_posix()
                text = text.replace('"../../android/touch_pad_model.hpp"', f'"{model}"')
                text = '#include "mp6_touch_pad.h"\n' + text
                source = str(output / filename)
                Path(source).write_text(text, encoding='utf-8')
                flags = [*flags, '-I', str(ROOT / 'src/gx/ui'), '-DMP6_SETTINGS_TOUCH_QA']
                obj = output / 'obj' / name
            if storybook_header is not None:
                source, flags, obj = _rewrite_storybook_source(
                    source, output, storybook_header, storybook_header_hash, flags, obj)
            units.append((source, flags, str(obj), flavor))
        return units

    build.configure_windows, build.collect_units = configure, collect
    sys.argv = ['tools/build.py', '--configuration', 'release', '-j' + os.environ.get('MP6_QA_JOBS', '2')]
    return build.main()


if __name__ == '__main__':
    raise SystemExit(main())
