#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

static size_t allocations = 0;
void* operator new(size_t size) {
  ++allocations;
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace wgpu {
using RenderPipeline = uint32_t;
} // namespace wgpu

using PipelineRef = uint64_t;
using HashType = uint64_t;

enum class ShaderType { Clear, GX, Rml };
enum class PipelinePriority { Background, Normal, Blocking };

static uint32_t uniformAlignment = 256;
static uint32_t align_uniform(uint32_t value) {
  return (value + uniformAlignment - 1) & ~(uniformAlignment - 1);
}
static uint32_t creates = 0;

namespace gx {
struct ShaderConfig {
  uint32_t seed = 0;
};
struct ShaderInfo {
  std::array<uint32_t, 32> fields{};
  uint32_t uniformSize = 0;
  bool operator==(const ShaderInfo&) const = default;
};
struct PipelineConfig {
  uint32_t key = 0;
  uint32_t payload = 0;
  ShaderConfig shaderConfig;
};
static ShaderInfo layout_oracle(const ShaderConfig& config) {
  ShaderInfo result;
  for (uint32_t i = 0; i < result.fields.size(); ++i) {
    result.fields[i] = config.seed * (i + 3) + i;
  }
  result.uniformSize = align_uniform(300 + config.seed % 512);
  return result;
}
static size_t layoutBuilds = 0;
static ShaderInfo build_shader_info(const ShaderConfig& config) {
  ++layoutBuilds;
  return layout_oracle(config);
}
static wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ++creates;
  return config.key + config.payload;
}
} // namespace gx

struct OtherConfig {
  uint32_t key = 0;
  uint32_t payload = 0;
};
static wgpu::RenderPipeline create_other_pipeline(const OtherConfig& config) {
  return config.key + config.payload;
}

template <class Config>
static PipelineRef xxh3_hash(const Config& config, HashType type) {
  return static_cast<PipelineRef>(config.key) + (type == static_cast<HashType>(ShaderType::GX) ? 0 : 100000);
}

static uint32_t frame = 10;
static uint32_t writes = 0;
static uint32_t notifications = 0;
static std::atomic<uint32_t> mutexLocks = 0;

static uint32_t current_frame() { return frame; }

#include "gx_ready_memo_cached.inc"

struct PendingPipeline {
  PipelineRef hash;
  uint32_t firstFrameUsed;
  std::function<wgpu::RenderPipeline()> create;
};
struct PipelineCacheWrite {
  uint32_t firstFrame;
};

static std::unordered_map<PipelineRef, CachedPipeline> g_pipelines;
static std::unordered_set<PipelineRef> g_pendingPipelines;
static std::deque<PendingPipeline> g_pipelineQueue;
static std::deque<PendingPipeline> g_backgroundPipelineQueue;
static bool g_hasPipelineThread = false;
static std::atomic_bool g_pipelineThreadEnd = false;
static uint32_t g_pipelinesPerFrame = 0;
static uint32_t queuedPipelines = 0;
static constexpr uint32_t BuildPipelinesPerFrame = 5;

struct CountingMutex {
  void lock() { ++mutexLocks; }
  void unlock() {}
  bool try_lock() {
    lock();
    return true;
  }
};
static CountingMutex g_pipelineMutex;

template <class Config>
static PipelineCacheWrite make_pipeline_cache_write(ShaderType, PipelineRef, const Config&, uint32_t first) {
  return {first};
}
static void enqueue_pipeline_cache_write(PipelineCacheWrite&&) { ++writes; }
static void notify_pipeline_ready(bool queued) {
  if (queued) --queuedPipelines;
  ++notifications;
}

static std::optional<PendingPipeline> take_pending_pipeline(PipelineRef hash) {
  for (auto* queue : {&g_pipelineQueue, &g_backgroundPipelineQueue}) {
    for (auto it = queue->begin(); it != queue->end(); ++it) {
      if (it->hash == hash) {
        auto result = std::move(*it);
        queue->erase(it);
        g_pendingPipelines.erase(hash);
        return result;
      }
    }
  }
  return {};
}
static PendingPipeline* touch_pending_pipeline(PipelineRef hash, PipelinePriority) {
  for (auto* queue : {&g_pipelineQueue, &g_backgroundPipelineQueue}) {
    for (auto& pending : *queue) {
      if (pending.hash == hash) return &pending;
    }
  }
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
  void notify_all() {}
  template <class Lock, class Predicate>
  void wait(Lock& lock, Predicate predicate) {
    lock.unlock();
    if (!g_pipelineThreadEnd) drain_worker();
    lock.lock();
    assert(predicate());
  }
};
static Condition g_pipelineReadyCv;
static Condition g_pipelineQueueCv;

#define ZoneScoped
#include "gx_ready_memo_defs.inc"
#include "gx_ready_memo_subject.inc"

