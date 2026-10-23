#include "rmlui.hpp"

#ifdef __EMSCRIPTEN__
#include <SDL3/SDL_video.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi_Backend.h>
#include <RmlUi_Platform_SDL.h>
#include <tracy/Tracy.hpp>

#include "window.hpp"
#include "internal.hpp"
#include "imgui.hpp"
#include "rmlui/FileInterface_SDL.h"
#include "rmlui/SystemInterface_Aurora.h"
#include "rmlui/WebGPURenderInterface.hpp"
#include "webgpu/gpu.hpp"

namespace aurora::rmlui {
Rml::Context* g_context = nullptr;
FileInterface_SDL* g_fileInterface = nullptr;

namespace {
Module Log("aurora::rmlui");
constexpr size_t MaxTrackedTouches = 16;

struct TrackedTouch {
  SDL_FingerID id = 0;
  Rml::Vector2f position;
  Rml::Vector2f rmlPosition;
  Rml::Vector2f startPosition;
  Rml::Element* target = nullptr;
  bool active = false;
};

uint32_t s_pressedMouseButtons = 0;
std::array<TrackedTouch, MaxTrackedTouches> s_trackedTouches{};
float s_uiScale = 0.0f;
webgpu::TextureWithSampler s_renderTarget;
wgpu::BindGroup s_renderTargetCopyBindGroup;

WebGPURenderInterface* get_render_interface() noexcept {
  return static_cast<WebGPURenderInterface*>(Backend::GetRenderInterface()); // NOLINT(*-pro-type-static-cast-downcast)
}

Rml::Vector2i dimensions_from_viewport(const gfx::Viewport& viewport) noexcept {
  return {
      std::max(1, static_cast<int>(std::lround(viewport.width))),
      std::max(1, static_cast<int>(std::lround(viewport.height))),
  };
}

bool has_visible_full_window_descendant(const Rml::Element* element) noexcept {
  // Match QuerySelectorAll's DOM-only traversal, but avoid reparsing a selector
  // and allocating a result list on every unlocked presentation. IsVisible(true)
  // rejects hidden ancestors, so whole hidden subtrees can be skipped exactly.
  if (!element->IsVisible()) return false;
  for (int i = 0, count = element->GetNumChildren(); i < count; ++i) {
    const auto* child = element->GetChild(i);
    if (child->GetTagName() == "#text" || !child->IsVisible()) continue;
    if (child->HasAttribute("data-full-window-ui") || has_visible_full_window_descendant(child)) return true;
  }
  return false;
}

bool full_window_ui() noexcept {
  return g_context && has_visible_full_window_descendant(g_context->GetRootElement());
}

bool frame_timings_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("MP6_RML_TIMINGS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

struct FrameTimings {
  using Clock = std::chrono::steady_clock;
  const bool enabled = frame_timings_enabled();
  std::array<Clock::time_point, 7> marks{};
  void mark(size_t index) { if (enabled) marks[index] = Clock::now(); }
  void report() const {
    if (!enabled) return;
    // record_frame is main-thread only. Logging is bounded and entirely opt-in.
    static std::array<double, 5> totals{};
    static unsigned frames = 0;
    static unsigned slow_frames = 0;
    const auto ms = [&](size_t from, size_t to) {
      return std::chrono::duration<double, std::milli>(marks[to] - marks[from]).count();
    };
    const double total = ms(0,6);
    const std::array sample{ms(1,2), ms(3,4), ms(4,5), ms(5,6), total};
    for (size_t i = 0; i < totals.size(); ++i) totals[i] += sample[i];
    if (total > 10.0 && slow_frames < 12) {
      ++slow_frames;
      Log.info("[MP6-RML-SLOW-FRAME] frame={} setup_ms={:.6f} update_ms={:.6f} backdrop_ms={:.6f} begin_ms={:.6f} "
               "render_ms={:.6f} present_end_ms={:.6f} total_ms={:.6f}",
               slow_frames, ms(0,1), ms(1,2), ms(2,3), ms(3,4), ms(4,5), ms(5,6), total);
    }
    if (++frames == 600) {
      Log.info("[MP6-RML-TIMINGS] frames={} update_ms={:.6f} begin_ms={:.6f} render_ms={:.6f} present_end_ms={:.6f} total_ms={:.6f}",
               frames, totals[0]/frames, totals[1]/frames, totals[2]/frames, totals[3]/frames, totals[4]/frames);
      totals = {};
      frames = 0;
    }
  }
};

Rml::Vector2i presentation_dimensions_from_window_size(const AuroraWindowSize& size) noexcept {
  if (full_window_ui()) return {int(size.native_fb_width), int(size.native_fb_height)};
  const auto viewport =
      webgpu::calculate_present_viewport(size.native_fb_width, size.native_fb_height, size.fb_width, size.fb_height);
  return dimensions_from_viewport(viewport);
}

void sync_context_metrics(Rml::Vector2i dimensions) noexcept {
  if (g_context == nullptr || dimensions.x <= 0 || dimensions.y <= 0) {
    return;
  }
  if (g_context->GetDimensions() != dimensions) {
    g_context->SetDimensions(dimensions);
  }
#ifdef __EMSCRIPTEN__
  // In the browser SDL reports the device pixel ratio as pixel density, not
  // display scale; layout in CSS pixels like the rest of the page.
  const float ratio = s_uiScale > 0.0f ? s_uiScale : SDL_GetWindowPixelDensity(window::get_sdl_window());
#else
  const float ratio = s_uiScale > 0.0f ? s_uiScale : window::get_window_size().scale;
#endif
  if (g_context->GetDensityIndependentPixelRatio() != ratio) {
    g_context->SetDensityIndependentPixelRatio(ratio);
  }
}

void ensure_render_target(Rml::Vector2i dimensions) noexcept {
  if (dimensions.x <= 0 || dimensions.y <= 0) {
    return;
  }
  const auto width = static_cast<uint32_t>(dimensions.x);
  const auto height = static_cast<uint32_t>(dimensions.y);
  if (s_renderTarget.view && s_renderTarget.size.width == width && s_renderTarget.size.height == height) {
    return;
  }
  s_renderTarget = webgpu::create_render_texture(width, height, false);
  s_renderTargetCopyBindGroup = webgpu::create_copy_bind_group(s_renderTarget);
}

bool element_has_visible_backdrop_filter(const Rml::Element* element) noexcept {
  if (element == nullptr || !element->IsVisible(true)) {
    return false;
  }

  const auto& computed = element->GetComputedValues();
  if (computed.opacity() > 0.f && computed.has_backdrop_filter()) {
    return true;
  }

  const int childCount = element->GetNumChildren();
  for (int childIndex = 0; childIndex < childCount; ++childIndex) {
    if (element_has_visible_backdrop_filter(element->GetChild(childIndex))) {
      return true;
    }
  }
  return false;
}

bool context_has_visible_backdrop_filter(Rml::Context* context) noexcept {
  if (context == nullptr) {
    return false;
  }
  return element_has_visible_backdrop_filter(context->GetRootElement());
}

struct MappedPoint {
  Rml::Vector2f position;
  bool valid = false;
  bool inside = false;
};

MappedPoint map_native_point_to_content(float nativeX, float nativeY) noexcept {
  if (g_context == nullptr) {
    return {};
  }

  const auto size = window::get_window_size();
  const Rml::Vector2i contentSize = g_context->GetDimensions();
  if (size.native_fb_width == 0 || size.native_fb_height == 0 || contentSize.x <= 0 || contentSize.y <= 0) {
    return {};
  }

  const auto viewport =
      webgpu::calculate_present_viewport(size.native_fb_width, size.native_fb_height,
                                         static_cast<uint32_t>(contentSize.x), static_cast<uint32_t>(contentSize.y));
  const float right = viewport.left + viewport.width;
  const float bottom = viewport.top + viewport.height;
  if (viewport.width <= 0.f || viewport.height <= 0.f) {
    return {};
  }

  return {
      .position =
          {
              (nativeX - viewport.left) * static_cast<float>(contentSize.x) / viewport.width,
              (nativeY - viewport.top) * static_cast<float>(contentSize.y) / viewport.height,
          },
      .valid = true,
      .inside = nativeX >= viewport.left && nativeY >= viewport.top && nativeX < right && nativeY < bottom,
  };
}

MappedPoint map_window_point_to_content(float windowX, float windowY) noexcept {
  const auto size = window::get_window_size();
  if (size.width == 0 || size.height == 0) {
    return {};
  }
  return map_native_point_to_content(
      windowX * static_cast<float>(size.native_fb_width) / static_cast<float>(size.width),
      windowY * static_cast<float>(size.native_fb_height) / static_cast<float>(size.height));
}

MappedPoint map_touch_point_to_content(const SDL_TouchFingerEvent& event) noexcept {
  const auto size = window::get_window_size();
  return map_native_point_to_content(event.x * static_cast<float>(size.native_fb_width),
                                     event.y * static_cast<float>(size.native_fb_height));
}

int rounded_content_coord(float value) noexcept { return static_cast<int>(std::floor(value)); }

bool mouse_button_tracked(uint8_t button) noexcept {
  return button < 32 && (s_pressedMouseButtons & (1u << button)) != 0;
}

void set_mouse_button_tracked(uint8_t button, bool tracked) noexcept {
  if (button >= 32) {
    return;
  }
  const uint32_t mask = 1u << button;
  if (tracked) {
    s_pressedMouseButtons |= mask;
  } else {
    s_pressedMouseButtons &= ~mask;
  }
}

TrackedTouch* find_tracked_touch(SDL_FingerID id) noexcept {
  for (auto& touch : s_trackedTouches) {
    if (touch.active && touch.id == id) {
      return &touch;
    }
  }
  return nullptr;
}

TrackedTouch* find_free_touch() noexcept {
  for (auto& touch : s_trackedTouches) {
    if (!touch.active) {
      return &touch;
    }
  }
  return nullptr;
}

Rml::TouchList touch_list(SDL_FingerID id, Rml::Vector2f position) {
  return {Rml::Touch{static_cast<Rml::TouchId>(id), position}};
}

void dispatch_touch_event(TrackedTouch& touch, const char* type, Rml::Vector2f position, bool inside,
                          Rml::Vector2f delta = {}) noexcept {
  if (touch.target == nullptr) {
    return;
  }

  Rml::Dictionary parameters;
  parameters["finger_id"] = touch.id;
  parameters["x"] = position.x;
  parameters["y"] = position.y;
  parameters["dx"] = delta.x;
  parameters["dy"] = delta.y;
  parameters["start_x"] = touch.startPosition.x;
  parameters["start_y"] = touch.startPosition.y;
  parameters["inside"] = inside;
  touch.target->DispatchEvent(type, parameters, true, true);
}

void handle_mouse_motion(const SDL_MouseMotionEvent& motion) noexcept {
  const MappedPoint mapped = map_window_point_to_content(motion.x, motion.y);
  if (!mapped.inside) {
    g_context->ProcessMouseLeave();
    return;
  }
  g_context->ProcessMouseMove(rounded_content_coord(mapped.position.x), rounded_content_coord(mapped.position.y),
                              RmlSDL::GetKeyModifierState());
}

void handle_mouse_button_down(const SDL_MouseButtonEvent& button) noexcept {
  const MappedPoint mapped = map_window_point_to_content(button.x, button.y);
  if (!mapped.inside) {
    g_context->ProcessMouseLeave();
    return;
  }
  g_context->ProcessMouseMove(rounded_content_coord(mapped.position.x), rounded_content_coord(mapped.position.y),
                              RmlSDL::GetKeyModifierState());
  g_context->ProcessMouseButtonDown(RmlSDL::ConvertMouseButton(button.button), RmlSDL::GetKeyModifierState());
  set_mouse_button_tracked(button.button, true);
  SDL_CaptureMouse(true);
}

void handle_mouse_button_up(const SDL_MouseButtonEvent& button) noexcept {
  if (!mouse_button_tracked(button.button)) {
    return;
  }
  const auto mapped = map_window_point_to_content(button.x, button.y);
  if (mapped.inside) {
    g_context->ProcessMouseMove(rounded_content_coord(mapped.position.x), rounded_content_coord(mapped.position.y),
                                RmlSDL::GetKeyModifierState());
  } else {
    g_context->ProcessMouseLeave();
  }
  g_context->ProcessMouseButtonUp(RmlSDL::ConvertMouseButton(button.button), RmlSDL::GetKeyModifierState());
  set_mouse_button_tracked(button.button, false);
  if (s_pressedMouseButtons == 0) {
    SDL_CaptureMouse(false);
  }
}

void handle_mouse_wheel(const SDL_MouseWheelEvent& wheel) noexcept {
  float mouseX = 0.f;
  float mouseY = 0.f;
  SDL_GetMouseState(&mouseX, &mouseY);
  if (!map_window_point_to_content(mouseX, mouseY).inside) {
    g_context->ProcessMouseLeave();
    return;
  }
  g_context->ProcessMouseWheel(Rml::Vector2f{wheel.x, -wheel.y}, RmlSDL::GetKeyModifierState());
}

void handle_touch_down(const SDL_TouchFingerEvent& finger) noexcept {
  if (find_tracked_touch(finger.fingerID) != nullptr) {
    return;
  }
  const auto mapped = map_touch_point_to_content(finger);
  if (!mapped.inside) {
    return;
  }
  auto* tracked = find_free_touch();
  if (tracked == nullptr) {
    return;
  }
  auto* target = g_context->GetElementAtPoint(mapped.position);
  if (target == nullptr) {
    target = g_context->GetRootElement();
  }
  *tracked = {
      .id = finger.fingerID,
      .position = mapped.position,
      .rmlPosition = mapped.position,
      .startPosition = mapped.position,
      .target = target,
      .active = true,
  };
  dispatch_touch_event(*tracked, TouchStartEvent, mapped.position, true);
  g_context->ProcessTouchStart(touch_list(finger.fingerID, mapped.position), RmlSDL::GetKeyModifierState());
}

void handle_touch_motion(const SDL_TouchFingerEvent& finger) noexcept {
  auto* tracked = find_tracked_touch(finger.fingerID);
  if (tracked == nullptr) {
    return;
  }
  const auto mapped = map_touch_point_to_content(finger);
  if (!mapped.valid) {
    return;
  }
  const Rml::Vector2f delta = mapped.position - tracked->position;
  tracked->position = mapped.position;
  dispatch_touch_event(*tracked, TouchMoveEvent, mapped.position, mapped.inside, delta);
  if (mapped.inside) {
    tracked->rmlPosition = mapped.position;
    g_context->ProcessTouchMove(touch_list(finger.fingerID, mapped.position), RmlSDL::GetKeyModifierState());
  }
}

void handle_touch_up(const SDL_TouchFingerEvent& finger) noexcept {
  auto* tracked = find_tracked_touch(finger.fingerID);
  if (tracked == nullptr) {
    return;
  }
  const auto mapped = map_touch_point_to_content(finger);
  const Rml::Vector2f position = mapped.valid ? mapped.position : tracked->position;
  const Rml::Vector2f delta = position - tracked->position;
  dispatch_touch_event(*tracked, TouchEndEvent, position, mapped.valid && mapped.inside, delta);
  const auto rmlPosition = tracked->rmlPosition;
  *tracked = {};
  g_context->ProcessTouchEnd(touch_list(finger.fingerID, rmlPosition), RmlSDL::GetKeyModifierState());
}

void handle_touch_cancel(const SDL_TouchFingerEvent& finger) noexcept {
  auto* tracked = find_tracked_touch(finger.fingerID);
  if (tracked == nullptr) {
    return;
  }
  dispatch_touch_event(*tracked, TouchCancelEvent, tracked->position, false);
  const auto rmlPosition = tracked->rmlPosition;
  *tracked = {};
  g_context->ProcessTouchCancel(touch_list(finger.fingerID, rmlPosition));
}
} // namespace

void initialize(const AuroraWindowSize& size) noexcept {
  const Rml::Vector2i dim = presentation_dimensions_from_window_size(size);
  if (!Backend::Initialize("Aurora RmlUi Backend", dim.x, dim.y, false)) {
    Log.error("Failed to initialize RmlUI Backend!");
    return;
  }

  g_fileInterface = new FileInterface_SDL();

  auto* renderInterface = get_render_interface();
  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(renderInterface);
  Rml::SetFileInterface(g_fileInterface);

  renderInterface->SetWindowSize(dim);
  renderInterface->SetRenderTargetFormat(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  renderInterface->CreateDeviceObjects();

  Rml::Initialise();
  g_context = Rml::CreateContext("main", dim);

  if (g_context) {
    sync_context_metrics(dim);
  } else {
    Log.error("Failed to initialize RmlUI Context!");
    Rml::Shutdown();
    Backend::Shutdown();
  }
}

Rml::Context* get_context() noexcept { return g_context; }

Rml::Vector2i scene_dimensions() noexcept {
  const auto& source = webgpu::present_source();
  return {int(source.size.width), int(source.size.height)};
}

bool is_initialized() noexcept { return g_context != nullptr; }

void set_ui_scale(float scale) noexcept { s_uiScale = scale > 0.0f ? std::clamp(scale, 0.25f, 4.0f) : 0.0f; }

float get_ui_scale() noexcept { return s_uiScale; }

void set_geometry_transform_palette(std::span<const GeometryTransform> transforms) noexcept {
  if (auto* renderInterface = get_render_interface()) {
    renderInterface->SetGeometryTransformPalette(transforms);
  }
}

void set_input_type(InputType type) noexcept {
  auto* systemInterface = static_cast<SystemInterface_Aurora*>(Backend::GetSystemInterface());
  if (systemInterface != nullptr) {
    systemInterface->SetInputType(type);
  }
}

void handle_event(SDL_Event& event) noexcept {
  if (g_context == nullptr || imgui::wants_capture_event(event)) {
    return;
  }

  switch (event.type) {
  case SDL_EVENT_MOUSE_MOTION:
    handle_mouse_motion(event.motion);
    return;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    handle_mouse_button_down(event.button);
    return;
  case SDL_EVENT_MOUSE_BUTTON_UP:
    handle_mouse_button_up(event.button);
    return;
  case SDL_EVENT_MOUSE_WHEEL:
    handle_mouse_wheel(event.wheel);
    return;
  case SDL_EVENT_FINGER_DOWN:
    handle_touch_down(event.tfinger);
    return;
  case SDL_EVENT_FINGER_MOTION:
    handle_touch_motion(event.tfinger);
    return;
  case SDL_EVENT_FINGER_UP:
    handle_touch_up(event.tfinger);
    return;
  case SDL_EVENT_FINGER_CANCELED:
    handle_touch_cancel(event.tfinger);
    return;
  case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    sync_context_metrics(presentation_dimensions_from_window_size(window::get_window_size()));
    return;
  case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    g_context->ProcessMouseLeave();
    return;
  case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    sync_context_metrics(presentation_dimensions_from_window_size(window::get_window_size()));
    return;
  default:
    break;
  }

  RmlSDL::InputEventHandler(g_context, window::get_sdl_window(), event);
}

// RmlUi keeps every image it has loaded, shown or not: the launcher's
// storybook alone holds four 2048x2048 pages, ~64 MB of the 80 MB of UI
// textures the web build still held in play. Images no frame has drawn for
// IdleImageFrames UI frames are released; RmlUi loads one again if it is
// drawn again (reopening a menu decodes its art once more).
constexpr uint32_t IdleImageFrames = 300;

void release_idle_images(WebGPURenderInterface& renderInterface) noexcept {
  static uint32_t frames = 0;
  if (++frames % 60 != 0) return;
  const auto sources = renderInterface.IdleImageSources(IdleImageFrames);
  for (const auto& source : sources) {
    Rml::ReleaseTexture(source, &renderInterface);
  }
  if (!sources.empty()) {
    Log.info("Released {} UI image(s) no frame has drawn lately", sources.size());
  }
}

RecordedFrame record_frame(const webgpu::Viewport& presentViewport) noexcept {
  if (g_context == nullptr) {
    return {};
  }

  ZoneScoped;
  FrameTimings timings;
  timings.mark(0);
  const bool fullWindow = full_window_ui();
  const Rml::Vector2i dim = fullWindow
      ? Rml::Vector2i{int(webgpu::g_graphicsConfig.surfaceConfiguration.width), int(webgpu::g_graphicsConfig.surfaceConfiguration.height)}
      : dimensions_from_viewport(presentViewport);
  ensure_render_target(dim);
  if (!s_renderTarget.view) {
    return {};
  }

  sync_context_metrics(dim);
  timings.mark(1);
  g_context->Update();
  timings.mark(2);
  // A full-window menu is an independent overlay, not a resized game composite.
  const bool needsBackdrop = !fullWindow && context_has_visible_backdrop_filter(g_context);

  auto* renderInterface = get_render_interface();
  renderInterface->SetWindowSize(g_context->GetDimensions());
  timings.mark(3);
  renderInterface->BeginFrame(s_renderTarget, webgpu::present_source(),
                              needsBackdrop ? BaseLayerContent::Scene : BaseLayerContent::Transparent);

  Backend::BeginFrame();
  timings.mark(4);
  g_context->Render();
  timings.mark(5);
  Backend::PresentFrame();

  const bool rendered = renderInterface->EndFrame();
  release_idle_images(*renderInterface);
  timings.mark(6);
  timings.report();
  if (!rendered) {
    // We didn't render anything
    return {};
  }
  return {
      .bindGroup = s_renderTargetCopyBindGroup,
      .overlay = !needsBackdrop,
      .fullWindow = fullWindow,
  };
}

void shutdown() noexcept {
  if (g_context == nullptr) {
    return;
  }

  Rml::Shutdown();
  Backend::Shutdown();
  g_context = nullptr;
  s_renderTarget = {};
  s_renderTargetCopyBindGroup = {};
}
} // namespace aurora::rmlui
