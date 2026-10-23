#pragma once

#include "gx.hpp"

namespace aurora::gx {
struct PipelineConfig;

// The GX ubershader: one WGSL module per line mode that interprets a draw's
// TEV stages, swap tables, konst selects, indirect stages, alpha compare, fog,
// texgen, lighting and vertex formats from data packed into its uniform block,
// instead of baking them in. A draw whose specialized pipeline isn't built
// yet renders through it on the same frame, with the same arithmetic, and the
// specialized pipeline replaces it once the compile threads finish.
//
// Uniform layout: a fixed superset of what build_uniform writes for any
// config (all matrices, lights, colors, texture sizes), followed by the
// packed config. MaxUniformSize covers it.
constexpr u32 UberConfigWords = 344;
constexpr u32 UberUniformSize = 3856 + UberConfigWords * 4;

std::string build_uber_shader_source(u8 lineMode) noexcept;
// Fixed-function key for an uber pipeline: the config with its shader config
// cleared except for the line mode (which selects the module).
PipelineConfig uber_pipeline_config(const PipelineConfig& config) noexcept;
wgpu::RenderPipeline create_uber_pipeline(const PipelineConfig& config);
gfx::Range build_uber_uniform(const ShaderConfig& config, const ShaderInfo& info, u32 vtxStart,
                              const BindGroupRanges& ranges) noexcept;
void clear_uber_shaders() noexcept;
} // namespace aurora::gx
