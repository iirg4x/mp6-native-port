#pragma once

#include "../gfx/common.hpp"
#include "WebGPURenderInterface.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace aurora::rmlui {

constexpr uint32_t RmlPipelineConfigVersion = 4;

enum class PipelineKind : uint32_t {
  Geometry,
  Gradient,
  Blit,
  OpaqueBlit,
  SeedResample,
  SimpleFilter,
  Blur,
  RegionBlit,
  DropShadow,
  MaskImage,
  TransformPalette,
};

enum class VertexLayoutKind : uint32_t {
  Geometry,
  Fullscreen,
  BlurFullscreen,
  TransformPalette,
};

enum class StencilMode : uint32_t {
  None,
  AlwaysKeep,
  EqualKeep,
  ClipReplace,
  ClipIntersect,
};

enum class BlendMode : uint32_t {
  None,
  Premultiplied,
};

enum class DrawKind : uint32_t {
  Geometry,
  Fullscreen,
};

// Immutable geometry storage promoted by the Rml backend after a geometry is
// observed in more than one recording frame. DrawData carries only a raw
// pointer so it remains trivially copyable; gfx pins the owning shared_ptr in
// the current FramePacket before the asynchronous worker can consume it.
struct ResidentGeometry {
  // The CPU copies are immutable after CompileGeometry promotion. They keep
  // worker-side upload retries independent from the Rml geometry handle.
  std::vector<uint8_t> vertexBytes;
  std::vector<uint8_t> indexBytes;
  // These handles are created and assigned only by the render worker. The
  // recording thread may inspect gpuReady, but never touches either handle.
  mutable wgpu::Buffer vertexBuffer;
  mutable wgpu::Buffer indexBuffer;
  uint32_t vertexSize = 0;
  uint32_t indexSize = 0;
  uint64_t deviceGeneration = 0;
  uintptr_t deviceIdentity = 0;
  mutable std::atomic<bool> gpuReady{false};
};

struct PipelineConfig {
  uint32_t version = RmlPipelineConfigVersion;
  uint32_t kind = static_cast<uint32_t>(PipelineKind::Geometry);
  uint32_t colorFormat = static_cast<uint32_t>(wgpu::TextureFormat::Undefined);
  uint32_t sampleCount = 1;
  uint32_t vertexLayout = static_cast<uint32_t>(VertexLayoutKind::Geometry);
  uint32_t stencilFormat = static_cast<uint32_t>(wgpu::TextureFormat::Undefined);
  uint32_t stencilMode = static_cast<uint32_t>(StencilMode::None);
  uint32_t blendMode = static_cast<uint32_t>(BlendMode::Premultiplied);
  uint32_t colorWriteMask = static_cast<uint32_t>(wgpu::ColorWriteMask::All);
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

struct SeedResampleUniformBlock {
  uint32_t samplerMode = 0;
  float frameWidth = 0.f;
  float frameHeight = 0.f;
  uint32_t _pad = 0;
};

struct DrawData {
  gfx::PipelineRef pipeline = 0;
  gfx::Range vertexRange;
  gfx::Range indexRange;
  const ResidentGeometry* residentGeometry = nullptr;
  gfx::Range uniformRange;
  gfx::BindGroupRef bindGroup1 = 0;
  gfx::BindGroupRef bindGroup2 = 0;
  uint32_t bindGroup1DynamicOffset = 0;
  uint32_t bindGroup2DynamicOffset = 0;
  uint32_t dynamicBindGroupMask = 0;
  uint32_t drawKind = static_cast<uint32_t>(DrawKind::Geometry);
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  uint32_t stencilRef = 0;
  std::array<float, 4> blendConstant{};
  uint32_t hasBlendConstant = 0;
};
static_assert(std::is_trivially_copyable_v<DrawData>);

// Worker-side binding state for exactly one WebGPU RenderPassEncoder. The
// recording packet remains immutable and trivially copyable; these handles are
// resolved only while the worker replays that pass. gfx resets the state at
// pass boundaries, before every non-Rml draw, and after custom/failed draws so
// it can never carry bindings across GX or custom state changes.
struct RmlPassBindingState {
  struct Slot {
    gfx::BindGroupRef ref = 0;
    wgpu::BindGroup bindGroup;
    uint32_t dynamicOffset = 0;
    bool dynamic = false;
    bool valid = false;
  };

  gfx::PipelineRef pipeline = 0;
  bool pipelineValid = false;
  uint32_t stencilRef = 0;
  bool stencilRefValid = false;
  std::array<Slot, 3> slots{};

  void invalidate() {
    pipeline = 0;
    pipelineValid = false;
    stencilRef = 0;
    stencilRefValid = false;
    slots = {};
  }
};

void initialize_pipeline();
void shutdown_pipeline();

gfx::BindGroupRef texture_bind_group_ref(const wgpu::TextureView& view);
gfx::BindGroupRef common_bind_group_ref();
gfx::BindGroupRef uniform_bind_group_ref();
gfx::BindGroupRef transform_palette_bind_group_ref();

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, RmlPassBindingState& state);

uint32_t sampler_mode() noexcept;

} // namespace aurora::rmlui
