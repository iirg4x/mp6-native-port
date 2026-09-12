#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

static size_t allocations = 0, configCopies = 0;
void* operator new(size_t size) {
  ++allocations;
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace wgpu { using RenderPipeline = uint32_t; }
using PipelineRef = uint64_t;
using HashType = uint64_t;
enum class ShaderType { GX, Rml, Clear };
enum class PipelinePriority { Background, Normal, Blocking };
static uint32_t uniformAlignment = 256, layoutBuilds = 0;
static uint32_t align_uniform(uint32_t value) {
  return (value + uniformAlignment - 1) & ~(uniformAlignment - 1);
}
namespace gx {
struct ShaderConfig { uint32_t seed = 0; };
struct ShaderInfo {
  std::array<uint32_t, 32> fields{};
  uint32_t uniformSize = 0;
  bool operator==(const ShaderInfo&) const = default;
};
static ShaderInfo layout_oracle(const ShaderConfig& config) {
  ShaderInfo result;
  for (uint32_t i = 0; i < result.fields.size(); ++i)
    result.fields[i] = config.seed * (i + 3) + i;
  result.uniformSize = align_uniform(300 + config.seed % 512);
  return result;
}
static ShaderInfo build_shader_info(const ShaderConfig& config) { ++layoutBuilds; return layout_oracle(config); }
}
struct Config {
  uint32_t key = 0;
  std::array<uint32_t, 1024> payload{};
  gx::ShaderConfig shaderConfig;
  Config() = default;
  Config(const Config& other) : key(other.key), payload(other.payload), shaderConfig(other.shaderConfig) { ++configCopies; }
};
static PipelineRef xxh3_hash(const Config& config, HashType type) { return config.key + type * 100000; }
static uint32_t frame = 10, creates = 0, writes = 0, notifications = 0;
static uint32_t current_frame() { return frame; }
static wgpu::RenderPipeline create_pipeline(const Config& config) { ++creates; return config.key + config.payload[0]; }
namespace gx {
using PipelineConfig = Config;
static wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) { return ::create_pipeline(config); }
}
#include "lazy_pipeline_cached.inc"
struct PendingPipeline {
  PipelineRef hash;
  uint32_t firstFrameUsed;
  std::function<wgpu::RenderPipeline()> create;
};
struct PipelineCacheWrite { uint32_t firstFrame; };
static std::unordered_map<PipelineRef, CachedPipeline> g_pipelines;
static std::unordered_set<PipelineRef> g_pendingPipelines;
static std::deque<PendingPipeline> g_pipelineQueue, g_backgroundPipelineQueue;
static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = false, g_pipelineThreadEnd = false;
static uint32_t g_pipelinesPerFrame = 0, queuedPipelines = 0;
static constexpr uint32_t BuildPipelinesPerFrame = 5;
static PipelineRef g_lastPipelineRef = std::numeric_limits<PipelineRef>::max();
static PipelineCacheWrite make_pipeline_cache_write(ShaderType, PipelineRef, const Config&, uint32_t first) {
  return {first};
}
static void enqueue_pipeline_cache_write(PipelineCacheWrite&&) { ++writes; }
static void notify_pipeline_ready(bool queued) { if (queued) --queuedPipelines; ++notifications; }
static std::optional<PendingPipeline> take_pending_pipeline(PipelineRef hash) {
  for (auto* queue : {&g_pipelineQueue, &g_backgroundPipelineQueue}) {
    for (auto it = queue->begin(); it != queue->end(); ++it) {
      if (it->hash == hash) {
        auto result = std::move(*it);
        queue->erase(it); g_pendingPipelines.erase(hash);
        return result;
      }
    }
  }
  return {};
}
static PendingPipeline* touch_pending_pipeline(PipelineRef hash, PipelinePriority) {
  for (auto* queue : {&g_pipelineQueue, &g_backgroundPipelineQueue})
    for (auto& pending : *queue) if (pending.hash == hash) return &pending;
  return nullptr;
}
static void drain_worker() {
  while (!g_pendingPipelines.empty()) {
    auto pending = take_pending_pipeline(*g_pendingPipelines.begin());
    assert(pending);
    g_pipelines.emplace(pending->hash, CachedPipeline{pending->create(), pending->firstFrameUsed});
    notify_pipeline_ready(true);
  }
}
struct Condition {
  void notify_one() {}
  template<class Lock, class Predicate> void wait(Lock& lock, Predicate predicate) {
    // Deterministic worker scheduling at the same unlock/relock boundary.
    lock.unlock(); if (!g_pipelineThreadEnd) drain_worker(); lock.lock(); assert(predicate());
  }
};
static Condition g_pipelineReadyCv, g_pipelineQueueCv;
#define ZoneScoped
#include "lazy_pipeline_subject.inc"

