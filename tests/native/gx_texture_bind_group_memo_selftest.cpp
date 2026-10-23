#include <array>
#include <bitset>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using u32 = uint32_t;
constexpr unsigned MaxTextures = 8;

using WGPUBindGroupLayout = uint64_t;
using WGPUTextureView = uint64_t;
using WGPUSampler = uint64_t;
constexpr size_t WGPU_STRLEN = std::numeric_limits<size_t>::max();

struct WGPUStringView {
  const char* data = nullptr;
  size_t length = 0;
};
struct WGPUBindGroupEntry {
  uint32_t binding = 0;
  WGPUTextureView textureView = 0;
  WGPUSampler sampler = 0;
};
struct WGPUBindGroupDescriptor {
  WGPUStringView label{};
  WGPUBindGroupLayout layout = 0;
  size_t entryCount = 0;
  const WGPUBindGroupEntry* entries = nullptr;
};

namespace wgpu {
template <typename Raw>
struct Handle {
  Raw value = 0;
  Handle() = default;
  explicit Handle(Raw value) : value(value) {}
  Raw Get() const noexcept { return value; }
  explicit operator bool() const noexcept { return value != 0; }
};
using Sampler = Handle<WGPUSampler>;
using TextureView = Handle<WGPUTextureView>;
using BindGroupLayout = Handle<WGPUBindGroupLayout>;
} // namespace wgpu

namespace aurora::gfx {
using BindGroupRef = uint64_t;

struct TextureRef {
  wgpu::TextureView sampleTextureView;
  bool isReplacement = false;
  bool generatedMips = false;
  bool hasArbitraryMips = false;
};
using TextureHandle = std::shared_ptr<TextureRef>;

struct TextureBind {
  TextureHandle ref;
  struct {
    u32 mode0 = 0;
    u32 mode1 = 0;
  } texObj;
  explicit operator bool() const noexcept { return static_cast<bool>(ref); }
};

} // namespace aurora::gfx

namespace aurora::webgpu {
struct GraphicsConfig {
  u32 textureAnisotropy = 1;
};
GraphicsConfig g_graphicsConfig;
} // namespace aurora::webgpu

namespace aurora::gfx {
uint32_t g_frameIndex = 0;
uint32_t bindGroupRefCalls = 0;
uint32_t samplerCalls = 0;
uint32_t samplerCreationCalls = 0;

struct DescriptorSnapshot {
  WGPUBindGroupLayout layout = 0;
  std::array<WGPUTextureView, MaxTextures> textureViews{};
  std::array<WGPUSampler, MaxTextures> samplers{};
  BindGroupRef id = 0;
  uint32_t lastUsedFrame = 0;
  bool operator==(const DescriptorSnapshot&) const = default;
};
std::vector<DescriptorSnapshot> g_bindGroups;
std::vector<DescriptorSnapshot> g_samplerInputs;

uint32_t current_frame() noexcept { return g_frameIndex; }

static uint64_t mix(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
  return hash;
}

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor) {
  ++bindGroupRefCalls;
  assert(descriptor.entryCount == MaxTextures * 2);
  DescriptorSnapshot snapshot;
  snapshot.layout = descriptor.layout;
  uint64_t id = mix(0xcbf29ce484222325ull, descriptor.layout);
  for (unsigned i = 0; i < MaxTextures; ++i) {
    const auto& textureEntry = descriptor.entries[i * 2];
    const auto& samplerEntry = descriptor.entries[i * 2 + 1];
    assert(textureEntry.binding == i * 2);
    assert(samplerEntry.binding == i * 2 + 1);
    snapshot.textureViews[i] = textureEntry.textureView;
    snapshot.samplers[i] = samplerEntry.sampler;
    id = mix(id, textureEntry.textureView);
    id = mix(id, samplerEntry.sampler);
  }
  snapshot.id = id == 0 ? 1 : id;
  snapshot.lastUsedFrame = g_frameIndex;
  for (auto& cached : g_bindGroups) {
    if (cached.id == snapshot.id) {
      assert(cached.layout == snapshot.layout);
      assert(cached.textureViews == snapshot.textureViews);
      assert(cached.samplers == snapshot.samplers);
      cached.lastUsedFrame = g_frameIndex;
      return cached.id;
    }
  }
  g_bindGroups.push_back(snapshot);
  return snapshot.id;
}

