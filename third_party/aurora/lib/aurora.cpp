#include <aurora/aurora.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Hand the finished frame to the browser: it composites the canvas only
// once control returns to the event loop. Waiting for the next animation
// frame doubles as vsync; the timeout keeps hidden tabs (no rAF) moving.
void aurora_web_apply_vsync(bool enabled);
// hidden: a frame that is never presented (aurora_hide_next_frame). It still
// hands the thread to the browser for one event-loop turn (input, audio), but
// never waits for a display refresh it will not use.
EM_ASYNC_JS(void, aurora_web_frame_yield, (int hidden), {
  // Module.auroraFrameStats: CPU milliseconds spent between yields (one
  // game frame of work), for the page's ?fps readout.
  const stats = Module.auroraFrameStats || (Module.auroraFrameStats = { workMs: 0, frames: 0, resumed: 0 });
  const now = performance.now();
  if (stats.resumed) {
    const work = now - stats.resumed;
    stats.workMs += work;
    stats.frames++;
    if (!(stats.maxMs > work)) stats.maxMs = work;
  }
  // The page may sample the frame just submitted (the canvas still holds it
  // until control returns to the browser), e.g. for the phone's reports.
  if (globalThis.mp6FrameHook) globalThis.mp6FrameHook();
  // The display's real refresh interval (not a fixed 16 ms: that halved a
  // 120 Hz screen), measured by a light rAF loop that runs only while
  // uncapped.
  if (Module.mp6Uncapped && !Module.mp6RafProbe) {
    Module.mp6RafProbe = true;
    let last = 0;
    const tick = (ts) => {
      const d = ts - last;
      if (last && d > 2 && d < 50) Module.mp6RafInterval = Module.mp6RafInterval ? Module.mp6RafInterval * 0.9 + d * 0.1 : d;
      last = ts;
      if (Module.mp6Uncapped) requestAnimationFrame(tick); else Module.mp6RafProbe = false;
    };
    requestAnimationFrame(tick);
  }
  if (hidden) {
    await new Promise((resolve) => {
      const ch = Module.mp6YieldChannel || (Module.mp6YieldChannel = new MessageChannel());
      ch.port1.onmessage = () => resolve();
      ch.port2.postMessage(0);
    });
  } else if (Module.mp6Uncapped && now - (Module.mp6LastShown || 0) >= (Module.mp6RafInterval || 16.7) - 1) {
    // Uncapped, but a display refresh is due: Safari only puts the canvas
    // on screen during a rendering update, which a stream of message
    // channel turns never lets happen (the counter read ~500 FPS while the
    // screen held one frame). Wait for it once per refresh.
    await new Promise((resolve) => {
      const timer = setTimeout(resolve, 100);
      requestAnimationFrame(() => { clearTimeout(timer); resolve(); });
    });
    Module.mp6LastShown = performance.now();
  } else if (Module.mp6Uncapped) {
    // Benchmark mode (?uncapped / V-Sync off): don't wait for the display.
    // A message channel turn hands the thread to the browser without
    // setTimeout's clamp; the screen still shows at most its refresh rate.
    await new Promise((resolve) => {
      const ch = Module.mp6YieldChannel || (Module.mp6YieldChannel = new MessageChannel());
      ch.port1.onmessage = () => resolve();
      ch.port2.postMessage(0);
    });
  } else {
    await new Promise((resolve) => {
      const timer = setTimeout(resolve, 100);
      requestAnimationFrame(() => { clearTimeout(timer); resolve(); });
    });
  }
  stats.resumed = performance.now();
  stats.waitMs = (stats.waitMs || 0) + (stats.resumed - now);
});
#endif

#ifdef AURORA_ENABLE_GX
#include "gfx/common.hpp"
#include "gfx/render_worker.hpp"
#include "gx/fifo.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/gpu_prof.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#ifdef AURORA_ENABLE_RMLUI
#include "rmlui.hpp"
#endif

#include "input.hpp"
#include "internal.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <magic_enum.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

#include "system_info.hpp"

#include <thread>
#include "tracy/Tracy.hpp"

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
Module Log("aurora");

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_queue;
using webgpu::g_surface;

uint32_t clamp_scissor_coord(double value, uint32_t maximum) noexcept {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(maximum)));
}