int main() {
  Config config;
  config.key = 42; config.payload[0] = 7;
  auto hash = find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Blocking);
  assert(g_pipelines.at(hash).pipeline == 49 && creates == 1);
  assert(configCopies == 0); // Immediate creation borrows only for the call.
  const auto allocationsBefore = allocations, copiesBefore = configCopies;
  for (uint32_t i = 0; i < 100000; ++i)
    assert(find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Blocking) == hash);
  assert(allocations == allocationsBefore && configCopies == copiesBefore && creates == 1);
  std::puts("100000 ready-cache lookups: zero allocations, zero configuration copies");

  g_hasPipelineThread = true;
  PipelineRef background;
  {
    Config temporary;
    temporary.key = 81; temporary.payload[0] = 900;
    background = find_pipeline_impl(ShaderType::GX, temporary, create_pipeline, PipelinePriority::Background, 99);
    assert(!g_pipelines.contains(background) && g_pendingPipelines.contains(background));
    temporary.key = 0; temporary.payload[0] = 0;
  }
  drain_worker(); // Original caller and its stack data are already gone.
  assert(g_pipelines.at(background).pipeline == 981);
  assert(g_pipelines.at(background).firstFrameUsed == 99);

  config.key = 83; config.payload[0] = 11;
  auto queued = find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Background, 100);
  const auto copiesQueued = configCopies;
  // Touching a pending entry must not construct another callback.
  find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Normal, 50);
  assert(configCopies == copiesQueued);
  // A required first draw waits for the queued worker and retains the earliest use.
  assert(find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Blocking, 2) == queued);
  assert(g_pipelines.at(queued).pipeline == 94 && g_pipelines.at(queued).firstFrameUsed == 2);

  // Threadless fallback consumes the already-owned callback exactly once.
  config.key = 85;
  auto pending = find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Background, 100);
  g_hasPipelineThread = false;
  const auto createsBefore = creates;
  assert(find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Blocking, 1) == pending);
  assert(creates == createsBefore + 1 && g_pipelines.at(pending).firstFrameUsed == 1);
  assert(g_pendingPipelines.empty() && queuedPipelines == 0);

  // The per-frame compile budget still queues optional misses.
  g_pipelinesPerFrame = BuildPipelinesPerFrame;
  config.key = 87;
  auto budgeted = find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Normal);
  assert(g_pendingPipelines.contains(budgeted));
  drain_worker();
  assert(g_pipelines.at(budgeted).pipeline == 98);
  const auto writesBefore = writes;
  find_pipeline_impl(ShaderType::GX, config, create_pipeline, PipelinePriority::Blocking, 0);
  assert(g_pipelines.at(budgeted).firstFrameUsed == 0 && writes == writesBefore + 1);
  std::puts("PASS deferred ownership, ready/pending/missing paths, blocking, threadless and persistence");

  // Layout analysis lives in the existing ready entry, with no extra cache or
  // allocations. Copy all fields back even when the caller reuses dirty storage.
  config.key = 89; config.shaderConfig.seed = 9;
  find_pipeline_impl(ShaderType::GX, config, ::create_pipeline, PipelinePriority::Blocking);
  gx::ShaderInfo info;
  auto expected = gx::layout_oracle(config.shaderConfig);
  find_gx_pipeline(config, info);
  assert(info == expected && layoutBuilds == 1);
  const auto layoutAllocations = allocations, layoutCopies = configCopies;
  for (uint32_t i = 0; i < 100000; ++i) {
    info.fields.fill(0xdeadbeef);
    find_gx_pipeline(config, info);
    assert(info == expected);
  }
  assert(layoutBuilds == 1 && allocations == layoutAllocations && configCopies == layoutCopies);
  uniformAlignment = 512;
  find_gx_pipeline(config, info);
  assert(info == gx::layout_oracle(config.shaderConfig) && layoutBuilds == 2);
  g_pipelines.clear(); // Device/cache recreation must discard old layout data.
  find_gx_pipeline(config, info);
  assert(info == gx::layout_oracle(config.shaderConfig) && layoutBuilds == 3);

  // Required draw arriving while a prewarmed pipeline is queued.
  g_hasPipelineThread = true;
  config.key = 91; config.shaderConfig.seed = 17;
  find_pipeline_impl(ShaderType::GX, config, ::create_pipeline, PipelinePriority::Background);
  const auto queuedLayouts = layoutBuilds;
  find_gx_pipeline(config, info);
  assert(info == gx::layout_oracle(config.shaderConfig) && layoutBuilds == queuedLayouts + 1);
  assert(g_pendingPipelines.empty());

  // The threadless consumption branch must publish layout too.
  config.key = 93; config.shaderConfig.seed = 25;
  find_pipeline_impl(ShaderType::GX, config, ::create_pipeline, PipelinePriority::Background);
  g_hasPipelineThread = false;
  find_gx_pipeline(config, info);
  assert(info == gx::layout_oracle(config.shaderConfig));

  // Shutdown may wake a waiter without a ready pipeline. Preserve the original
  // layout calculation even though this draw cannot be submitted.
  g_hasPipelineThread = true; g_pipelineThreadEnd = true;
  config.key = 95; config.shaderConfig.seed = 31;
  find_gx_pipeline(config, info);
  assert(info == gx::layout_oracle(config.shaderConfig));
  std::puts("PASS 100000 cached layouts, alignment changes, cache recreation, worker/fallback/shutdown");
}