void clear_global_bind_groups() { g_bindGroups.clear(); }

void expire_global_bind_groups() {
  for (auto it = g_bindGroups.begin(); it != g_bindGroups.end();) {
    if (g_frameIndex - it->lastUsedFrame > 32) {
      it = g_bindGroups.erase(it);
    } else {
      ++it;
    }
  }
}
} // namespace aurora::gfx

namespace aurora::gx {
using webgpu::g_graphicsConfig;

struct GXBindGroups {
  gfx::BindGroupRef textureBindGroup = 0;
  bool operator==(const GXBindGroups&) const = default;
};
struct ShaderInfo {
  std::bitset<MaxTextures> sampledTextures;
  std::bitset<MaxTextures> sampledIndTextures;
};
struct GXState {
  std::array<gfx::TextureBind, MaxTextures> textures;
} g_gxState;

wgpu::Sampler sEmptySampler;
wgpu::TextureView sEmptyTextureView;
wgpu::BindGroupLayout sTextureBindGroupLayout;

static WGPUSampler texture_sampler(u32 slot, const gfx::TextureBind& texture) {
  ++gfx::samplerCalls;
  uint64_t id = 0x84222325cbf29ce4ull;
  auto add = [&id](uint64_t value) { id = id * 1099511628211ull ^ (value + 0x9e3779b9); };
  add(slot);
  add(texture.texObj.mode0);
  add(texture.texObj.mode1);
  add(g_graphicsConfig.textureAnisotropy);
  add(texture.ref->isReplacement ? 1 : 0);
  add(texture.ref->generatedMips ? 1 : 0);
  add(texture.ref->hasArbitraryMips ? 1 : 0);
  const WGPUSampler sampler = id == 0 ? 1 : id;
  bool known = false;
  for (const auto& input : gfx::g_samplerInputs) {
    if (input.id == sampler) {
      known = true;
      break;
    }
  }
  if (!known) {
    ++gfx::samplerCreationCalls;
    gfx::g_samplerInputs.push_back({.id = sampler});
  }
  return sampler;
}
} // namespace aurora::gx

#define ZoneScoped ((void)0)
namespace aurora::gx {
#include "gx_texture_bind_group_memo_subject.inc"
} // namespace aurora::gx

