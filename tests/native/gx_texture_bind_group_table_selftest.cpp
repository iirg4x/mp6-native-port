#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <type_traits>
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

namespace aurora::gx {
void clear_bind_group_memo() noexcept;
} // namespace aurora::gx

static uint64_t hashOverride = std::numeric_limits<uint64_t>::max();
static uint32_t hashCalls = 0;

namespace aurora {
using HashType = uint64_t;
template <typename Key>
static HashType xxh3_hash(const Key& key) {
  ++hashCalls;
  if (hashOverride != std::numeric_limits<uint64_t>::max()) return hashOverride;
  // The host stub maps layouts to slots; collision tests set hashOverride to
  // one value. Production uses Aurora's full xxh3 hash on the proven key.
  return static_cast<HashType>(key.layout);
}
} // namespace aurora

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

uint32_t g_frameIndex = 0;
uint32_t bindGroupRefCalls = 0;
uint32_t samplerCalls = 0;

struct DescriptorSnapshot {
  WGPUBindGroupLayout layout = 0;
  std::array<WGPUTextureView, MaxTextures> textureViews{};
  std::array<WGPUSampler, MaxTextures> samplers{};
  BindGroupRef id = 0;
  uint32_t lastUsedFrame = 0;
  bool operator==(const DescriptorSnapshot&) const = default;
};
std::vector<DescriptorSnapshot> g_bindGroups;

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

void clear_global_bind_groups() {
  g_bindGroups.clear();
  // Aurora's global cache clear also invalidates the GX memo through
  // gfx::clear_caches(); keep the harness lifecycle ordering explicit.
  aurora::gx::clear_bind_group_memo();
}
} // namespace aurora::gfx

namespace aurora::webgpu {
struct GraphicsConfig {
  u32 textureAnisotropy = 1;
};
GraphicsConfig g_graphicsConfig;
} // namespace aurora::webgpu

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
  id = id * 1099511628211ull ^ (slot + 0x9e3779b9);
  id = id * 1099511628211ull ^ (texture.texObj.mode0 + 0x9e3779b9);
  id = id * 1099511628211ull ^ (texture.texObj.mode1 + 0x9e3779b9);
  id = id * 1099511628211ull ^ (g_graphicsConfig.textureAnisotropy + 0x9e3779b9);
  id = id * 1099511628211ull ^ (texture.ref->isReplacement ? 1 : 0);
  id = id * 1099511628211ull ^ (texture.ref->generatedMips ? 1 : 0);
  id = id * 1099511628211ull ^ (texture.ref->hasArbitraryMips ? 1 : 0);
  return id == 0 ? 1 : id;
}
} // namespace aurora::gx

