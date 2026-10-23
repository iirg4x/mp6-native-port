#include "pipeline.hpp"

#include "../webgpu/gpu.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora::gx {
static Module Log("aurora::gx");

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const auto shader = build_shader(config.shaderConfig);
  const auto label = fmt::format("GX Pipeline {:x} shader {:x}",
                                 xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)),
                                 xxh3_hash(config.shaderConfig));
  return build_pipeline(config, {}, shader, label.c_str());
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, RenderState& state) {
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
    return;
  }

  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, gfx::g_uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup && data.bindGroups.textureBindGroup != state.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
    state.textureBindGroup = data.bindGroups.textureBindGroup;
  }
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    if (!state.indexBufferBound) {
      // The whole buffer: the web's grows (gfx/common.cpp, grow_web_buffer).
      pass.SetIndexBuffer(gfx::g_indexBuffer, wgpu::IndexFormat::Uint16, 0, wgpu::kWholeSize);
      state.indexBufferBound = true;
    }
    pass.DrawIndexed(data.indexCount, data.instanceCount, data.idxRange.offset / sizeof(uint16_t));
  }
}
} // namespace aurora::gx