namespace {
using aurora::gx::GXBindGroups;
using aurora::gx::ShaderInfo;

struct SlotState {
  bool present = false;
  uint64_t view = 0;
  u32 mode0 = 0;
  u32 mode1 = 0;
  bool replacement = false;
  bool generatedMips = false;
  bool arbitraryMips = false;
};

enum class MemoEvent {
  None,
  Clear,
  Initialize,
  Shutdown,
};

struct Transition {
  uint32_t frame = 0;
  uint64_t layout = 0;
  uint8_t sampled = 0;
  uint8_t sampledInd = 0;
  uint32_t anisotropy = 1;
  std::array<SlotState, MaxTextures> slots{};
  MemoEvent event = MemoEvent::None;
};

struct RunResult {
  std::vector<GXBindGroups> refs;
  uint32_t bindCalls = 0;
  uint32_t samplerCalls = 0;
  uint32_t samplerCreations = 0;
  std::vector<aurora::gfx::DescriptorSnapshot> descriptors;
};

void reset_runtime() {
  aurora::gfx::g_frameIndex = 0;
  aurora::gfx::bindGroupRefCalls = 0;
  aurora::gfx::samplerCalls = 0;
  aurora::gfx::samplerCreationCalls = 0;
  aurora::gfx::g_bindGroups.clear();
  aurora::gfx::g_samplerInputs.clear();
  aurora::gx::clear_bind_group_memo();
  aurora::gx::sEmptyTextureView = wgpu::TextureView{9001};
  aurora::gx::sEmptySampler = wgpu::Sampler{9002};
  aurora::gx::sTextureBindGroupLayout = wgpu::BindGroupLayout{77};
}

void apply_transition(const Transition& transition) {
  aurora::gfx::g_frameIndex = transition.frame;
  aurora::webgpu::g_graphicsConfig.textureAnisotropy = transition.anisotropy;
  aurora::gx::sTextureBindGroupLayout = wgpu::BindGroupLayout{transition.layout};
  for (unsigned i = 0; i < MaxTextures; ++i) {
    const auto& source = transition.slots[i];
    if (!source.present) {
      aurora::gx::g_gxState.textures[i] = {};
      continue;
    }
    auto ref = std::make_shared<aurora::gfx::TextureRef>();
    ref->sampleTextureView = wgpu::TextureView{source.view};
    ref->isReplacement = source.replacement;
    ref->generatedMips = source.generatedMips;
    ref->hasArbitraryMips = source.arbitraryMips;
    aurora::gx::g_gxState.textures[i] = {
        .ref = std::move(ref),
        .texObj = {.mode0 = source.mode0, .mode1 = source.mode1},
    };
  }
}

ShaderInfo info_for(const Transition& transition) {
  ShaderInfo info;
  for (unsigned i = 0; i < MaxTextures; ++i) {
    info.sampledTextures[i] = (transition.sampled >> i) & 1;
    info.sampledIndTextures[i] = (transition.sampledInd >> i) & 1;
  }
  return info;
}

GXBindGroups uncached_oracle(const ShaderInfo& info) {
  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    return {};
  }
  std::array<WGPUBindGroupEntry, MaxTextures * 2> entries{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    auto& textureEntry = entries[i * 2];
    auto& samplerEntry = entries[i * 2 + 1];
    textureEntry.binding = i * 2;
    samplerEntry.binding = i * 2 + 1;
    const auto& texture = aurora::gx::g_gxState.textures[i];
    if (texture && (info.sampledTextures[i] || info.sampledIndTextures[i])) {
      textureEntry.textureView = texture.ref->sampleTextureView.Get();
      samplerEntry.sampler = aurora::gx::texture_sampler(i, texture);
    } else {
      textureEntry.textureView = aurora::gx::sEmptyTextureView.Get();
      samplerEntry.sampler = aurora::gx::sEmptySampler.Get();
    }
  }
  const WGPUBindGroupDescriptor descriptor{
      .label = {"GX Texture Bind Group", WGPU_STRLEN},
      .layout = aurora::gx::sTextureBindGroupLayout.Get(),
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  return {.textureBindGroup = aurora::gfx::bind_group_ref(descriptor)};
}

RunResult run_sequence(const std::vector<Transition>& transitions, bool candidate) {
  reset_runtime();
  RunResult result;
  for (const auto& transition : transitions) {
    apply_transition(transition);
    if (transition.event != MemoEvent::None) {
      // The production callers synchronize the worker before this boundary;
      // the fixture models the following memo/cache clear as one operation.
      aurora::gx::clear_bind_group_memo();
      aurora::gfx::clear_global_bind_groups();
    }
    const auto info = info_for(transition);
    result.refs.push_back(candidate ? aurora::gx::build_bind_groups(info) : uncached_oracle(info));
  }
  result.bindCalls = aurora::gfx::bindGroupRefCalls;
  result.samplerCalls = aurora::gfx::samplerCalls;
  result.samplerCreations = aurora::gfx::samplerCreationCalls;
  result.descriptors = aurora::gfx::g_bindGroups;
  return result;
}

Transition initial_transition() {
  Transition transition;
  transition.frame = 7;
  transition.layout = 77;
  transition.sampled = 0x07;
  transition.anisotropy = 4;
  for (unsigned i = 0; i < MaxTextures; ++i) {
    transition.slots[i] = {
        .present = true,
        .view = 100 + i,
        .mode0 = 0x10 + i,
        .mode1 = 0x20 + i,
    };
  }
  return transition;
}

void basic_contract_tests() {
  reset_runtime();
  auto transition = initial_transition();
  apply_transition(transition);
  const auto info = info_for(transition);
  const auto first = aurora::gx::build_bind_groups(info);
  assert(first.textureBindGroup != 0);
  const auto firstCalls = aurora::gfx::bindGroupRefCalls;
  for (unsigned i = 0; i < 1000; ++i) {
    assert(aurora::gx::build_bind_groups(info).textureBindGroup == first.textureBindGroup);
  }
  assert(aurora::gfx::bindGroupRefCalls == firstCalls);

  // A change in an unused slot does not change the materialized key.
  transition.slots[7].view = 900;
  transition.slots[7].mode0 ^= 0x4000;
  apply_transition(transition);
  assert(aurora::gx::build_bind_groups(info).textureBindGroup == first.textureBindGroup);
  assert(aurora::gfx::bindGroupRefCalls == firstCalls);

  // Active view/sampler/layout changes each force a miss.
  transition.slots[0].view = 901;
  apply_transition(transition);
  const auto viewChanged = aurora::gx::build_bind_groups(info);
  assert(viewChanged.textureBindGroup != first.textureBindGroup);
  const auto afterView = aurora::gfx::bindGroupRefCalls;
  transition.slots[0].mode0 ^= 1;
  apply_transition(transition);
  const auto samplerChanged = aurora::gx::build_bind_groups(info);
  assert(samplerChanged.textureBindGroup != viewChanged.textureBindGroup);
  assert(aurora::gfx::bindGroupRefCalls == afterView + 1);
  const auto afterSampler = aurora::gfx::bindGroupRefCalls;
  transition.layout = 78;
  apply_transition(transition);
  assert(aurora::gx::build_bind_groups(info).textureBindGroup != samplerChanged.textureBindGroup);
  assert(aurora::gfx::bindGroupRefCalls == afterSampler + 1);

  // The no-texture early return neither hashes nor returns the previous group.
  const auto beforeEmpty = aurora::gfx::bindGroupRefCalls;
  ShaderInfo empty;
  assert(aurora::gx::build_bind_groups(empty).textureBindGroup == 0);
  assert(aurora::gfx::bindGroupRefCalls == beforeEmpty);

  // A new frame refreshes the global cache entry even with the same key.
  transition.frame++;
  apply_transition(transition);
  const auto nextFrame = aurora::gx::build_bind_groups(info);
  assert(nextFrame.textureBindGroup != 0);
  assert(aurora::gfx::bindGroupRefCalls == beforeEmpty + 1);

  // Explicit clear is required even when the frame number is reused.
  aurora::gx::clear_bind_group_memo();
  aurora::gfx::clear_global_bind_groups();
  const auto afterClear = aurora::gx::build_bind_groups(info);
  assert(afterClear.textureBindGroup == nextFrame.textureBindGroup);
  assert(aurora::gfx::bindGroupRefCalls == beforeEmpty + 2);

  // A backwards frame transition is treated as reset/wrap, not as a hit.
  transition.frame = 1;
  apply_transition(transition);
  const auto afterReset = aurora::gx::build_bind_groups(info);
  assert(afterReset.textureBindGroup == nextFrame.textureBindGroup);
  assert(aurora::gfx::bindGroupRefCalls == beforeEmpty + 3);

  // Exercise the real uint32 frame wrap explicitly.  UINT32_MAX is also the
  // memo's initial frame value, so validity must be carried independently.
  aurora::gx::clear_bind_group_memo();
  aurora::gfx::clear_global_bind_groups();
  transition.frame = UINT32_MAX;
  apply_transition(transition);
  const auto beforeWrap = aurora::gfx::bindGroupRefCalls;
  const auto atMaxFrame = aurora::gx::build_bind_groups(info);
  assert(aurora::gfx::bindGroupRefCalls == beforeWrap + 1);
  transition.frame = 0;
  apply_transition(transition);
  const auto afterWrap = aurora::gx::build_bind_groups(info);
  assert(afterWrap.textureBindGroup == atMaxFrame.textureBindGroup);
  assert(aurora::gfx::bindGroupRefCalls == beforeWrap + 2);

  // A sweep-aged global entry cannot be returned by the recording memo.
  aurora::gx::clear_bind_group_memo();
  aurora::gfx::clear_global_bind_groups();
  transition.frame = 100;
  apply_transition(transition);
  aurora::gx::build_bind_groups(info);
  transition.frame = 140;
  apply_transition(transition);
  aurora::gfx::expire_global_bind_groups();
  const auto afterSweep = aurora::gx::build_bind_groups(info);
  assert(afterSweep.textureBindGroup == nextFrame.textureBindGroup);

  // Reusing a raw view handle after cache clear is an ABA miss, not a stale hit.
  aurora::gx::clear_bind_group_memo();
  aurora::gfx::clear_global_bind_groups();
  transition.frame = 141;
  transition.slots[0].view = 100;
  apply_transition(transition);
  const auto beforeAba = aurora::gfx::bindGroupRefCalls;
  aurora::gx::build_bind_groups(info);
  assert(aurora::gfx::bindGroupRefCalls == beforeAba + 1);
}

std::vector<Transition> randomized_transitions() {
  auto state = initial_transition();
  std::vector<Transition> transitions;
  transitions.reserve(24000);
  for (unsigned i = 0; i < 24000; ++i) {
    if (i == 4096) {
      state.event = MemoEvent::Initialize;
    } else if (i == 8192) {
      state.event = MemoEvent::Shutdown;
    } else if (i == 12288) {
      state.event = MemoEvent::Clear;
    } else {
      state.event = MemoEvent::None;
    }
    if (i > 0 && i % 37 == 0) {
      ++state.frame;
    }
    if (i > 0 && i % 211 == 0) {
      state.frame = 3; // exercise frame reset without relying on equality alone
    }
    if (i % 53 == 0) {
      state.slots[(i / 53) % 3].mode0 ^= 0x100 + i;
    }
    if (i % 71 == 0) {
      state.slots[(i / 71) % 3].view = 100 + (i * 17) % 4096;
    }
    if (i % 89 == 0) {
      state.slots[7].view = 10000 + i; // sampled mask leaves this unused
      state.slots[7].mode1 ^= i;
    }
    if (i % 97 == 0) {
      state.sampled ^= static_cast<uint8_t>(1u << ((i / 97) % 3));
    }
    if (i % 109 == 0) {
      state.sampledInd ^= static_cast<uint8_t>(1u << ((i / 109) % 4));
    }
    if (i % 131 == 0) {
      ++state.layout;
    }
    if (i % 157 == 0) {
      state.anisotropy = 1 + (i / 157) % 8;
    }
    if (i % 181 == 0) {
      state.slots[(i / 181) % MaxTextures].present = !state.slots[(i / 181) % MaxTextures].present;
    }
    // Include explicit empty-info transitions; they must never consult memo.
    if (i % 223 == 0) {
      state.sampled = 0;
      state.sampledInd = 0;
    } else if (i % 223 == 1) {
      state.sampled = 0x07;
      state.sampledInd = 0;
    }
    transitions.push_back(state);
  }
  return transitions;
}

void randomized_contract_and_benchmark() {
  const auto transitions = randomized_transitions();
  const auto candidate = run_sequence(transitions, true);
  const auto oracle = run_sequence(transitions, false);
  assert(candidate.refs == oracle.refs);
  assert(candidate.descriptors == oracle.descriptors);
  assert(candidate.bindCalls < oracle.bindCalls / 3);
  assert(candidate.bindCalls > 0);
  assert(candidate.samplerCalls >= candidate.bindCalls);
  std::printf("GX_MEMO benchmark textured_calls=%zu memo_bind_refs=%u oracle_bind_refs=%u sampler_materializations=%u sampler_creations=%u\n",
              transitions.size(), candidate.bindCalls, oracle.bindCalls,
              candidate.samplerCalls, candidate.samplerCreations);
}

void synthetic_timing_probe() {
  constexpr uint32_t iterations = 100000;
  const auto transition = initial_transition();
  const auto info = info_for(transition);

  reset_runtime();
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  volatile uint64_t hitSink = 0;
  const auto hitStart = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < iterations; ++i) {
    hitSink ^= aurora::gx::build_bind_groups(info).textureBindGroup;
  }
  const auto hitNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - hitStart)
                           .count();

  reset_runtime();
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  volatile uint64_t missSink = 0;
  const auto missStart = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < iterations; ++i) {
    // Advancing the frame forces the production memo miss while preserving
    // the same materialized descriptor key and sampler state.
    aurora::gfx::g_frameIndex = transition.frame + i + 1;
    missSink ^= aurora::gx::build_bind_groups(info).textureBindGroup;
  }
  const auto missNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - missStart)
                            .count();

  std::printf("GX_MEMO timing iterations=%u hit_only_ns=%lld forced_miss_ns=%lld "
              "hit_only_ns_per_call=%.2f forced_miss_ns_per_call=%.2f sinks=%llu/%llu\n",
              iterations, static_cast<long long>(hitNanos),
              static_cast<long long>(missNanos),
              static_cast<double>(hitNanos) / iterations,
              static_cast<double>(missNanos) / iterations,
              static_cast<unsigned long long>(hitSink),
              static_cast<unsigned long long>(missSink));
}
} // namespace

int main() {
  basic_contract_tests();
  randomized_contract_and_benchmark();
  synthetic_timing_probe();
  std::puts("PASS: GX texture bind-group memo exact-key hits, frame refresh, invalidation, sweep/ABA safety, oracle parity");
}