#define ZoneScoped ((void)0)
namespace aurora::gx {
#include "gx_texture_bind_group_table_subject.inc"
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

struct Transition {
  uint32_t frame = 0;
  uint64_t layout = 0;
  uint8_t sampled = 0;
  uint8_t sampledInd = 0;
  uint32_t anisotropy = 1;
  std::array<SlotState, MaxTextures> slots{};
};

struct RunResult {
  std::vector<GXBindGroups> refs;
  uint32_t bindCalls = 0;
  std::vector<aurora::gfx::DescriptorSnapshot> descriptors;
};

void reset_runtime() {
  aurora::gfx::g_frameIndex = 0;
  aurora::gfx::bindGroupRefCalls = 0;
  aurora::gfx::samplerCalls = 0;
  aurora::gfx::g_bindGroups.clear();
  aurora::gx::clear_bind_group_memo();
  aurora::gx::sEmptyTextureView = wgpu::TextureView{9001};
  aurora::gx::sEmptySampler = wgpu::Sampler{9002};
  aurora::gx::sTextureBindGroupLayout = wgpu::BindGroupLayout{77};
  hashOverride = std::numeric_limits<uint64_t>::max();
  hashCalls = 0;
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

aurora::gx::TextureBindGroupMemoKey key_for(const ShaderInfo& info) {
  aurora::gx::TextureBindGroupMemoKey key{};
  key.layout = aurora::gx::sTextureBindGroupLayout.Get();
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& texture = aurora::gx::g_gxState.textures[i];
    if (texture && (info.sampledTextures[i] || info.sampledIndTextures[i])) {
      key.textureViews[i] = texture.ref->sampleTextureView.Get();
      key.samplers[i] = aurora::gx::texture_sampler(i, texture);
    } else {
      key.textureViews[i] = aurora::gx::sEmptyTextureView.Get();
      key.samplers[i] = aurora::gx::sEmptySampler.Get();
    }
  }
  return key;
}

GXBindGroups bind_from_key(const aurora::gx::TextureBindGroupMemoKey& key) {
  std::array<WGPUBindGroupEntry, MaxTextures * 2> entries{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    entries[i * 2] = {.binding = i * 2, .textureView = key.textureViews[i]};
    entries[i * 2 + 1] = {.binding = i * 2 + 1, .sampler = key.samplers[i]};
  }
  const WGPUBindGroupDescriptor descriptor{
      .label = {"GX Texture Bind Group", WGPU_STRLEN},
      .layout = key.layout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  return {.textureBindGroup = aurora::gfx::bind_group_ref(descriptor)};
}

GXBindGroups uncached_oracle(const ShaderInfo& info) {
  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) return {};
  return bind_from_key(key_for(info));
}

static aurora::gx::TextureBindGroupMemo referenceMemo;
GXBindGroups reference_0076(const ShaderInfo& info) {
  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) return {};
  const auto key = key_for(info);
  const auto frame = aurora::gfx::current_frame();
  if (referenceMemo.valid && frame < referenceMemo.frame) referenceMemo = {};
  if (referenceMemo.valid && referenceMemo.frame == frame && referenceMemo.key == key) {
    return referenceMemo.bindGroups;
  }
  const auto bindGroups = bind_from_key(key);
  referenceMemo = {.key = key, .bindGroups = bindGroups, .frame = frame, .valid = true};
  return bindGroups;
}

RunResult run_sequence(const std::vector<Transition>& transitions, unsigned mode,
                       uint64_t forcedHash = std::numeric_limits<uint64_t>::max()) {
  reset_runtime();
  hashOverride = forcedHash;
  RunResult result;
  for (const auto& transition : transitions) {
    apply_transition(transition);
    const auto info = info_for(transition);
    if (mode == 0) {
      result.refs.push_back(aurora::gx::build_bind_groups(info));
    } else if (mode == 1) {
      result.refs.push_back(reference_0076(info));
    } else {
      result.refs.push_back(uncached_oracle(info));
    }
  }
  result.bindCalls = aurora::gfx::bindGroupRefCalls;
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

void table_bound_and_last_key_fastpath() {
  static_assert(aurora::gx::TextureBindGroupMemoTableSize == 64);
  static_assert(sizeof(aurora::gx::sTextureBindGroupMemoTable) / sizeof(aurora::gx::sTextureBindGroupMemoTable[0]) == 64);
  reset_runtime();
  auto transition = initial_transition();
  apply_transition(transition);
  const auto info = info_for(transition);
  const auto first = aurora::gx::build_bind_groups(info);
  const auto firstHashCalls = hashCalls;
  const auto firstBindCalls = aurora::gfx::bindGroupRefCalls;
  for (unsigned i = 0; i < 1000; ++i) {
    assert(aurora::gx::build_bind_groups(info).textureBindGroup == first.textureBindGroup);
  }
  // The retained last-key path returns before the table hash.
  assert(hashCalls == firstHashCalls && aurora::gfx::bindGroupRefCalls == firstBindCalls);
}

void alternating_keys_and_forced_collisions() {
  reset_runtime();
  auto first = initial_transition();
  auto second = first;
  second.layout = 78;
  second.slots[0].view = 500;
  const auto candidate = run_sequence({first, second, first, second}, 0);
  const auto reference = run_sequence({first, second, first, second}, 1);
  const auto oracle = run_sequence({first, second, first, second}, 2);
  assert(candidate.refs == reference.refs && candidate.refs == oracle.refs);
  assert(candidate.bindCalls == 2);
  assert(reference.bindCalls == 4 && oracle.bindCalls == 4);

  const auto collisionCandidate = run_sequence({first, second, first}, 0, 17);
  const auto collisionOracle = run_sequence({first, second, first}, 2);
  assert(collisionCandidate.refs == collisionOracle.refs);
  assert(collisionCandidate.bindCalls == 3); // full-key compare rejects the collision.
}

void expect_new_bind(const Transition& transition, uint32_t& calls) {
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info_for(transition));
  assert(aurora::gfx::bindGroupRefCalls == ++calls);
}

