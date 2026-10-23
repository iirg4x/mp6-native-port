#pragma once

#ifdef AURORA_ENABLE_RMLUI

#include <RmlUi/Core/Context.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace aurora::rmlui {

enum class InputType {
  Text,
  Number,
};

inline constexpr const char* TouchStartEvent = "touchstart";
inline constexpr const char* TouchMoveEvent = "touchmove";
inline constexpr const char* TouchEndEvent = "touchend";
inline constexpr const char* TouchCancelEvent = "touchcancel";

Rml::Context* get_context() noexcept;
// Dimensions of the current borrowed pre-UI game scene, independent of the UI viewport.
Rml::Vector2i scene_dimensions() noexcept;
bool is_initialized() noexcept;
void set_input_type(InputType type) noexcept;
void set_ui_scale(float scale) noexcept;
float get_ui_scale() noexcept;

struct GeometryTransform {
  std::array<float, 4> basis;
  std::array<float, 4> offset;
  std::array<float, 4> color;
};
inline constexpr size_t GeometryTransformPaletteSize = 256;
void set_geometry_transform_palette(std::span<const GeometryTransform> transforms) noexcept;

struct RuntimeTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  std::span<const std::byte> rgba8;
  bool premultipliedAlpha = false;
};

using TextureProvider = std::function<std::optional<RuntimeTexture>(std::string_view)>;

void register_texture_provider(std::string scheme, TextureProvider provider);
void unregister_texture_provider(std::string_view scheme) noexcept;

} // namespace aurora::rmlui

#endif
