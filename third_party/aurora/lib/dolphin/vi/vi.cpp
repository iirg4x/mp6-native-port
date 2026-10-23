#include <dolphin/vi.h>

#include "../../window.hpp"
#include "aurora/math.hpp"
#include "vi_internal.hpp"

#include <optional>

namespace aurora::gx {
/* WS16 (mp6-native port, black-bar/boxing live-resize fix): declared here
 * (rather than pulling in the full gx.hpp, which transitively drags in
 * <webgpu/webgpu_cpp.h> et al. via gfx/common.hpp -- headers the aurora_vi
 * CMake target, unlike aurora_gx, was never configured with an include
 * path for) purely to call it from configure() below. Defined in
 * lib/gx/gx.cpp, which also declares it (identically) in gx.hpp for every
 * OTHER caller in that translation unit's own namespace -- both
 * declarations describe the exact same real symbol, resolved at final-link
 * time exactly like this file's own existing call into
 * aurora::window::request_frame_buffer_resize() (a different library
 * again) already is. */
void rescale_logical_viewport_scissor(Vec2<uint32_t> oldLogicalSize, Vec2<uint32_t> newLogicalSize) noexcept;
} // namespace aurora::gx

namespace aurora::vi {
std::optional<GXRenderModeObj> g_renderMode;

Vec2<uint32_t> render_mode_size() noexcept {
  if (!g_renderMode) {
    return {640, 480};
  }
  return {g_renderMode->fbWidth, g_renderMode->efbHeight};
}

void configure(const GXRenderModeObj* rm) noexcept {
  const auto oldSize = render_mode_size();
  if (rm == nullptr) {
    g_renderMode.reset();
  } else {
    g_renderMode = *rm;
  }
  const auto newSize = render_mode_size();
  if (newSize != oldSize) {
    /* WS16 (mp6-native port, black-bar/boxing live-resize fix): a port
     * feature (dynamic widescreen) can change what this function's OWN
     * render_mode_size() returns on every live window resize, independent
     * of whether the game ever re-issues GXSetViewport/GXSetScissor again
     * -- see lib/gx/gx.cpp's rescale_logical_viewport_scissor() for the
     * full story (confirmed root cause of a live-resize black-bar/boxing
     * bug, not present on real hardware where this size never changes). */
    gx::rescale_logical_viewport_scissor(oldSize, newSize);
    window::request_frame_buffer_resize();
  }
}

Vec2<uint32_t> configured_fb_size() noexcept {
  return render_mode_size();
}
} // namespace aurora::vi

extern "C" {
void VIInit() {}
void VIConfigure(const GXRenderModeObj* rm) { aurora::vi::configure(rm); }
void VIConfigurePan(u16 xOrg, u16 yOrg, u16 width, u16 height) {
  (void)xOrg;
  (void)yOrg;
  (void)width;
  (void)height;
}
u32 VIGetTvFormat() { return 0; }
void VIFlush() {}

void VISetWindowTitle(const char* title) { aurora::window::set_title(title); }
void VISetWindowFullscreen(bool fullscreen) { aurora::window::set_fullscreen(fullscreen); }
bool VIGetWindowFullscreen() { return aurora::window::get_fullscreen(); }
void VISetWindowSize(uint32_t width, uint32_t height) { aurora::window::set_window_size(width, height); }
void VISetWindowPosition(uint32_t x, uint32_t y) { aurora::window::set_window_position(x, y); }
void VICenterWindow() { aurora::window::center_window(); }
void VISetFrameBufferScale(float scale) { aurora::window::set_frame_buffer_scale(scale); }
}
