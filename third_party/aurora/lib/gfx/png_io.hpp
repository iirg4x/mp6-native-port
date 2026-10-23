#pragma once

#include <filesystem>
#include <optional>

#include "common.hpp"
#include "texture_convert.hpp"

namespace aurora::gfx::png {
std::optional<ConvertedTexture> parse_png_bytes(ArrayRef<uint8_t> bytes) noexcept;
std::optional<ConvertedTexture> load_png_file(const std::filesystem::path& path) noexcept;

// Writes `data` (tightly packed 8-bit grayscale, width*height bytes, row-
// major, no row padding) to `path` as an 8-bit grayscale (PNG_COLOR_TYPE_GRAY)
// PNG. Debug/tooling use only (MP6 shadow-map dump, aurora-patches/0016) --
// not on any hot path. Returns false on any failure (bad args, file open,
// libpng error); never throws.
bool write_png_gray8(const std::filesystem::path& path, const uint8_t* data, uint32_t width,
                      uint32_t height) noexcept;
}
