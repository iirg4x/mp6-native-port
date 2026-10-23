// Diagnostic-only Storybook sky A/B harness.  This file is injected into the
// isolated launcher QA executable by storybook_rigid_qa.py; it is never part of
// the game and does not write the user's configuration.
#pragma once

#include "settings.hpp"
#include "storybook_view.hpp"
#include "lib/gfx/common.hpp"
#include "lib/gfx/render_worker.hpp"
#include "lib/gx/fifo.hpp"
#include "lib/rmlui/WebGPURenderInterface.hpp"
#include "lib/webgpu/gpu.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <RmlUi/Core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" float mp6_rigid_qa_time();
static float mp6_rigid_qa_time_value = 0;
static void mp6_rigid_qa_set_time(float value)
{
    mp6_rigid_qa_time_value = value;
}

namespace mp6::ui {

namespace rigid_qa {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void require(bool condition, std::string_view message)
{
    if (!condition) throw Failure(std::string(message));
}

inline std::filesystem::path output_root()
{
    const char *raw = std::getenv("MP6_RIGID_QA_CAPTURE_ROOT");
    require(raw && *raw, "MP6_RIGID_QA_CAPTURE_ROOT is not set");
    const std::filesystem::path root(raw);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    require(!error, "cannot create rigid sky capture directory");
    return root;
}

inline std::string variant_name()
{
    const char *raw = std::getenv("MP6_RIGID_QA_VARIANT");
    return raw && *raw ? raw : "unknown";
}

inline void capture(const aurora::webgpu::TextureWithSampler &target,
                    const std::filesystem::path &path)
{
    using namespace aurora::webgpu;
    aurora::gfx::render_worker::synchronize();
    const uint32_t width = target.size.width, height = target.size.height;
    const uint32_t pitch = (width * 4 + 255u) & ~255u;
    const uint64_t bytes = uint64_t(pitch) * height;
    const wgpu::BufferDescriptor descriptor{
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
        .size = bytes,
    };
    auto buffer = g_device.CreateBuffer(&descriptor);
    auto encoder = g_device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo source{.texture = target.texture};
    wgpu::TexelCopyBufferInfo destination{
        .layout = {.bytesPerRow = pitch, .rowsPerImage = height},
        .buffer = buffer,
    };
    const wgpu::Extent3D extent{width, height, 1};
    encoder.CopyTextureToBuffer(&source, &destination, &extent);
    const auto commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    bool mapped = false;
    const auto future = buffer.MapAsync(
        wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
            mapped = status == wgpu::MapAsyncStatus::Success;
        });
    g_instance.WaitAny(future, 5000000000ull);
    require(mapped, "rigid sky GPU readback");
    const auto *pixels = static_cast<const unsigned char *>(buffer.GetConstMappedRange(0, bytes));
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    require(!error, "cannot create rigid sky capture parent");
    const auto pathString = path.string();
    FILE *file = fopen(pathString.c_str(), "wb");
    require(file != nullptr, "cannot open rigid sky capture");
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    const bool bgra = target.format == wgpu::TextureFormat::BGRA8Unorm ||
                      target.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto *pixel = pixels + y * pitch + x * 4;
            const unsigned char rgb[]{pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
            fwrite(rgb, 1, 3, file);
        }
    }
    fclose(file);
    buffer.Unmap();
}

inline StorybookView *find_book(Rml::Context *context)
{
    for (int i = 0; i < context->GetNumDocuments(); ++i) {
        Rml::ElementList views;
        context->GetDocument(i)->QuerySelectorAll(views, "storybook-view");
        for (auto *view : views) {
            if (view->IsVisible()) return static_cast<StorybookView *>(view);
        }
    }
    return nullptr;
}

struct Sample {
    int width = 0;
    int height = 0;
    int time = 0;
    size_t skyBuilds = 0;
    size_t skyReuses = 0;
    size_t geometryBuilds = 0;
    size_t geometryReuses = 0;
    size_t renderBatches = 0;
};

inline void write_metrics(const std::filesystem::path &root,
                          const std::vector<Sample> &samples)
{
    const auto path = root / "metrics.json";
    std::ofstream file(path, std::ios::binary);
    require(file.good(), "cannot write rigid sky metrics");
    file << "{\n  \"variant\": \"" << variant_name() << "\",\n  \"samples\": [\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto &sample = samples[i];
        file << "    {\"width\": " << sample.width
             << ", \"height\": " << sample.height
             << ", \"time\": " << sample.time
             << ", \"sky_builds\": " << sample.skyBuilds
             << ", \"sky_reuses\": " << sample.skyReuses
             << ", \"geometry_builds\": " << sample.geometryBuilds
             << ", \"geometry_reuses\": " << sample.geometryReuses
             << ", \"render_batches\": " << sample.renderBatches << "}";
        if (i + 1 != samples.size()) file << ',';
        file << '\n';
    }
    file << "  ]\n}\n";
    require(file.good(), "writing rigid sky metrics failed");
}