void set_present_viewport(const wgpu::RenderPassEncoder& pass, const gfx::Viewport& viewport, uint32_t surfaceWidth,
                          uint32_t surfaceHeight) noexcept {
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const auto scissorX = clamp_scissor_coord(std::floor(viewport.left), surfaceWidth);
  const auto scissorY = clamp_scissor_coord(std::floor(viewport.top), surfaceHeight);
  const auto scissorRight = clamp_scissor_coord(std::ceil(viewport.left + viewport.width), surfaceWidth);
  const auto scissorBottom = clamp_scissor_coord(std::ceil(viewport.top + viewport.height), surfaceHeight);
  pass.SetScissorRect(scissorX, scissorY, scissorRight - scissorX, scissorBottom - scissorY);
}
#endif

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#if defined(ENABLE_BACKEND_WEBGPU) || defined(__EMSCRIPTEN__)
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D11
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
#ifdef DAWN_ENABLE_BACKEND_OPENGLES
    BACKEND_OPENGLES,
#endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
#ifdef __EMSCRIPTEN__
  ::aurora_web_apply_vsync(config.vsync);
#endif
  Log.info("Aurora initializing");
#ifdef __EMSCRIPTEN__
  log_system_information();
#else
  // Only logging, and on Windows it queries WMI (~20-40 ms): keep it off the
  // path to the first frame.
  std::thread(log_system_information).detach();
#endif
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  AuroraBackend selectedBackend = config.desiredBackend;
  bool windowCreated = false;
  if (selectedBackend != BACKEND_AUTO && window::create_window(selectedBackend)) {
    if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
      windowCreated = true;
    } else {
      window::destroy_window();
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        continue;
      }
      if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
        windowCreated = true;
        break;
      } else {
        window::destroy_window();
      }
    }
  }

  ASSERT(windowCreated, "Error creating window: {}", SDL_GetError());

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();

#ifdef AURORA_ENABLE_GX
  gfx::initialize();

  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize(size);
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

void shutdown() noexcept {
#ifdef AURORA_ENABLE_GX
  gfx::render_worker::synchronize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown();
#endif
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  input::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
  }
  return window::poll_events();
}

// [MP6] Why the last begin_frame() refused, for a host that has to tell a
// minimized window from a GPU that stopped returning staging buffers.
static const char* g_beginFailReason = "";

bool begin_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  {
    if (!window::is_presentable()) {
      webgpu::release_surface();
      g_beginFailReason = "not presentable";
      return false;
    }
    if (window::is_paused()) {
      g_beginFailReason = "window paused (hidden or unfocused)";
      return false;
    }
    if (!g_surface) {
      webgpu::refresh_surface(true);
      if (!g_surface) {
        g_beginFailReason = "no surface";
        return false;
      }
    }
  }

  // WS7: resolve the content-framebuffer fit unconditionally every frame
  // (see window.cpp's own comment on refresh_frame_buffer_fit()) instead of
  // relying solely on whichever discrete resize event happens to be
  // processed -- guarantees a transient RenderMode/aspect desync (e.g. an
  // ordinary VIConfigure() call unrelated to any widescreen/aspect feature)
  // self-corrects within a single frame instead of possibly persisting.
  window::refresh_frame_buffer_fit();

  imgui::new_frame(window::get_window_size());
  if (!gfx::begin_frame()) {
    g_beginFailReason = "no free frame or staging buffer (GPU work not finished)";
    return false;
  }
#endif
  return true;
}

/* Replay-only admission: unlike begin_frame(), gfx::try_begin_frame never
 * waits for frame/staging slots.  Keep ImGui untouched when admission fails. */
bool try_begin_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  {
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }
    if (window::is_paused()) {
      return false;
    }
    if (!g_surface) {
      webgpu::refresh_surface(true);
      if (!g_surface) {
        return false;
      }
    }
  }
  window::refresh_frame_buffer_fit();
  if (!gfx::try_begin_frame()) {
    return false;
  }
  imgui::new_frame(window::get_window_size());
#endif
  return true;
}

static std::atomic<uint64_t> g_endFrameSeq{0};
static std::atomic<bool> g_presentLogOn{false};
static std::mutex g_presentLogMutex;
static std::vector<std::pair<uint64_t, int64_t>> g_presentLog;

// Called by the render worker right after a successful surface present.
static void note_present(uint64_t endSeq) {
#ifndef __EMSCRIPTEN__ // the web never calls a real Present: nothing to time
  if (!g_presentLogOn.load(std::memory_order_relaxed)) return;
  const int64_t nowNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  std::lock_guard lock{g_presentLogMutex};
  if (g_presentLog.size() < 65536) g_presentLog.emplace_back(endSeq, nowNs);
#else
  (void)endSeq;
#endif
}

