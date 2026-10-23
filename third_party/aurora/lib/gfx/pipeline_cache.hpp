#pragma once

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
struct ShaderInfo;
} // namespace aurora::gx

namespace aurora::rmlui {
struct PipelineConfig;
} // namespace aurora::rmlui

namespace aurora::gfx {

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config,
                          wgpu::RenderPipeline (*create)(const Config&));

PipelineRef find_gx_pipeline(const gx::PipelineConfig& config, gx::ShaderInfo& info);
// As above, but never waits: when the specialized pipeline isn't built yet it
// is queued and the ubershader pipeline for the same fixed-function state is
// returned with uber = true (the draw then uses gx::build_uber_uniform).
PipelineRef find_gx_pipeline(const gx::PipelineConfig& config, gx::ShaderInfo& info, bool& uber);
// Changes whenever a ready pipeline may have been dropped (scene release,
// cache init/shutdown); a caller holding a ready PipelineRef revalidates then.
uint64_t gx_ready_pipeline_generation() noexcept;

bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);

} // namespace aurora::gfx
