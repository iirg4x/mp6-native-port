// Included only by the generated QA launcher. Never compiled into the game.
#include "settings.hpp"
#include "prelaunch.hpp"
#include "content_setup.hpp"
#include "modal.hpp"
#include "graphics_tuner.hpp"
#include "menu_bar.hpp"
#include "console.hpp"
#include "storybook_view.hpp"
#include "lib/rmlui/WebGPURenderInterface.hpp"
#include "lib/webgpu/gpu.hpp"
#include "lib/gfx/render_worker.hpp"
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <stdexcept>
#include <set>

extern "C" void mp6_touch_pad_begin_edit(void) {}

namespace {
class PrelaunchProbe : public mp6::ui::Prelaunch {
public:
    Rml::ElementDocument *document() {return mDocument;}
};
class SettingsProbe : public mp6::ui::SettingsWindow {
public:
    explicit SettingsProbe(bool inGame = false) : SettingsWindow(false, 0, inGame) {}
    Rml::ElementDocument *document() { return mDocument; }
    Rml::Element *root() { return mRoot; }
    mp6::ui::StorybookView *book() { return mStorybook; }
};

void settings_qa_require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
    static std::set<std::string> reported;
    if (reported.insert(message).second) printf("[SETTINGS-QA] PASS %s\n", message);
}

void settings_qa_capture(const aurora::webgpu::TextureWithSampler &target, const char *path)
{
    using namespace aurora::webgpu;
    aurora::gfx::render_worker::synchronize();
    const uint32_t width = target.size.width, height = target.size.height;
    const uint32_t pitch = (width * 4 + 255u) & ~255u;
    const uint64_t size = uint64_t(pitch) * height;
    const wgpu::BufferDescriptor bd{.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, .size = size};
    auto buffer = g_device.CreateBuffer(&bd);
    auto encoder = g_device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{.texture = target.texture};
    wgpu::TexelCopyBufferInfo dst{.layout = {.bytesPerRow = pitch, .rowsPerImage = height}, .buffer = buffer};
    wgpu::Extent3D extent{width, height, 1};
    encoder.CopyTextureToBuffer(&src, &dst, &extent);
    auto commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    bool mapped = false;
    const auto future = buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) { mapped = status == wgpu::MapAsyncStatus::Success; });
    g_instance.WaitAny(future, 5000000000ull);
    settings_qa_require(mapped, "GPU readback");
    const auto *pixels = static_cast<const unsigned char *>(buffer.GetConstMappedRange(0, size));
    FILE *file = fopen(path, "wb");
    settings_qa_require(file != nullptr, "capture file");
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    const bool bgra = target.format == wgpu::TextureFormat::BGRA8Unorm || target.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const auto *p = pixels + y * pitch + x * 4;
        const unsigned char rgb[]{p[bgra ? 2 : 0], p[1], p[bgra ? 0 : 2]};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file); buffer.Unmap();
}