void end_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  const bool skipPresent = gfx::recording_frame_hidden();
  const uint64_t endSeq = g_endFrameSeq.fetch_add(1, std::memory_order_relaxed) + 1;
  gx::fifo::drain();
  gfx::finish();
  auto imguiDrawData = imgui::freeze();

  const auto& presentSource = webgpu::present_source();
  const auto viewport = webgpu::calculate_present_viewport(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                                           webgpu::g_graphicsConfig.surfaceConfiguration.height,
                                                           presentSource.size.width, presentSource.size.height);

  wgpu::BindGroup rmlBindGroup;
  bool rmlOverlay = false;
  bool rmlFullWindow = false;
#if AURORA_ENABLE_RMLUI
  if (rmlui::is_initialized()) {
    auto rmlFrame = rmlui::record_frame(viewport);
    rmlBindGroup = std::move(rmlFrame.bindGroup);
    rmlOverlay = rmlFrame.overlay;
    rmlFullWindow = rmlFrame.fullWindow;
  }
#endif

  gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, rmlFullWindow, viewport, skipPresent, endSeq,
                  imguiDrawData = std::move(imguiDrawData)](wgpu::CommandEncoder& encoder) {
    wgpu::Texture currentTexture;
    wgpu::TextureView currentView;
    auto surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::Error;
    {
      window::SurfaceLock surfaceLock;
      if (!skipPresent && window::is_presentable() && g_surface) {
        ZoneScopedN("Acquire texture");
        wgpu::SurfaceTexture surfaceTexture;
        g_surface.GetCurrentTexture(&surfaceTexture);
        surfaceStatus = surfaceTexture.status;
        if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
            surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
          currentTexture = std::move(surfaceTexture.texture);
          currentView = currentTexture.CreateView();
        }
      }
    }

    const bool canPresent = currentTexture && currentView;
    if (canPresent) {
      wgpu::BindGroup presentBindGroup;
      const bool directResample = (!rmlBindGroup || rmlOverlay) && webgpu::can_resample_present_direct(viewport);
      if (rmlBindGroup && !rmlOverlay) {
        presentBindGroup = rmlBindGroup;
      } else if (!directResample) {
        const auto& resampledSource = webgpu::resample_present_source(encoder, viewport);
        presentBindGroup = webgpu::create_copy_bind_group(resampledSource);
      }
      {
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = currentView,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "EFB copy render pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
            .timestampWrites = webgpu::gpu_prof::pass_writes("Present + touch UI"),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        // Copy EFB -> XFB (swapchain)
        set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                             webgpu::g_graphicsConfig.surfaceConfiguration.height);
        if (directResample) {
          // Same resampling/FXAA shader, directly into the surface. Avoid a
          // full-size intermediate store/load before the UI is composited.
          webgpu::draw_resampled_present(pass, viewport);
        } else {
          pass.SetPipeline(webgpu::g_CopyPipeline);
          pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
          pass.Draw(3);
        }
        if (rmlBindGroup && rmlOverlay) {
          if (rmlFullWindow) {
            pass.SetViewport(0.f, 0.f, float(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                             float(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
            pass.SetScissorRect(0, 0, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                webgpu::g_graphicsConfig.surfaceConfiguration.height);
          }
          pass.SetPipeline(webgpu::g_CopyPremultipliedAlphaPipeline);
          pass.SetBindGroup(0, rmlBindGroup, 0, nullptr);
          pass.Draw(3);
        }
        // Same target and ordering: draw touch UI before storing the surface.
        // A separate load/store pass needlessly spills mobile tile memory.
        pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
        imgui::render(pass, imguiDrawData);
        pass.End();
      }
    } else if (!skipPresent) {
      Log.info("Skipping present; window not presentable");
    }
    webgpu::gpu_prof::frame_end(encoder);
    const wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "Redraw command buffer"};
    const auto buffer = encoder.Finish(&cmdBufDescriptor);
    {
      ZoneScopedN("Queue Submit");
      g_queue.Submit(1, &buffer);
    }
    webgpu::gpu_prof::after_submit();
#ifdef __EMSCRIPTEN__
    aurora_web_frame_yield(skipPresent ? 1 : 0);
#endif
    if (canPresent && g_surface) {
      ZoneScopedN("Present");
      wgpu::ConvertibleStatus status = wgpu::Status::Error;
      {
        window::SurfaceLock surfaceLock;
        if (window::is_presentable()) {
#ifdef __EMSCRIPTEN__
          // The browser presents the canvas when control returns to the
          // event loop; wgpuSurfacePresent aborts under emdawnwebgpu.
          status = wgpu::Status::Success;
#else
          status = g_surface.Present();
#endif
        }
      }
      if (status) {
        note_present(endSeq);
        gfx::after_present();
        if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
          // Present the usable image before refreshing the surface.
          window::push_custom_event(window::CustomEvent::RefreshSurface);
        }
      } else {
        Log.warn("Surface present failed");
        webgpu::release_surface();
      }
    } else if (g_surface && !skipPresent) {
      switch (surfaceStatus) {
      case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
        Log.warn("Surface texture acquisition timed out");
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
      case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
        Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceStatus));
        window::push_custom_event(window::CustomEvent::RefreshSurface);
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Lost:
        Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
        webgpu::release_surface();
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Error:
        Log.warn("Surface texture is {}, dropping surface", magic_enum::enum_name(surfaceStatus));
        g_surface = {};
        break;
      default:
        if (!window::is_presentable()) {
          webgpu::release_surface();
        } else {
          Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceStatus));
        }
        break;
      }
    }
    gfx::after_submit();

