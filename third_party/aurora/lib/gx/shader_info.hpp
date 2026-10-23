#pragma once

#include "gx.hpp"

namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
gfx::Range build_uniform(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges) noexcept;
u8 color_channel(GXChannelID id) noexcept;
f32 uber_tex_offset(GXTexOffset offs) noexcept;
u32 uber_point_texcoord_mask() noexcept;
u32 uber_line_texcoord_mask() noexcept;
Vec4<float> uber_texture_size_bias(const gfx::TextureBind& tex) noexcept;
}; // namespace aurora::gx