void each_key_component_and_empty_slots() {
  reset_runtime();
  auto transition = initial_transition();
  transition.sampled = 0xff;
  transition.sampledInd = 0;
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info_for(transition));
  auto calls = aurora::gfx::bindGroupRefCalls;

  // Every sampled slot contributes both its view and sampler-derived state to
  // the key. Exercise all eight slots, including all sampler metadata flags.
  for (unsigned slot = 0; slot < MaxTextures; ++slot) {
    transition.slots[slot].view += 900;
    expect_new_bind(transition, calls);

    transition.slots[slot].mode0 ^= 0x101 + slot;
    expect_new_bind(transition, calls);

    transition.slots[slot].replacement = true;
    expect_new_bind(transition, calls);

    transition.slots[slot].generatedMips = true;
    expect_new_bind(transition, calls);

    transition.slots[slot].arbitraryMips = true;
    expect_new_bind(transition, calls);

    transition.slots[slot].present = false;
    expect_new_bind(transition, calls);

    transition.slots[slot].present = true;
    expect_new_bind(transition, calls);
  }

  // An unsampled present texture is represented by the global empty pair, so
  // changing its source texture must not change the key.
  transition.sampled = 0xfe;
  transition.slots[0].view += 1000;
  expect_new_bind(transition, calls);
  transition.slots[0].view += 1;
  transition.slots[0].mode1 ^= 0x4000;
  transition.slots[0].replacement = !transition.slots[0].replacement;
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info_for(transition));
  assert(aurora::gfx::bindGroupRefCalls == calls);

  // Replacing either global empty resource changes every absent/unsampled
  // sampled slot and must materialize a new bind group.
  transition.slots[1].present = false;
  expect_new_bind(transition, calls);
  aurora::gx::sEmptyTextureView = wgpu::TextureView{9101};
  expect_new_bind(transition, calls);
  aurora::gx::sEmptySampler = wgpu::Sampler{9102};
  expect_new_bind(transition, calls);

  ShaderInfo empty;
  assert(aurora::gx::build_bind_groups(empty).textureBindGroup == 0);
  assert(aurora::gfx::bindGroupRefCalls == calls);
}

void first_use_stamp_and_cache_clear() {
  reset_runtime();
  auto transition = initial_transition();
  transition.frame = 0x12345678;
  apply_transition(transition);
  const auto first = aurora::gx::build_bind_groups(info_for(transition));
  assert(first.textureBindGroup != 0);
  assert(aurora::gfx::bindGroupRefCalls == 1);
  assert(aurora::gfx::g_bindGroups.size() == 1);
  assert(aurora::gfx::g_bindGroups.front().lastUsedFrame == transition.frame);

  // Clearing the global cache must invalidate both the retained key and all
  // secondary entries before any old BindGroupRef can be returned.
  aurora::gfx::clear_global_bind_groups();
  assert(aurora::gfx::g_bindGroups.empty());
  assert(!aurora::gx::sTextureBindGroupMemo.valid);
  for (const auto& entry : aurora::gx::sTextureBindGroupMemoTable) assert(!entry.valid);
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info_for(transition));
  assert(aurora::gfx::bindGroupRefCalls == 2);
  assert(aurora::gfx::g_bindGroups.size() == 1);
  assert(aurora::gfx::g_bindGroups.front().lastUsedFrame == transition.frame);

  // Direct lifecycle invalidation has the same guarantee while retaining the
  // global cache object itself.
  aurora::gx::clear_bind_group_memo();
  assert(!aurora::gx::sTextureBindGroupMemo.valid);
  for (const auto& entry : aurora::gx::sTextureBindGroupMemoTable) assert(!entry.valid);
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info_for(transition));
  assert(aurora::gfx::bindGroupRefCalls == 3);
  assert(aurora::gfx::g_bindGroups.front().lastUsedFrame == transition.frame);
}

