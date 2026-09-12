#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <dolphin/gx/GXEnum.h>

namespace wgpu {
enum class IndexFormat { Uint16, Uint32 };
struct Color { double r, g, b, a; };
struct Draw {
  bool indexed;
  uint32_t count, instances, uniform, pipeline;
  uint64_t texture, indexByteOffset;
  double alpha;
  bool operator==(const Draw&) const = default;
};
struct RenderPassEncoder {
  mutable uint64_t texture = 0, offset = 0, size = 0;
  mutable uint32_t uniform = 0, pipeline = 0;
  mutable IndexFormat format = IndexFormat::Uint16;
  mutable double alpha = 0;
  mutable uint32_t textureCalls = 0, indexCalls = 0, uniformCalls = 0;
  mutable std::vector<Draw> draws;
  void SetBindGroup(uint32_t slot, uint64_t group, size_t count = 0, const uint32_t* offsets = nullptr) const {
    if (slot == 1) {
      assert(group == 13 && count == 1);
      uniform = offsets[0];
      ++uniformCalls;
    } else {
      assert(slot == 2);
      texture = group;
      ++textureCalls;
    }
  }
  void SetIndexBuffer(uint32_t buffer, IndexFormat fmt, uint64_t start, uint64_t length) const {
    assert(buffer == 17 && start + length <= 1048576);
    format = fmt; offset = start; size = length; ++indexCalls;
  }
  void SetBlendConstant(const Color* color) const { alpha = color->a; }
  void Draw(uint32_t count, uint32_t instances) const {
    draws.push_back({false, count, instances, uniform, pipeline, texture, 0, alpha});
  }
  void DrawIndexed(uint32_t count, uint32_t instances, uint32_t firstIndex = 0) const {
    assert(format == IndexFormat::Uint16);
    assert(uint64_t(firstIndex + count) * 2 <= size);
    draws.push_back({true, count, instances, uniform, pipeline, texture, offset + firstIndex * 2, alpha});
  }
};
}

namespace aurora::gfx {
using BindGroupRef = uint64_t;
using PipelineRef = uint32_t;
struct Range { uint32_t offset, size; };
constexpr uint64_t IndexBufferSize = 1048576;
constexpr uint32_t g_indexBuffer = 17;
constexpr uint64_t g_uniformBindGroup = 13;
bool bind_pipeline(PipelineRef pipeline, const wgpu::RenderPassEncoder& pass) {
  if (!pipeline) return false;
  pass.pipeline = pipeline;
  return true;
}
uint64_t find_bind_group(uint64_t id) { assert(id); return id; }
}
namespace aurora::gx {
struct GXBindGroups { gfx::BindGroupRef textureBindGroup; };
}
#include "gx_bindings_subject.inc"

int main() {
  using namespace aurora;
  wgpu::RenderPassEncoder pass;
  gx::RenderState state{};
  std::vector<wgpu::Draw> expected;
  uint32_t seed = 5489, successful = 0, oldTextureCalls = 0;
  uint64_t texture = 0;
  double alpha = 0;
  auto random = [&]() { seed = seed * 1664525 + 1013904223; return seed; };
  for (uint32_t i = 0; i < 200000; ++i) {
    if (i % 257 == 0) { // New encoder/pass: nothing may carry over.
      assert(pass.draws == expected);
      expected.clear(); pass.draws.clear();
      state = {};
      pass.texture = texture = 0; pass.alpha = alpha = 0;
      pass.offset = 0; pass.size = 0;
    } else if (i % 89 == 0 || i % 173 == 0) { // Rml/custom overwrite GX state.
      pass.texture = texture = 987;
      pass.format = wgpu::IndexFormat::Uint32;
      pass.offset = 128; pass.size = 128;
      state = {};
    } else if (i % 113 == 0) { // Clear changes blend state, not the texture.
      pass.alpha = alpha = 1;
      state = {};
    }
    gx::DrawData draw{};
    draw.pipeline = (i % 97 == 0) ? 0 : 1 + random() % 13;
    draw.vtxCount = 1 + random() % 65535;
    draw.indexCount = (i % 7 == 0) ? 0 : 3 + random() % 1024;
    draw.instanceCount = 1 + random() % 32; // Includes line/point instancing.
    draw.uniformRange = {random() & 0x3ff00, 256};
    draw.idxRange.size = draw.indexCount * 2;
    draw.idxRange.offset = (random() % (1048576 - draw.idxRange.size)) & ~3U;
    if (i % 211 == 0) { // Last valid two bytes of the buffer.
      draw.indexCount = 1; draw.idxRange = {1048574, 2};
    }
    draw.bindGroups.textureBindGroup = (i % 11 == 0) ? 0 : 1 + (i / 31) % 64;
    draw.dstAlpha = (i % 3 == 0) ? random() % 256 : UINT32_MAX;
    if (draw.pipeline) {
      ++successful;
      if (draw.bindGroups.textureBindGroup) {
        texture = draw.bindGroups.textureBindGroup;
        ++oldTextureCalls;
      }
      if (draw.dstAlpha != UINT32_MAX) alpha = draw.dstAlpha / 255.f;
      expected.push_back({draw.indexCount != 0,
        draw.indexCount ? draw.indexCount : draw.vtxCount, draw.instanceCount,
        draw.uniformRange.offset, draw.pipeline, texture,
        draw.indexCount ? draw.idxRange.offset : 0, alpha});
    }
    gx::render(draw, pass, state);
    assert(pass.draws.size() == expected.size());
    if (!expected.empty()) assert(pass.draws.back() == expected.back());
  }
  assert(pass.draws == expected);
  assert(pass.uniformCalls == successful);
  assert(pass.indexCalls < successful / 10);
  assert(pass.textureCalls < oldTextureCalls / 10);
  std::printf("200000 draws verified; index binds %u -> %u; texture binds %u -> %u\n",
    successful, pass.indexCalls, oldTextureCalls, pass.textureCalls);
}