static PipelineRef find_gx_pipeline(const gx::PipelineConfig& config, gx::ShaderInfo& info) {
  return find_pipeline_impl(ShaderType::GX, config, gx::create_pipeline, PipelinePriority::Blocking, std::nullopt,
                            &info);
}

static void reset() {
  g_pipelines.clear();
  g_pendingPipelines.clear();
  g_pipelineQueue.clear();
  g_backgroundPipelineQueue.clear();
  g_hasPipelineThread = false;
  g_pipelineThreadEnd = false;
  g_pipelinesPerFrame = 0;
  queuedPipelines = 0;
  notifications = 0;
  creates = 0;
  writes = 0;
  mutexLocks = 0;
  frame = 10;
  uniformAlignment = 256;
  g_lastPipelineRef = std::numeric_limits<PipelineRef>::max();
  g_gxReadyPipelineMemoGeneration.store(1, std::memory_order_release);
  g_gxReadyPipelineMemoFrame = UINT32_MAX;
  for (auto& entry : g_gxReadyPipelineMemo) entry.valid = false;
  gx::layoutBuilds = 0;
}

static gx::PipelineConfig config(uint32_t key, uint32_t seed = 1, uint32_t payload = 7) {
  return gx::PipelineConfig{key, payload, gx::ShaderConfig{seed}};
}

static void assert_info(const gx::ShaderInfo& info, const gx::PipelineConfig& value) {
  assert(info == gx::layout_oracle(value.shaderConfig));
}

static void repeats_and_output_parity() {
  reset();
  auto value = config(42, 3);
  gx::ShaderInfo info;
  const auto hash = find_gx_pipeline(value, info);
  assert(hash == 42 && creates == 1 && g_pipelines.at(hash).pipeline == 49);
  assert_info(info, value);
  const auto locks = mutexLocks.load();
  const auto layouts = gx::layoutBuilds;
  const auto allocationsBefore = allocations;
  for (uint32_t i = 0; i < 10000; ++i) {
    info.fields.fill(0xdeadbeef);
    info.uniformSize = 0;
    assert(find_gx_pipeline(value, info) == hash);
    assert_info(info, value);
  }
  assert(mutexLocks == locks && gx::layoutBuilds == layouts && creates == 1);
  assert(allocations == allocationsBefore);
}

static void reordered_keys_and_collisions() {
  reset();
  auto first = config(43, 4);
  auto second = config(44, 5);
  gx::ShaderInfo info;
  find_gx_pipeline(first, info);
  find_gx_pipeline(second, info);
  find_gx_pipeline(first, info);
  find_gx_pipeline(second, info);
  assert_info(info, second);
  assert(mutexLocks == 2 && gx::layoutBuilds == 2);

  reset();
  auto collisionA = config(64, 6);
  auto collisionB = config(128, 7);
  find_gx_pipeline(collisionA, info);
  find_gx_pipeline(collisionB, info);
  find_gx_pipeline(collisionA, info);
  assert_info(info, collisionA);
  assert(mutexLocks == 3 && gx::layoutBuilds == 2);
  find_gx_pipeline(collisionB, info);
  assert_info(info, collisionB);
  assert(mutexLocks == 4);
}

static void frame_alignment_and_first_frame_authority() {
  reset();
  auto value = config(51, 8);
  gx::ShaderInfo info;
  frame = 20;
  find_gx_pipeline(value, info);
  assert(g_pipelines.at(51).firstFrameUsed == 20);
  frame = 21;
  find_gx_pipeline(value, info);
  frame = 19;
  find_gx_pipeline(value, info);
  assert(g_pipelines.at(51).firstFrameUsed == 19);
  frame = UINT32_MAX;
  find_gx_pipeline(value, info);
  frame = 0;
  find_gx_pipeline(value, info);
  assert(mutexLocks == 5);

  reset();
  value = config(52, 9);
  frame = 31;
  find_gx_pipeline(value, info);
  const auto layouts = gx::layoutBuilds;
  uniformAlignment = 512;
  find_gx_pipeline(value, info);
  assert_info(info, value);
  assert(gx::layoutBuilds == layouts + 1);
  const auto locks = mutexLocks.load();
  find_gx_pipeline(value, info);
  assert(mutexLocks == locks);
  uniformAlignment = 256;
  find_gx_pipeline(value, info);
  assert_info(info, value);
  assert(mutexLocks == locks + 1);
}