// Retained reference harness for the earlier DOM-painted draft. The production
// menu now draws through StorybookView; its actual input/pixel tests are below.
#if defined(MP6_STORYBOOK_PERF_QA)
#include "storybook_perf.hpp"
#elif defined(MP6_LEGACY_SETTINGS_QA)
int mp6_settings_qa_run(void *windowPtr)
{
    try {
        using namespace mp6::ui;
        settings_qa_require(mp6_launcher_parse_config("{\"interface.menu_motion\":false}") != 0 &&
            cfg().menuMotion == 0, "animation preference parses");
        cfg().menuMotion = 1;
        auto menu = std::make_unique<SettingsProbe>();
        auto *probe = menu.get();
        push_document(std::move(menu));
        auto *context = probe->document()->GetContext();
        auto *renderer = static_cast<aurora::rmlui::WebGPURenderInterface *>(Rml::GetRenderInterface());
        auto render = [&](int width, int height, const char *name, float dpi = 1.f) {
            // Override Aurora too, not just RmlUi. Otherwise end_frame restores
            // the host's 125% DPI between captures and shifts the deck's target.
            aurora::rmlui::set_ui_scale(dpi);
            // Match the isolated hidden swapchain to the capture viewport too.
            // Otherwise end_frame temporarily restores 1440px and clamps the
            // horizontal deck's scroll position during a phone capture.
            settings_qa_require(SDL_SetWindowSize(static_cast<SDL_Window *>(windowPtr), width, height), "hidden QA viewport resize");
            auto target = aurora::webgpu::create_render_texture(width, height, false);
            float previousScroll = -1.f;
            int settledFrames = 0;
            for (int i = 0; i < 180; ++i) {
                aurora_update();
                if (!aurora_begin_frame()) throw std::runtime_error("hidden frame unavailable");
                context->SetDimensions({width, height});
                context->SetDensityIndependentPixelRatio(dpi);
                mp6::ui::update();
                context->Update();
                renderer->SetWindowSize({width, height});
                renderer->BeginFrame(target, aurora::webgpu::present_source(), aurora::rmlui::BaseLayerContent::Transparent);
                context->Render();
                renderer->EndFrame();
                aurora_end_frame();
                SDL_Delay(16);
                auto *deck = probe->document()->QuerySelector("pane.settings-list");
                const float scroll = deck ? deck->GetScrollLeft() : 0.f;
                settledFrames = scroll == previousScroll ? settledFrames + 1 : 0;
                previousScroll = scroll;
                // Long controller jumps need more time than adjacent cards.
                // Capture the settled result, bounded to three seconds.
                if (i >= 29 && settledFrames >= 6) break;
            }
            settings_qa_capture(target, name);
            // Aurora also records the ordinary hidden-window UI at end_frame.
            // Restore the captured viewport before asserting its layout/input.
            context->SetDimensions({width, height});
            context->Update();
            Rml::ElementList tabs;
            probe->document()->QuerySelectorAll(tabs, "tab");
            bool tabLabelsFit = true;
            for (auto *tab : tabs) {
                auto *label = tab->QuerySelector("span");
                if (!label) continue;
                const auto labelPos = label->GetAbsoluteOffset();
                const auto tabPos = tab->GetAbsoluteOffset();
                tabLabelsFit &= labelPos.x >= tabPos.x && labelPos.y >= tabPos.y &&
                    labelPos.x + label->GetOffsetWidth() <= tabPos.x + tab->GetOffsetWidth() &&
                    labelPos.y + label->GetOffsetHeight() <= tabPos.y + tab->GetOffsetHeight() - 4.f;
            }
            settings_qa_require(tabLabelsFit, "category labels fit with bottom clearance");
            auto *book = probe->document()->QuerySelector("story-book");
            auto *footer = probe->document()->QuerySelector("settings-footer");
            settings_qa_require(book && footer &&
                footer->GetAbsoluteOffset().y > book->GetAbsoluteOffset().y + book->GetOffsetHeight(),
                "button holder is detached from the book");
            settings_qa_require(probe->document()->QuerySelector("story-book .page.left") &&
                probe->document()->QuerySelector("story-book .page.right") &&
                probe->document()->QuerySelector("story-cast .brighton") &&
                probe->document()->QuerySelector("story-cast .twila"), "independent book and character components");
            if (!probe->root()->IsClassSet("detail-open")) {
                auto *list = probe->document()->QuerySelector("pane.settings-list");
                settings_qa_require(!probe->document()->QuerySelector("pane.settings-detail")->IsVisible(),
                    "choices stay hidden until opened");
                Rml::ElementList rows;
                list->QuerySelectorAll(rows, ".setting-row");
                settings_qa_require(!rows.empty(), "chapter contains settings rows");
                for (size_t i=1; i<rows.size(); ++i)
                    settings_qa_require(rows[i]->GetAbsoluteOffset().y >=
                        rows[i-1]->GetAbsoluteOffset().y + rows[i-1]->GetOffsetHeight() - 1,
                        "settings rows do not overlap");
                if (auto *current = list->QuerySelector(".current-row")) {
                    const auto p=current->GetAbsoluteOffset();
                    settings_qa_require(p.y >= list->GetAbsoluteOffset().y - 1 &&
                        p.y + current->GetOffsetHeight() <= list->GetAbsoluteOffset().y + list->GetClientHeight() + 1,
                        "focused setting is visible");
                }
            }
            Rml::ElementList rows;
            probe->document()->QuerySelectorAll(rows, "select-button");
            for (auto *row : rows) {
                if (!row->IsVisible()) continue;
                auto *value = row->QuerySelector("value");
                if (value) settings_qa_require(value->GetAbsoluteOffset().x + value->GetOffsetWidth() <=
                    row->GetAbsoluteOffset().x + row->GetOffsetWidth(), "value stays inside row");
            }
        };
        auto find = [&](const char *label) -> Rml::Element * {
            Rml::ElementList list;
            probe->document()->QuerySelectorAll(list, "select-button");
            for (auto *row : list) if (auto *key = row->QuerySelector("key"))
                if (key->GetInnerRML() == label) return row;
            throw std::runtime_error(label);
        };
        render(1440, 900, "picture.ppm");
        Rml::ElementList categories;
        probe->document()->QuerySelectorAll(categories, "tab");
        settings_qa_require(categories.size() == 5, "five categories");
        settings_qa_require(probe->root()->GetClientWidth() == 1440, "full-screen width");
        render(1280, 720, "picture-720p.ppm");
        render(2560, 1440, "picture-hidpi.ppm", 2.f);
        probe->set_active_tab(kSettingsTabAudio); probe->focus_content();
        render(1440, 900, "sound.ppm");
        auto *volume = find("Master Volume");
        volume->Focus();
        const int before = cfg().masterVolume;
        context->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
        context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);

        context->ProcessKeyDown(Rml::Input::KI_LEFT, 0);
        context->ProcessKeyUp(Rml::Input::KI_LEFT, 0);
        settings_qa_require(cfg().masterVolume == before - 5, "slider keyboard step");

        Rml::Dictionary change; change["value"] = 65.f;
        volume->QuerySelector("input")->DispatchEvent(Rml::EventId::Change, change);
        settings_qa_require(cfg().masterVolume == 65, "slider change event");
        mp6::ui::update(); context->Update();
        auto *range = volume->QuerySelector("input");
        const auto pos = range->GetAbsoluteOffset();
        // Layout offsets exclude the focused card's animated transform. Hit
        // test the actual track, as a pointer does, instead of assuming y+16.
        const int clickX = int(pos.x + range->GetOffsetWidth() * .4f);
        int clickY = -1;
        for (int y = int(pos.y - 32); y <= int(pos.y + range->GetOffsetHeight()); ++y) {
            auto *hit = context->GetElementAtPoint({float(clickX), float(y)});
            if (hit && hit->Closest("slidertrack") && hit->Closest("input") == range) { clickY = y; break; }
        }
        settings_qa_require(clickY >= 0, "slider track is pointer-accessible");
        context->ProcessMouseMove(clickX, clickY, 0);
        context->ProcessMouseButtonDown(0, 0);
        context->ProcessMouseButtonUp(0, 0);
        settings_qa_require(cfg().masterVolume != 65, "slider pointer changes value");
        auto *effects = find("More Sound Effects"); effects->Focus();
        cfg().sfxVoices = 32;
        context->ProcessKeyDown(Rml::Input::KI_RETURN, 0); context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
        settings_qa_require(cfg().sfxVoices == 16, "choose toggles off");
        context->ProcessKeyDown(Rml::Input::KI_RETURN, 0); context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
        settings_qa_require(cfg().sfxVoices == 32, "choose toggles on");
        context->ProcessKeyDown(Rml::Input::KI_LEFT, 0); context->ProcessKeyUp(Rml::Input::KI_LEFT, 0);
        settings_qa_require(cfg().sfxVoices == 16, "left turns a switch off");
        context->ProcessKeyDown(Rml::Input::KI_RIGHT, 0); context->ProcessKeyUp(Rml::Input::KI_RIGHT, 0);
        settings_qa_require(cfg().sfxVoices == 32, "right turns a switch on");
        volume->Focus(); mp6::ui::update(); context->Update();
        const auto effectsPos = effects->GetAbsoluteOffset();
        const int ex=int(effectsPos.x + effects->GetOffsetWidth()*.4f);
        const int ey=int(effectsPos.y + effects->GetOffsetHeight()*.5f);
        context->ProcessMouseMove(ex, ey, 0);
        context->ProcessMouseButtonDown(0, 0);
        context->ProcessMouseButtonUp(0, 0);
        settings_qa_require(cfg().sfxVoices == 16, "one click activates an unfocused setting");
        settings_qa_require(effects->GetAbsoluteOffset() == effectsPos, "hover does not move the clicked row");
        SDL_Event pad{};
        pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        pad.gbutton.which = 0x7fffffffu; // Synthetic event, no device is opened.
        pad.gbutton.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        mp6::ui::handle_event(pad);
        pad.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
        mp6::ui::handle_event(pad);
        auto *selectedTab = probe->document()->QuerySelector("tab:selected span");
        settings_qa_require(selectedTab && selectedTab->GetInnerRML() == "Controls", "controller shoulder changes category");
        probe->set_active_tab(kSettingsTabControls); probe->focus_content();
        render(1280, 800, "controls-tablet.ppm");
        render(900, 420, "controls-phone-landscape.ppm");
        probe->set_active_tab(kSettingsTabVideo); probe->focus_content();
        render(900, 420, "picture-landscape.ppm");
        auto *ao = find("Ambient Occlusion"); ao->Focus();
        context->ProcessKeyDown(Rml::Input::KI_RETURN, 0); context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
        render(900, 420, "choices-landscape.ppm");
        settings_qa_require(probe->root()->IsClassSet("detail-open"), "landscape choices revealed");
        context->ProcessKeyDown(Rml::Input::KI_ESCAPE, 0); context->ProcessKeyUp(Rml::Input::KI_ESCAPE, 0);
        settings_qa_require(probe->visible() && !probe->root()->IsClassSet("detail-open"), "Back returns from choices");
        render(900, 420, "choices-return-landscape.ppm");
        auto *wide = find("Widescreen"); wide->Focus();
        probe->document()->QuerySelector(".settings-help")->DispatchEvent(Rml::EventId::Click, {});
        render(900, 420, "help-landscape.ppm");
        settings_qa_require(probe->root()->IsClassSet("detail-open"), "phone switch help is reachable");
        context->ProcessKeyDown(Rml::Input::KI_ESCAPE, 0); context->ProcessKeyUp(Rml::Input::KI_ESCAPE, 0);
        settings_qa_require(probe->visible() && !probe->root()->IsClassSet("detail-open"), "Back returns from text-only help");
        probe->set_active_tab(kSettingsTabGame); probe->focus_content();
        render(1440, 900, "game.ppm");
        probe->hide(false);
        settings_qa_require(!probe->document()->IsVisible(), "close immediately releases input");
        probe->show();
        settings_qa_require(probe->document()->IsVisible(), "rapid reopen");
        cfg().menuMotion = 0;
        probe->set_active_tab(kSettingsTabAdvanced); probe->focus_content();
        render(1440, 900, "extras.ppm");
        settings_qa_require(probe->root()->IsClassSet("quiet-motion"), "animation opt-out");
        auto *sun = probe->document()->QuerySelector(".brighton");
        auto *star = probe->document()->QuerySelector("select-button:focus focus-star");
        settings_qa_require(sun && sun->GetProperty("animation")->Get<Rml::AnimationList>().empty(), "sun animation stops");
        settings_qa_require(star && star->GetProperty("animation")->Get<Rml::AnimationList>().empty(), "cursor animation stops");
        // Render the in-game-only cards, but never execute a save/load action.
        probe->hide(false);
        auto inGameMenu = std::make_unique<SettingsProbe>(true);
        probe = inGameMenu.get();
        push_document(std::move(inGameMenu));
        probe->set_active_tab(kSettingsTabGame);
        render(1440, 900, "save-slots.ppm");
        Rml::ElementList slots;
        probe->document()->QuerySelectorAll(slots, "pane.settings-list button");
        int numbered=0;
        std::set<Rml::String> names;
        for (auto *slot : slots) {
            const auto name=slot->GetAttribute<Rml::String>("data-title", "");
            settings_qa_require(names.insert(name).second, "action labels are unambiguous");
            if (name.find("Slot ") != Rml::String::npos) ++numbered;
        }
        settings_qa_require(numbered == 10, "five numbered save/load pairs");
        printf("[SETTINGS-QA] COMPLETE\n"); fflush(stdout);
        return 1;
    } catch (const std::exception &e) {
        fprintf(stderr, "[SETTINGS-QA] FAIL %s\n", e.what()); fflush(stderr);
        std::exit(9);
    }
}
#else
#include "storybook_qa.hpp"
#endif
}