void frame_and_lifecycle_invalidation() {
  reset_runtime();
  auto transition = initial_transition();
  const auto info = info_for(transition);
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  transition.frame = 8;
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  transition.frame = 7; // backwards transition must not reuse old frame.
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  transition.frame = UINT32_MAX;
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  transition.frame = 0; // explicit uint32 wrap.
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  assert(aurora::gfx::bindGroupRefCalls == 5);

  aurora::gx::clear_bind_group_memo();
  for (const auto& entry : aurora::gx::sTextureBindGroupMemoTable) assert(!entry.valid);
  const auto beforeClear = aurora::gfx::bindGroupRefCalls;
  transition.frame = 0;
  apply_transition(transition);
  (void)aurora::gx::build_bind_groups(info);
  assert(aurora::gfx::bindGroupRefCalls == beforeClear + 1);
}

std::vector<Transition> randomized_transitions() {
  auto state = initial_transition();
  std::vector<Transition> transitions;
  transitions.reserve(1800);
  for (unsigned i = 0; i < 1800; ++i) {
    if (i > 0 && i % 29 == 0) ++state.frame;
    if (i > 0 && i % 211 == 0) state.frame = 3;
    if (i % 43 == 0) state.layout = 70 + (i / 43) % 80;
    if (i % 47 == 0) state.slots[(i / 47) % 3].view = 100 + (i * 13) % 900;
    if (i % 53 == 0) state.slots[(i / 53) % 3].mode0 ^= 0x101 + i;
    if (i % 61 == 0) state.sampled ^= static_cast<uint8_t>(1u << ((i / 61) % 4));
    if (i % 67 == 0) state.sampledInd ^= static_cast<uint8_t>(1u << ((i / 67) % 4));
    if (i % 71 == 0) state.anisotropy = 1 + (i / 71) % 8;
    if (i % 79 == 0) state.slots[7].view = 10000 + i;
    if (i % 83 == 0) {
      state.sampled = 0;
      state.sampledInd = 0;
    } else if (i % 83 == 1) {
      state.sampled = 0x07;
      state.sampledInd = 0;
    }
    transitions.push_back(state);
  }
  return transitions;
}

void parity_and_budget() {
  const auto transitions = randomized_transitions();
  const auto candidate = run_sequence(transitions, 0);
  const auto reference = run_sequence(transitions, 1);
  const auto oracle = run_sequence(transitions, 2);
  assert(candidate.refs == reference.refs && candidate.refs == oracle.refs);
  assert(candidate.descriptors == reference.descriptors && candidate.descriptors == oracle.descriptors);
  assert(candidate.bindCalls <= reference.bindCalls);
  assert(candidate.bindCalls <= oracle.bindCalls);

  // Fill all 64 direct slots, then prove the bounded table evicts only the
  // colliding slot while retaining the other entries.
  reset_runtime();
  std::vector<Transition> slots;
  auto state = initial_transition();
  state.frame = 99;
  state.sampled = 1;
  for (unsigned i = 0; i < 64; ++i) {
    state.layout = i;
    state.slots[0].view = 1000 + i;
    slots.push_back(state);
  }
  for (const auto& slot : slots) {
    apply_transition(slot);
    (void)aurora::gx::build_bind_groups(info_for(slot));
  }
  assert(aurora::gfx::bindGroupRefCalls == 64);
  apply_transition(slots[0]);
  (void)aurora::gx::build_bind_groups(info_for(slots[0]));
  assert(aurora::gfx::bindGroupRefCalls == 64);
  state.layout = 64;
  state.slots[0].view = 1064;
  apply_transition(state);
  (void)aurora::gx::build_bind_groups(info_for(state));
  assert(aurora::gfx::bindGroupRefCalls == 65);
}

} // namespace

int main() {
  table_bound_and_last_key_fastpath();
  alternating_keys_and_forced_collisions();
  each_key_component_and_empty_slots();
  first_use_stamp_and_cache_clear();
  frame_and_lifecycle_invalidation();
  parity_and_budget();
  std::puts("PASS: GX texture bind-group table");
}