inline int run(void *windowPtr)
{
    try {
        using namespace mp6::ui;
        const auto root = output_root();
        const std::array<Rml::Vector2i, 4> sizes{{
            {1280, 720}, {1440, 900}, {2560, 1440}, {1920, 820},
        }};
        const std::array<int, 4> times{{0, 3, 11, 43}};
        const bool expectRigid = variant_name() == "rigid" || variant_name() == "candidate";

        // Motion must remain enabled: that is the production condition that
        // selects the transformed sky path.  This harness changes no persisted
        // setting and exits before the game boot path.
        cfg().menuMotion = 1;
        cfg().vsync = 0;
        auto menu = std::make_unique<SettingsWindow>(false, 0, false);
        push_document(std::move(menu));
        auto *context = Rml::GetContext(0);
        auto *renderer = static_cast<aurora::rmlui::WebGPURenderInterface *>(Rml::GetRenderInterface());
        require(context && renderer, "rigid sky render context unavailable");

        // Let the normal intro finish once.  All captures then share one
        // settled spread; only the diagnostic sky clock changes between them.
        context->SetDimensions({1440, 900});
        context->SetDensityIndependentPixelRatio(1.f);
        mp6::ui::update();
        context->Update();
        SDL_Delay(1700);
        std::vector<Sample> samples;
        samples.reserve(sizes.size() * times.size());
        size_t previousLayoutBuilds = 0;
        bool firstSize = true;

        for (const auto size : sizes) {
            SDL_SetWindowSize(static_cast<SDL_Window *>(windowPtr), size.x, size.y);
            aurora::rmlui::set_ui_scale(1.f);
            context->SetDimensions(size);
            context->SetDensityIndependentPixelRatio(1.f);
            size_t firstSkyBuilds = 0;
            for (size_t timeIndex = 0; timeIndex < times.size(); ++timeIndex) {
                const int time = times[timeIndex];
                // The generated header's hook reads this test-owned value.
                mp6_rigid_qa_set_time(float(time));
                auto target = aurora::webgpu::create_render_texture(size.x, size.y, false);
                // Prime font atlas/layout invalidations before inspecting
                // whether time-only changes reuse the same geometry.
                for(int warm=0;warm<(timeIndex==0?3:1);++warm){
                aurora_update();
                require(aurora_begin_frame(), "rigid sky frame unavailable");
                mp6::ui::update();
                context->SetDimensions(size);
                context->SetDensityIndependentPixelRatio(1.f);
                context->Update();
                renderer->SetWindowSize(size);
                renderer->BeginFrame(target, aurora::webgpu::present_source(),
                                      aurora::rmlui::BaseLayerContent::Transparent);
                context->Render();
                renderer->EndFrame();
                aurora_end_frame();
                }
                auto *book = find_book(context);
                require(book != nullptr, "rigid sky StorybookView is not visible");
                const Sample sample{
                    size.x, size.y, time,
                    book->rigid_qa_sky_build_count(), book->rigid_qa_sky_reuse_count(),
                    book->geometry_build_count(), book->geometry_reuse_count(),
                    book->render_batch_count(),
                };
                if (timeIndex == 0) {
                    firstSkyBuilds = sample.skyBuilds;
                    if (expectRigid && !firstSize) require(sample.skyBuilds > previousLayoutBuilds,
                        "rigid sky did not rebuild after a layout change");
                    previousLayoutBuilds = sample.skyBuilds;
                    firstSize = false;
                } else if (expectRigid) {
                    require(sample.skyBuilds == firstSkyBuilds,
                        "rigid sky rebuilt geometry for a time-only change");
                    require(sample.skyReuses > samples.back().skyReuses,
                        "rigid sky did not reuse transformed geometry");
                }
                const auto name = variant_name() + "-" + std::to_string(size.x) + "x" +
                                  std::to_string(size.y) + "-t" + std::to_string(time) + ".ppm";
                capture(target, root / name);
                printf("[RIGID-QA] sample variant=%s size=%dx%d time=%d sky_builds=%zu sky_reuses=%zu "
                       "geometry_builds=%zu geometry_reuses=%zu render_batches=%zu\n",
                       variant_name().c_str(), size.x, size.y, time, sample.skyBuilds,
                       sample.skyReuses, sample.geometryBuilds, sample.geometryReuses,
                       sample.renderBatches);
                fflush(stdout);
                samples.push_back(sample);
            }
        }
        if (!expectRigid) {
            for (const auto &sample : samples) {
                require(sample.skyBuilds == 0 && sample.skyReuses == 0,
                    "legacy sky unexpectedly used transformed mesh groups");
            }
        } else {
            require(!samples.empty() && samples.front().skyBuilds > 0,
                "rigid sky produced no reusable geometry");
        }
        write_metrics(root, samples);
        printf("[RIGID-QA] COMPLETE variant=%s samples=%zu\n", variant_name().c_str(), samples.size());
        fflush(stdout);
        return 1; // main_native treats this as an intentional isolated exit.
    } catch (const std::exception &error) {
        fprintf(stderr, "[RIGID-QA] FAIL %s\n", error.what());
        fflush(stderr);
        std::exit(9);
    }
}

} // namespace rigid_qa
} // namespace mp6::ui

// Defined in this injected TU and used by the copied StorybookView header.
extern "C" float mp6_rigid_qa_time()
{
    return mp6_rigid_qa_time_value;
}

inline int mp6_rigid_qa_run(void *windowPtr)
{
    return mp6::ui::rigid_qa::run(windowPtr);
}