static void lifecycle_pending_and_shutdown_paths() {
  reset();
  auto value = config(61, 10);
  gx::ShaderInfo info;
  find_gx_pipeline(value, info);
  const auto generation = g_gxReadyPipelineMemoGeneration.load();
  bump_gx_ready_pipeline_memo_generation();
  assert(g_gxReadyPipelineMemoGeneration.load() == generation + 1);
  const auto locks = mutexLocks.load();
  find_gx_pipeline(value, info);
  assert(mutexLocks == locks + 1);
  bump_gx_ready_pipeline_memo_generation();
  find_gx_pipeline(value, info);
  assert(mutexLocks == locks + 2);

  reset();
  g_hasPipelineThread = true;
  value = config(62, 11);
  const auto background = find_pipeline_impl(ShaderType::GX, value, gx::create_pipeline,
                                             PipelinePriority::Background, std::nullopt, &info);
  assert(!g_pipelines.contains(background) && g_pendingPipelines.contains(background));
  const auto queuedLocks = mutexLocks.load();
  assert(find_gx_pipeline(value, info) == background);
  assert_info(info, value);
  assert(g_pipelines.contains(background) && g_pendingPipelines.empty());
  const auto readyLocks = mutexLocks.load();
  find_gx_pipeline(value, info);
  assert(mutexLocks == readyLocks);
  assert(readyLocks > queuedLocks);

  reset();
  g_hasPipelineThread = true;
  g_pipelineThreadEnd = true;
  value = config(63, 12);
  find_gx_pipeline(value, info);
  assert_info(info, value);
  const auto failedLocks = mutexLocks.load();
  const auto failedLayouts = gx::layoutBuilds;
  find_gx_pipeline(value, info);
  assert_info(info, value);
  assert(mutexLocks > failedLocks && gx::layoutBuilds == failedLayouts + 1);
  assert(!g_pipelines.contains(63));
}

static void eligibility_guards() {
  reset();
  auto value = config(71, 13);
  gx::ShaderInfo info;
  find_pipeline_impl(ShaderType::GX, value, gx::create_pipeline, PipelinePriority::Blocking, 77, &info);
  const auto overrideLocks = mutexLocks.load();
  find_pipeline_impl(ShaderType::GX, value, gx::create_pipeline, PipelinePriority::Blocking, 77, &info);
  assert(mutexLocks == overrideLocks + 1);
  find_gx_pipeline(value, info);
  const auto readyLocks = mutexLocks.load();
  find_gx_pipeline(value, info);
  assert(mutexLocks == readyLocks);

  reset();
  value = config(72, 14);
  find_pipeline_impl(ShaderType::GX, value, gx::create_pipeline, PipelinePriority::Blocking, std::nullopt, nullptr);
  const auto nullLocks = mutexLocks.load();
  find_pipeline_impl(ShaderType::GX, value, gx::create_pipeline, PipelinePriority::Blocking, std::nullopt, nullptr);
  assert(mutexLocks == nullLocks + 1);

  reset();
  OtherConfig other{73, 4};
  find_pipeline_impl(ShaderType::Clear, other, create_other_pipeline, PipelinePriority::Blocking, std::nullopt,
                     &info);
  const auto nonGxLocks = mutexLocks.load();
  find_pipeline_impl(ShaderType::Clear, other, create_other_pipeline, PipelinePriority::Blocking, std::nullopt,
                     &info);
  assert(mutexLocks == nonGxLocks + 1);

  reset();
  value = config(74, 15);
  find_pipeline_impl(ShaderType::GX, value, gx::create_pipeline, PipelinePriority::Normal, std::nullopt, &info);
  const auto nonBlockingLocks = mutexLocks.load();
  find_pipeline_impl(ShaderType::GX, value, gx::create_pipeline, PipelinePriority::Normal, std::nullopt, &info);
  assert(mutexLocks == nonBlockingLocks);
  find_gx_pipeline(value, info);
  assert(mutexLocks == nonBlockingLocks + 1);
}

static void thread_local_memo() {
  reset();
  auto value = config(81, 16);
  gx::ShaderInfo info;
  find_gx_pipeline(value, info);
  const auto mainLocks = mutexLocks.load();
  std::thread producer([&] {
    gx::ShaderInfo childInfo;
    assert(find_gx_pipeline(value, childInfo) == 81);
    assert_info(childInfo, value);
    const auto childLocks = mutexLocks.load();
    childInfo.fields.fill(0xbeef);
    find_gx_pipeline(value, childInfo);
    assert_info(childInfo, value);
    assert(mutexLocks == childLocks);
  });
  producer.join();
  assert(mutexLocks == mainLocks + 1);
  info.fields.fill(0xabcd);
  find_gx_pipeline(value, info);
  assert_info(info, value);
  assert(mutexLocks == mainLocks + 1);
}

int main() {
  repeats_and_output_parity();
  reordered_keys_and_collisions();
  frame_alignment_and_first_frame_authority();
  lifecycle_pending_and_shutdown_paths();
  eligibility_guards();
  thread_local_memo();
  std::puts("PASS: GX ready pipeline memo");
}