#ifdef TRACY_ENABLE
    const AuroraStats* frameStats = aurora_get_stats();
    TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

    TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(frameStats->queuedPipelines));
    TracyPlot("aurora: createdPipelines", static_cast<int64_t>(frameStats->createdPipelines));
    TracyPlot("aurora: drawCallCount", static_cast<int64_t>(frameStats->drawCallCount));
    TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(frameStats->mergedDrawCallCount));
    TracyPlot("aurora: lastVertSize", static_cast<int64_t>(frameStats->lastVertSize));
    TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(frameStats->lastUniformSize));
    TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(frameStats->lastIndexSize));
    TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(frameStats->lastStorageSize));
    TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(frameStats->lastTextureUploadSize));
#endif
  });

#endif
}
} // namespace
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
const char* aurora_begin_frame_fail_reason() { return aurora::g_beginFailReason; }
bool aurora_try_begin_frame() { return aurora::try_begin_frame(); }
int aurora_wait_replay_ready(uint64_t maxWaitNs) {
#ifdef AURORA_ENABLE_GX
  if (!aurora::window::is_presentable() || aurora::window::is_paused()) return -1;
  // A missing surface is recovered by the existing try_begin_frame path.
  return aurora::gfx::wait_for_replay_resources(std::chrono::nanoseconds{
      static_cast<int64_t>(std::min<uint64_t>(maxWaitNs, INT64_MAX))}) ? 1 : 0;
#else
  (void)maxWaitNs;
  return 1;
#endif
}
void aurora_end_frame() { aurora::end_frame(); }
uint64_t aurora_end_frame_seq(void) { return aurora::g_endFrameSeq.load(std::memory_order_relaxed); }
int aurora_present_log_drain(uint64_t* seqs, int64_t* presentNs, int max) {
  aurora::g_presentLogOn.store(true, std::memory_order_relaxed);
  std::lock_guard lock{aurora::g_presentLogMutex};
  int n = 0;
  for (; n < max && n < static_cast<int>(aurora::g_presentLog.size()); ++n) {
    seqs[n] = aurora::g_presentLog[n].first;
    presentNs[n] = aurora::g_presentLog[n].second;
  }
  aurora::g_presentLog.erase(aurora::g_presentLog.begin(), aurora::g_presentLog.begin() + n);
  return n;
}
void aurora_hide_next_frame(void) {
#ifdef AURORA_ENABLE_GX
  aurora::gfx::hide_next_frame();
#endif
}
int aurora_frame_hidden(void) {
#ifdef AURORA_ENABLE_GX
  return aurora::gfx::recording_frame_hidden() ? 1 : 0;
#else
  return 0;
#endif
}
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_render_scale(float scale) {
#ifdef AURORA_ENABLE_GX
  const float before = aurora::webgpu::render_scale();
  aurora::webgpu::set_render_scale(scale);
  if (aurora::webgpu::render_scale() != before) {
    const auto size = aurora::window::get_window_size();
    aurora::webgpu::resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height);
  }
#else
  (void)scale;
#endif
}
void aurora_set_resampler(AuroraSampler sampler) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_resampler(sampler);
#else
  (void)sampler;
#endif
}
/* Anti-Aliasing P2 (aurora-patches/0017): clones aurora_set_resampler exactly
 * (impl above) -- FXAA is a post-process, so this applies live. */
void aurora_set_post_aa(AuroraPostAA mode) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_post_aa(mode);
#else
  (void)mode;
#endif
}
