#include "common.hpp"
#include "uniform_writer.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "../gx/pipeline.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "pipeline_cache.hpp"
#include "render_worker.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "texture.hpp"
#include "../window.hpp"
#include "../gx/fifo.hpp"
#include "../gx/command_processor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

#include "tracy/Tracy.hpp"

namespace aurora::gfx {
static Module Log("aurora::gfx");

using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;

#ifdef AURORA_GFX_DEBUG_GROUPS
std::vector<std::string> g_debugGroupStack;
std::vector<std::string> g_debugMarkers;
#endif

static std::string pass_label(std::string_view kind) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (!g_debugGroupStack.empty()) {
    return fmt::format("{} ({})", kind, g_debugGroupStack.back());
  }
#endif
  return std::string{kind};
}

// A frame's vertex, uniform, index and storage data, then its texture
// uploads, in one CPU-visible region.
#ifdef __EMSCRIPTEN__
// Web texture uploads go straight to Queue::WriteTexture: no region for them.
constexpr uint64_t FrameTextureUploadSize = 0;
#else
constexpr uint64_t FrameTextureUploadSize = UseTextureBuffer ? TextureUploadSize : 0;
#endif
constexpr uint64_t FrameDataSize =
    UniformBufferSize + VertexBufferSize + IndexBufferSize + StorageBufferSize + FrameTextureUploadSize;
#ifdef __EMSCRIPTEN__
// Web: Queue::WriteBuffer puts the frame's buffer data straight into the
// shared buffers (queue order keeps it behind the previous frame's
// commands), and texture uploads go straight in with Queue::WriteTexture
// (every upload fills a texture created this frame, so nothing has drawn
// from it yet). The GPU staging buffers are left unused: at full size they
// were 3 x 62 MB of the page's GPU memory, which iOS counts.
constexpr uint64_t StagingBufferSize = 4;
#else
constexpr uint64_t StagingBufferSize = FrameDataSize;
#endif
constexpr size_t FrameSlotCount = 2;
#ifdef __EMSCRIPTEN__
// Web: staging is filled by Queue::WriteBuffer at end_frame and its slot is
// free again at once (no MapAsync round trip), so the extra slots that cover
// map latency on native only cost memory: each is 62 MB of GPU buffer, and
// iOS kills the page when its total memory grows too large.
constexpr size_t StagingBufferCount = FrameSlotCount + 1;
#else
constexpr size_t StagingBufferCount = FrameSlotCount + 3;
#endif

#ifdef __EMSCRIPTEN__
// Web: a frame's vertices, uniforms, indices and storage are recorded into
// growable host buffers, and end_frame writes the bytes the frame used to the
// shared GPU buffers with Queue::WriteBuffer. The GPU buffers start at the
// sizes in common.hpp and grow (grow_web_buffer) instead of aborting when a
// frame outgrows them.
enum WebRegion : size_t { WebVerts, WebUniforms, WebIndices, WebStorage, WebRegionCount };
struct WebRegionState {
  uint64_t capacity;
  // The host buffer the last uploaded frame handed back, for the next frame.
  ByteBuffer host;
};
static std::array<WebRegionState, WebRegionCount> g_webRegions{{
    {VertexBufferSize, {}},
    {UniformBufferSize, {}},
    {IndexBufferSize, {}},
    {StorageBufferSize, {}},
}};
static void grow_web_buffer(WebRegion region, uint64_t needed);
#endif

struct StagingHighWater {
  uint32_t verts = 0;
  uint32_t uniforms = 0;
  uint32_t indices = 0;
  uint32_t storage = 0;
  uint32_t textureUpload = 0;
  size_t textureUploadCount = 0;
};

struct CustomDrawCommand {
  DrawTypeId type;
  uint32_t payloadSize;
  std::array<std::byte, InlineDrawPayloadSize> payload;
};

struct ShaderDrawCommand {
  ShaderType type;
  union {
    clear::DrawData clear;
    gx::DrawData gx;
#ifdef AURORA_ENABLE_RMLUI
    rmlui::DrawData rml;
#endif
  };
};
enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  CustomDraw,
  DebugMarker,
};
struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    ShaderDrawCommand draw;
    CustomDrawCommand customDraw;
    size_t debugMarkerIndex;
  } data;
};
} // namespace aurora::gfx

namespace aurora {
// For types that we can't ensure are safe to hash with has_unique_object_representations,
// we create specialized methods to handle them. Note that these are highly dependent on
// the structure definition, which could easily change with Dawn updates.
template <>
inline HashType xxh3_hash(const WGPUBindGroupDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(WGPUBindGroupDescriptor, layout); // skip nextInChain, label
  const auto hash = xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                                sizeof(WGPUBindGroupDescriptor) - offset - sizeof(void*) /* skip entries */, seed);
  return xxh3_hash_s(input.entries, sizeof(WGPUBindGroupEntry) * input.entryCount, hash);
}
template <>
inline HashType xxh3_hash(const wgpu::SamplerDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(wgpu::SamplerDescriptor, addressModeU); // skip nextInChain, label
  return xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                     sizeof(wgpu::SamplerDescriptor) - offset - 2 /* skip padding */, seed);
}
} // namespace aurora

namespace aurora::gfx {
namespace {
struct CachedBindGroup {
  wgpu::BindGroup bindGroup;
  uint32_t lastUsedFrame = 0;
#ifdef __EMSCRIPTEN__
  // Bind groups are keyed by the raw handles in their entries. Native Dawn
  // keeps a bound view/buffer/sampler alive, so a cached key can't be
  // reused; emdawnwebgpu frees the handle as soon as the app drops it and
  // the next CreateView/CreateBuffer can return the same address -- then a
  // lookup hands back the stale group still showing the OLD texture (RmlUi's
  // regenerated font atlas: scrambled menu text on the iPhone). Holding the
  // handles here keeps every cached key unique for as long as it is cached.
  std::vector<wgpu::TextureView> views;
  std::vector<wgpu::Buffer> buffers;
  std::vector<wgpu::Sampler> samplers;
#endif
};

#ifdef __EMSCRIPTEN__
void hold_bind_group_entries(CachedBindGroup& cached, const WGPUBindGroupDescriptor& descriptor) {
  for (size_t i = 0; i < descriptor.entryCount; ++i) {
    const auto& entry = descriptor.entries[i];
    if (entry.textureView) cached.views.emplace_back(entry.textureView);
    if (entry.buffer) cached.buffers.emplace_back(entry.buffer);
    if (entry.sampler) cached.samplers.emplace_back(entry.sampler);
  }
}
#endif

struct RuntimeDrawType {
  std::string label;
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
  uint32_t generation = 1;
};

struct RuntimeEncoderTaskType {
  std::string label;
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  bool readsDepth = false;
  uint32_t generation = 1;
};

constexpr uint32_t draw_type_index(DrawTypeId id) { return static_cast<uint32_t>(id & 0xFFFFFFFFu); }
constexpr uint32_t draw_type_generation(DrawTypeId id) { return static_cast<uint32_t>(id >> 32); }
constexpr DrawTypeId make_draw_type_id(uint32_t index, uint32_t generation) {
  return static_cast<DrawTypeId>(generation) << 32 | index;
}

constexpr uint32_t BindGroupCacheRetainFrames = 32;
constexpr uint32_t BindGroupCacheSweepPeriod = 16;
} // namespace

static absl::flat_hash_map<BindGroupRef, CachedBindGroup> g_cachedBindGroups;
static absl::flat_hash_map<SamplerRef, wgpu::Sampler> g_cachedSamplers;
static std::vector<RuntimeDrawType> g_runtimeDrawTypes;
static std::vector<uint32_t> g_freeDrawTypeSlots;
static std::vector<RuntimeEncoderTaskType> g_runtimeEncoderTaskTypes;
static std::vector<uint32_t> g_freeEncoderTaskTypeSlots;
static std::mutex g_bindGroupCacheMutex;
static std::mutex g_samplerCacheMutex;
static std::mutex g_runtimeDrawTypeMutex;

// Requires g_runtimeDrawTypeMutex held.
static const RuntimeDrawType* find_runtime_draw_type(DrawTypeId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidDrawType || idx >= g_runtimeDrawTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeDrawTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.draw == nullptr) {
    return nullptr;
  }
  return &slot;
}

// Requires g_runtimeDrawTypeMutex held.
static const RuntimeEncoderTaskType* find_runtime_encoder_task_type(EncoderTaskId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidEncoderTask || idx >= g_runtimeEncoderTaskTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeEncoderTaskTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.callback == nullptr) {
    return nullptr;
  }
  return &slot;
}

wgpu::Buffer g_vertexBuffer;
wgpu::Buffer g_uniformBuffer;
wgpu::Buffer g_indexBuffer;
wgpu::Buffer g_storageBuffer;
enum class BufferMapState {
  Unmapped,
  Mapping,
  Mapped,
};
static std::array<wgpu::Buffer, StagingBufferCount> g_stagingBuffers;
static std::array<std::atomic<BufferMapState>, StagingBufferCount> s_mappingStates;
static wgpu::Limits g_cachedLimits;
static std::atomic_uint32_t g_frameIndex{UINT32_MAX};
static PipelineRef g_currentPipeline;
wgpu::BindGroupLayout g_staticBindGroupLayout;
wgpu::BindGroup g_staticBindGroup;
wgpu::BindGroupLayout g_uniformBindGroupLayout;
wgpu::BindGroup g_uniformBindGroup;

// for imgui debug
// One worker publishes complete frames; API callers receive thread-local
// snapshots. Do not expose worker-owned storage or mix atomic/plain accesses.
static AuroraStats s_completedStats{};
static std::mutex s_statsMutex;

static void publish_completed_stats(const AuroraStats& stats) {
  std::lock_guard lock{s_statsMutex};
  s_completedStats = stats;
}

static AuroraStats read_completed_stats() {
  AuroraStats snapshot;
  {
    std::lock_guard lock{s_statsMutex};
    snapshot = s_completedStats;
  }
  read_pipeline_stats(snapshot);
  return snapshot;
}
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

using CommandList = std::vector<Command>;
struct RenderPass {
  std::string label;
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView; // MSAA resolve target; null if msaaSamples == 1
  wgpu::TextureView depthStencilView;
  wgpu::Texture copySourceTexture;
  wgpu::TextureView copySourceView;
  wgpu::TextureView copySourceDepthView;
  wgpu::Extent3D targetSize;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  Range resolveUniformRange;
  // Full-target snapshots for the public resolve_pass API
  wgpu::Texture snapshotColorDst;
  wgpu::TextureView snapshotDepthDst;
  // Public offscreen passes own a unique color destination for this frame.
  // Export it directly instead of copying the completed attachment again.
  bool colorSnapshotTarget = false;
  bool colorSnapshotExported = false;
  // Immutable snapshot inherited only by a loading continuation. Any draw or
  // clear invalidates reuse; offscreen suspension preserves the EFB identity.
  wgpu::TextureView unchangedDepthSnapshot;
  bool deferColorResolve = false;
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  float clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
  wgpu::LoadOp colorLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp colorStoreOp = wgpu::StoreOp::Store;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Store;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
  CommandList commands;
  bool clearColor = true;
  bool clearDepth = true;
  bool hasDepth = true;
  bool hasStencil = false;
  bool depthReadOnly = false;
  bool hasDraws = false;
  bool discardable = false;
  bool captureDepthSnapshot = false;
  bool finalEfb = false;
  bool sealed = false;
  // [MP6] aurora-patches/0020. msResolveCurrent: at the START of this pass the
  // MSAA resolve target already holds resolve(colorView) -- true only for the
  // continuation pass resolve_pass_into() creates, because the pass it just
  // sealed carries a resolveTarget and therefore always resolves.
  // skipRenderPass: this pass mutates nothing, so its BeginRenderPass/End pair
  // (and with it the store+reload of the sample-count-sized color+depth
  // attachment, plus a redundant full-attachment resolve) is pure cost; only
  // the trailing copy is encoded. See resolve_pass_into().
  bool msResolveCurrent = false;
  bool skipRenderPass = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;

  // Something reads this pass's output: a GX copy, snapshot, or direct export.
  bool has_consumer() const { return resolveTarget || snapshotColorDst || snapshotDepthDst || colorSnapshotExported; }
  // The pass mutates its attachments: draws or pending clears.
  bool has_content() const { return hasDraws || clearColor || clearDepth; }
};

struct TextureCopy {
  wgpu::TexelCopyTextureInfo src;
  wgpu::TexelCopyTextureInfo dst;
  wgpu::Extent3D size;
};

struct EncoderTask {
  wgpu::TextureView depth;
  wgpu::Extent3D targetSize{};
  uint32_t sampleCount = 1;
  EncoderTaskId type = InvalidEncoderTask;
  std::array<uint8_t, InlineDrawPayloadSize> payload{};
  uint32_t payloadSize = 0;
};

enum class FrameOpType : uint8_t {
  RenderPass,
  TextureCopy,
  EncoderTask,
};

struct FrameOp {
  FrameOpType type = FrameOpType::RenderPass;
  uint32_t index = 0;
  RenderPass* renderPass = nullptr;
  TextureCopy* textureCopy = nullptr;
  EncoderTask* encoderTask = nullptr;
  StagingHighWater highWater;
  std::vector<const TextureUpload*> textureUploads;
};

using RenderPassList = std::deque<RenderPass>;
struct FramePacket {
  RenderPassList renderPasses;
  std::deque<TextureCopy> textureCopies;
  std::deque<EncoderTask> encoderTasks;
  uint32_t nextOpIndex = 0;
  size_t capturedTextureUploads = 0;
  std::vector<CommandList> recycledCommands;
  std::deque<TextureUpload> textureUploads;
  ByteBuffer verts;
  ByteBuffer uniforms;
  ByteBuffer indices;
  ByteBuffer storage;
  ByteBuffer textureUpload;
  std::vector<std::shared_ptr<const void>> retainedFrameResources;
  wgpu::CommandEncoder encoder;
  uint64_t frameId = 0;
  uint32_t frameIndex = 0;
  bool hidden = false; // [MP6] 0008: never presented, unread passes dropped
  size_t stagingBuffer = 0;
  StagingHighWater copied;
  AuroraStats stats{};
};

static std::array<FramePacket, FrameSlotCount> g_framePackets;
static FramePacket* g_recordingFrame = nullptr;
static size_t g_recordingFrameSlot = 0;
static uint64_t g_nextFrameId = 1;
static render_worker::FrameSlotPool g_frameSlots{FrameSlotCount};
static render_worker::FrameSlotPool g_stagingSlots{StagingBufferCount};
static std::optional<size_t> g_reservedFrameSlot;
static std::optional<size_t> g_reservedStagingSlot;
static bool g_reservedTryQueue;
static u32 g_currentRenderPass = UINT32_MAX;
static bool g_hideNextFrame = false;
static bool g_inOffscreen = false;
static std::optional<RenderPass> g_suspendedEfbPass;
static Viewport g_suspendedEfbViewport;
static ClipRect g_suspendedEfbScissor;
static webgpu::TextureWithSampler g_offscreenColor;
static webgpu::TextureWithSampler g_offscreenDepth;
// Public snapshot passes and reusable GX shadows have different lifetimes.
// Never let full-screen AO evict the shadow target (or allocate unused color).
static webgpu::TextureWithSampler g_snapshotPassDepth;
static Viewport g_cachedViewport;
static ClipRect g_cachedScissor;

using PresentClock = std::chrono::steady_clock;
static constexpr auto PresentFpsWindow = std::chrono::seconds{1};
static std::mutex g_presentStatsMutex;
static std::deque<PresentClock::time_point> g_presentTimes;
static std::atomic_bool g_processEventsQueued = false;
static std::atomic_int64_t g_lastPresentNs = 0;
static std::atomic_int64_t g_presentPeriodNs = 0;
static std::atomic_int64_t g_cpuFrameTimeNs = 0;
static PresentClock::time_point g_cpuFrameStart;
static constexpr auto FrameStartSafetyMargin = std::chrono::milliseconds{2};
static constexpr auto MaxPacingSample = std::chrono::milliseconds{250};
static constexpr uint32_t PacingEmaWeight = 8;

static int64_t timestamp_ns(PresentClock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

static int64_t duration_ns(PresentClock::duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

static void update_ema(std::atomic_int64_t& value, int64_t sample) {
  if (sample <= 0 || sample > duration_ns(MaxPacingSample)) {
    return;
  }

  int64_t current = value.load(std::memory_order_acquire);
  while (true) {
    const int64_t next = current == 0 ? sample : current + (sample - current) / static_cast<int64_t>(PacingEmaWeight);
    if (value.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return;
    }
  }
}

static void prune_present_times(PresentClock::time_point now) {
  while (!g_presentTimes.empty() && g_presentTimes.front() + PresentFpsWindow < now) {
    g_presentTimes.pop_front();
  }
}

static void process_events() {
  ZoneScopedN("ProcessEvents");
  if (g_instance) {
    g_instance.ProcessEvents();
  }
}

static void enqueue_process_events() {
  if (render_worker::is_worker_thread()) {
    process_events();
    return;
  }

  bool expected = false;
  if (!g_processEventsQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return;
  }

  render_worker::enqueue_work([] {
    process_events();
    g_processEventsQueued.store(false, std::memory_order_release);
  });
}

static void wait_for_gpu_progress(uint64_t observed, std::chrono::nanoseconds sleepDuration) {
  if (render_worker::is_idle()) {
    enqueue_process_events();
  }
  render_worker::wait_for_progress(observed, sleepDuration);
}

static void pace_frame_start() {
  ZoneScopedN("Frame start pacing");
#ifdef __EMSCRIPTEN__
  return; // the browser's animation frame is the pacing clock (aurora.cpp)
#endif
  if (g_frameSlots.free_count() == FrameSlotCount) {
    return;
  }

  const int64_t lastPresentNs = g_lastPresentNs.load(std::memory_order_acquire);
  const int64_t presentPeriodNs = g_presentPeriodNs.load(std::memory_order_acquire);
  const int64_t cpuFrameTimeNs = g_cpuFrameTimeNs.load(std::memory_order_acquire);
  if (lastPresentNs == 0 || presentPeriodNs == 0 || cpuFrameTimeNs == 0) {
    return;
  }

  const int64_t safetyMarginNs = duration_ns(FrameStartSafetyMargin);
  const int64_t targetStartNs = lastPresentNs + presentPeriodNs - cpuFrameTimeNs - safetyMarginNs;
  int64_t nowNs = timestamp_ns(PresentClock::now());
  if (targetStartNs <= nowNs) {
    return;
  }

  const double initialWaitMs = static_cast<double>(targetStartNs - nowNs) / 1'000'000.0;
  TracyPlot("aurora: frameStartPaceWaitMs", initialWaitMs);
  while (nowNs < targetStartNs) {
    const int64_t remainingNs = targetStartNs - nowNs;
    const auto sleepDuration =
        remainingNs > 1'000'000 ? std::chrono::milliseconds{1} : std::chrono::nanoseconds{remainingNs};
    wait_for_gpu_progress(render_worker::progress_version(), sleepDuration);
    nowNs = timestamp_ns(PresentClock::now());
  }
}

#ifdef __EMSCRIPTEN__
// Web: emdawnwebgpu emulates a mapped range with a wasm-heap copy that Unmap
// writes back in full -- 62 MB per frame for this staging layout, measured as
// almost all of the frame's CPU time (and re-mapping cost as much again).
// Instead the staging buffers stay unmapped, each frame is recorded into host
// buffers (g_webRegions), and end_frame uploads only the bytes the frame used
// with Queue::WriteBuffer.
#endif

static void map_staging_buffer(size_t slot, bool releaseSlotOnCompletion = false) {
  auto expected = BufferMapState::Unmapped;
  if (!s_mappingStates[slot].compare_exchange_strong(expected, BufferMapState::Mapping, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return;
  }
#ifdef __EMSCRIPTEN__
  s_mappingStates[slot].store(BufferMapState::Mapped, std::memory_order_release);
  if (releaseSlotOnCompletion) {
    g_stagingSlots.release(slot);
  }
  render_worker::notify_progress();
  return;
#endif

  g_stagingBuffers[slot].MapAsync(
      wgpu::MapMode::Write, 0, StagingBufferSize, wgpu::CallbackMode::AllowSpontaneous,
      [slot, releaseSlotOnCompletion](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        if (status == wgpu::MapAsyncStatus::CallbackCancelled || status == wgpu::MapAsyncStatus::Aborted) {
          Log.warn("Buffer mapping {}: {}", magic_enum::enum_name(status), message);
          s_mappingStates[slot].store(BufferMapState::Unmapped, std::memory_order_release);
          if (releaseSlotOnCompletion) {
            g_stagingSlots.release(slot);
          }
          render_worker::notify_progress();
          return;
        }
        ASSERT(status == wgpu::MapAsyncStatus::Success, "Buffer mapping failed: {} {}", magic_enum::enum_name(status),
               message);
        s_mappingStates[slot].store(BufferMapState::Mapped, std::memory_order_release);
        if (releaseSlotOnCompletion) {
          g_stagingSlots.release(slot);
        }
        render_worker::notify_progress();
      });
}

static void set_efb_targets(RenderPass& pass) {
  pass.colorView = webgpu::g_frameBuffer.view;
  pass.resolveView = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : nullptr;
  pass.depthStencilView = webgpu::g_depthBuffer.view;
  pass.copySourceTexture =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.texture : webgpu::g_frameBuffer.texture;
  pass.copySourceView =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : webgpu::g_frameBuffer.view;
  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.targetSize = webgpu::g_frameBuffer.size;
  pass.msaaSamples = webgpu::g_graphicsConfig.msaaSamples;
  pass.hasDepth = true;
  pass.hasStencil = false;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth;
  // [MP6] LRU stamp for the bounded eviction in get_offscreen_textures().
  uint64_t lastUse = 0;
};
static absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;
// [MP6] Bound g_offscreenCache. It is keyed by (width,height) and was only
// ever cleared on surface resize (clear_caches) or shutdown, so any workload
// that renders offscreen passes at several distinct sizes over a run --
// notably cycling the Mods-page Shadow Quality, whose offscreen shadow pass
// resolves at (size*scale*2)^2 -- accreted a full-resolution color + depth
// target pair per distinct size for the entire process lifetime (hundreds of
// MiB of GPU memory, a device-loss risk). Retain two sizes: split-camera games
// such as M632 render 240- and 192-base maps in the SAME frame. A one-entry
// cache recreated both large targets every tick, retaining queued references
// while allocating replacements and eventually exhausting VRAM. This bounded
// two-size working set avoids that churn; LRU still retires older sizes, and
// shadow teardown explicitly clears both when quality or overlay changes.
static constexpr size_t MP6_OFFSCREEN_CACHE_MAX = 2;
static uint64_t g_offscreenClock = 0;

// Pooled destinations for the public resolve_pass API. Entries are recycled
// per frame slot: a slot is only re-acquired after the render worker has
// submitted its previous frame, and queue serialization orders the new frame's
// copies after the old frame's reads.
struct PassSnapshotEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth; // R32Float raw depth
};
struct PassSnapshotPool {
  std::vector<PassSnapshotEntry> entries;
  size_t used = 0;
};
static std::array<PassSnapshotPool, FrameSlotCount> g_passSnapshotPools;

static PassSnapshotEntry& acquire_pass_snapshot(uint32_t width, uint32_t height, bool wantColor, bool wantDepth) {
  auto& pool = g_passSnapshotPools[g_recordingFrameSlot];
  if (pool.used == pool.entries.size()) {
    pool.entries.emplace_back();
  }
  auto& entry = pool.entries[pool.used++];
  const wgpu::Extent3D size{width, height, 1};
  if (wantColor && (!entry.color.texture || entry.color.size.width != width || entry.color.size.height != height ||
                    entry.color.format != webgpu::g_graphicsConfig.surfaceConfiguration.format)) {
    const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
    const wgpu::TextureDescriptor desc{
        .label = "Pass Snapshot Color",
        .usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc |
                 wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment,
        .dimension = wgpu::TextureDimension::e2D,
        .size = size,
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    auto texture = g_device.CreateTexture(&desc);
    auto view = texture.CreateView();
    entry.color = webgpu::TextureWithSampler{
        .texture = std::move(texture),
        .view = std::move(view),
        .size = size,
        .format = format,
    };
  }
  if (wantDepth && (!entry.depth.texture || entry.depth.size.width != width || entry.depth.size.height != height)) {
    const wgpu::TextureDescriptor desc{
        .label = "Pass Snapshot Depth",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = size,
        .format = wgpu::TextureFormat::R32Float,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    auto texture = g_device.CreateTexture(&desc);
    auto view = texture.CreateView();
    entry.depth = webgpu::TextureWithSampler{
        .texture = std::move(texture),
        .view = std::move(view),
        .size = size,
        .format = wgpu::TextureFormat::R32Float,
    };
  }
  return entry;
}

static FramePacket& current_frame_packet() {
  CHECK(g_recordingFrame != nullptr, "No active frame packet");
  return *g_recordingFrame;
}

static RenderPassList& current_render_passes() { return current_frame_packet().renderPasses; }

// [MP6] Retained replay (compat/aurora/windows/0006). One recorded GX region
// of a replay frame: the commands it pushed, the copy-clear pass boundaries it
// opened, and the vertex/index/storage/uniform bytes it appended. A later
// replay of the same stream re-emits all of it instead of re-decoding the FIFO.
// A recording is refused when the region did anything beyond plain EFB
// drawing: offscreen passes, copies, uploads, palette conversions, encoder
// tasks, custom draws or retained frame resources.
namespace {
struct RetainedOp {
  bool copyClear = false;
  uint32_t pass = 0;
  uint32_t index = 0;
  CommandType type = CommandType::SetViewport;
  Command::Data data{};
  bool clearColor = false, clearAlpha = false, clearDepth = false;
  Vec4<float> clearColorValue{};
  float clearDepthValue = 0.f;
};
struct RetainedRegion {
  bool valid = false;
  uint32_t startPass = 0;
  size_t startPassCount = 0;
  size_t startCommandCount = 0;
  wgpu::Extent3D targetSize{};
  uint32_t msaaSamples = 0;
  Viewport startViewport{}, endViewport{};
  ClipRect startScissor{}, endScissor{};
  StagingHighWater start{}, end{};
  size_t textureCopies = 0, encoderTasks = 0, paletteConvs = 0;
  uint32_t drawCalls = 0, mergedDrawCalls = 0;
  std::vector<RetainedOp> ops;
  std::vector<BindGroupRef> bindGroups;
  std::vector<uint8_t> verts, uniforms, indices, storage;
};
RetainedRegion g_retained;
// While recording, the frame's byte buffers are swapped for these host-memory
// buffers: the staging buffer is write-combined mapped memory, and reading the
// recorded region back out of it costs far more than the decode it replaces.
std::array<ByteBuffer, 4> g_retainHost;
bool g_retainRecording = false;
bool g_retainTainted = false;
// [MP6] Set by aurora_gx_replay_stream_hooked while its region records: the
// hooks' custom draws (inline payloads only) are re-emitted in order on reuse.
bool g_retainAllowCustom = false;
int g_retainSuppress = 0;

void retained_note_command(CommandType type, uint32_t pass, size_t index) {
  if (!g_retainRecording || g_retainSuppress != 0) return;
  if (type == CommandType::CustomDraw && !g_retainAllowCustom) {
    g_retainTainted = true;
    return;
  }
  RetainedOp op;
  op.pass = pass;
  op.index = static_cast<uint32_t>(index);
  op.type = type;
  g_retained.ops.push_back(op);
}
} // namespace

static StagingHighWater current_high_water(const FramePacket& frame) noexcept {
  return {
      .verts = static_cast<uint32_t>(frame.verts.size()),
      .uniforms = static_cast<uint32_t>(frame.uniforms.size()),
      .indices = static_cast<uint32_t>(frame.indices.size()),
      .storage = static_cast<uint32_t>(frame.storage.size()),
      .textureUpload = static_cast<uint32_t>(frame.textureUpload.size()),
      .textureUploadCount = frame.textureUploads.size(),
  };
}

static FrameOp capture_frame_op(FramePacket& frame, FrameOpType type, uint32_t index) {
  FrameOp op{
      .type = type,
      .index = index,
      .renderPass =
          type == FrameOpType::RenderPass && index < frame.renderPasses.size() ? &frame.renderPasses[index] : nullptr,
      .textureCopy = type == FrameOpType::TextureCopy && index < frame.textureCopies.size()
                         ? &frame.textureCopies[index]
                         : nullptr,
      .encoderTask =
          type == FrameOpType::EncoderTask && index < frame.encoderTasks.size() ? &frame.encoderTasks[index] : nullptr,
      .highWater = current_high_water(frame),
  };
  // Operations are encoded in order: snapshot only newly published uploads.
  // Recopying the complete prefix into every closure was quadratic at load.
  op.textureUploads.reserve(op.highWater.textureUploadCount - frame.capturedTextureUploads);
  for (size_t i = frame.capturedTextureUploads; i < op.highWater.textureUploadCount; ++i) {
    op.textureUploads.push_back(&frame.textureUploads[i]);
  }
  frame.capturedTextureUploads = op.highWater.textureUploadCount;
  return op;
}

static void seal_pass(FramePacket& frame, uint32_t passIndex) {
  if (passIndex >= frame.renderPasses.size()) {
    return;
  }
  auto& pass = frame.renderPasses[passIndex];
  if (pass.sealed) {
    return;
  }
  pass.sealed = true;
}

static void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op);
static void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex);
static void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, const RenderPass& passInfo);
static void render_custom_draw(const CustomDrawCommand& draw, const wgpu::RenderPassEncoder& pass,
                               const RenderPass& passInfo);
static void execute_encoder_task(wgpu::CommandEncoder& cmd, const EncoderTask& task);
static void resume_efb_pass_loading(const RenderPass& prevPass);
static void expire_cached_bind_groups();
static void push_command(CommandType type, const Command::Data& data);

static void enqueue_op(FramePacket& frame, size_t frameSlot, FrameOp op) {
  const auto opIndex = frame.nextOpIndex++;
  render_worker::enqueue_encode_pass(frame.frameId, opIndex, [frameSlot, op = std::move(op)] {
    if (op.renderPass == nullptr && op.textureCopy == nullptr && op.encoderTask == nullptr) {
      return;
    }
    auto& packet = g_framePackets[frameSlot];
    encode_op(packet.encoder, packet, op);
  });
}

static void enqueue_pass(FramePacket& frame, size_t frameSlot, uint32_t passIndex) {
  seal_pass(frame, passIndex);
  enqueue_op(frame, frameSlot, capture_frame_op(frame, FrameOpType::RenderPass, passIndex));
}

void queue_texture_upload(TextureUpload upload) {
  if (g_currentRenderPass != UINT32_MAX) {
    ASSERT(!current_render_passes()[g_currentRenderPass].sealed,
           "Attempted to append texture upload to sealed render pass {}", g_currentRenderPass);
  }
  current_frame_packet().textureUploads.emplace_back(std::move(upload));
}

bool texture_upload_fits_staging(uint32_t bytesPerRow, uint32_t rowsPerImage) {
#ifdef __EMSCRIPTEN__
  return true; // written directly, see queue_texture_upload_data
#endif
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  return current_frame_packet().textureUpload.size() + copyBytesPerRow * rowsPerImage <= TextureUploadSize;
}

void queue_texture_upload_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage,
                               wgpu::TexelCopyTextureInfo tex, wgpu::Extent3D size) {
#ifdef __EMSCRIPTEN__
  {
    // Every caller fills a texture it just created (static conversions, HD
    // replacements, UI images), so writing it now, ahead of this frame's
    // commands, is what a staged copy in the frame would have produced, and
    // the browser stages the bytes itself.
    const wgpu::TexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = rowsPerImage};
    const size_t bytes = size_t{bytesPerRow} * rowsPerImage * std::max(size.depthOrArrayLayers, 1u);
    g_queue.WriteTexture(&tex, data, bytes, &layout, &size);
    return;
  }
#endif
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  auto& frame = current_frame_packet();
  if (frame.textureUpload.size() + copyBytesPerRow * rowsPerImage <= TextureUploadSize) {
    const auto range = push_texture_data(data, bytesPerRow, rowsPerImage);
    const wgpu::TexelCopyBufferLayout layout{
        .offset = range.offset,
        .bytesPerRow = bytesPerRow,
        .rowsPerImage = rowsPerImage,
    };
    queue_texture_upload(TextureUpload{layout, std::move(tex), size});
    return;
  }

  const uint64_t uploadSize = copyBytesPerRow * rowsPerImage;
  const wgpu::BufferDescriptor descriptor{
      .label = "Overflow Texture Upload Buffer",
      .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
      .size = uploadSize,
      .mappedAtCreation = true,
  };
  auto buffer = g_device.CreateBuffer(&descriptor);
  auto* dst = static_cast<uint8_t*>(buffer.GetMappedRange(0, uploadSize));
  for (uint32_t row = 0; row < rowsPerImage; ++row) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  buffer.Unmap();

  const wgpu::TexelCopyBufferLayout layout{
      .offset = 0,
      .bytesPerRow = bytesPerRow,
      .rowsPerImage = rowsPerImage,
  };
  queue_texture_upload(TextureUpload{layout, std::move(tex), size, std::move(buffer)});
}

void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);
    g_currentRenderPass = UINT32_MAX;
  }

  const auto copyIndex = static_cast<uint32_t>(frame.textureCopies.size());
  frame.textureCopies.emplace_back(TextureCopy{
      .src = std::move(src),
      .dst = std::move(dst),
      .size = size,
  });
  enqueue_op(frame, g_recordingFrameSlot, capture_frame_op(frame, FrameOpType::TextureCopy, copyIndex));
}

void begin_color_pass(const ColorPassDescriptor& desc) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);
  }

  RenderPass pass{
      .label = desc.label != nullptr ? desc.label : "",
      .colorView = desc.colorView,
      .resolveView = desc.resolveView,
      .depthStencilView = desc.depthStencilView,
      .targetSize = desc.targetSize,
      .msaaSamples = desc.sampleCount,
      .clearColorValue =
          {
              static_cast<float>(desc.clearColor.r),
              static_cast<float>(desc.clearColor.g),
              static_cast<float>(desc.clearColor.b),
              static_cast<float>(desc.clearColor.a),
          },
      .clearDepthValue = desc.depthClearValue,
      .colorLoadOp = desc.colorLoadOp,
      .colorStoreOp = desc.colorStoreOp,
      .depthLoadOp = desc.depthLoadOp,
      .depthStoreOp = desc.depthStoreOp,
      .stencilLoadOp = desc.stencilLoadOp,
      .stencilStoreOp = desc.stencilStoreOp,
      .stencilClearValue = desc.stencilClearValue,
      .clearColor = desc.colorLoadOp == wgpu::LoadOp::Clear,
      .clearDepth = desc.depthLoadOp == wgpu::LoadOp::Clear,
      .hasDepth = desc.hasDepth,
      .hasStencil = desc.hasStencil,
  };
  frame.renderPasses.emplace_back(std::move(pass));
  g_currentRenderPass = static_cast<uint32_t>(frame.renderPasses.size() - 1);

  g_cachedViewport = {0.f, 0.f, static_cast<float>(desc.targetSize.width), static_cast<float>(desc.targetSize.height),
                      0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(desc.targetSize.width), static_cast<int32_t>(desc.targetSize.height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_color_pass(bool finalUse) {
  ZoneScoped;
  if (g_currentRenderPass == UINT32_MAX) {
    return;
  }
  auto& pass = current_render_passes()[g_currentRenderPass];
  if (finalUse) {
    if (pass.hasStencil) pass.stencilStoreOp = wgpu::StoreOp::Discard;
    if (pass.resolveView) pass.colorStoreOp = wgpu::StoreOp::Discard;
  }
  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);
  g_currentRenderPass = UINT32_MAX;
}

static void push_command(CommandType type, const Command::Data& data) {
  if (g_currentRenderPass == UINT32_MAX)
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  auto& renderPass = current_render_passes()[g_currentRenderPass];
  ASSERT(!renderPass.sealed, "Attempted to append command {} to sealed render pass {}", magic_enum::enum_name(type),
         g_currentRenderPass);
  auto& spare = current_frame_packet().recycledCommands;
  if (renderPass.commands.capacity() == 0 && !spare.empty()) {
    renderPass.commands = std::move(spare.back());
    spare.pop_back();
  }
  if (type == CommandType::Draw || type == CommandType::CustomDraw) {
    renderPass.hasDraws = true;
  }
  retained_note_command(type, g_currentRenderPass, renderPass.commands.size());
  renderPass.commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_debugGroupStack,
#endif
      .data = data,
  });
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_currentRenderPass >= current_render_passes().size()) {
    return nullptr;
  }
  auto& last = current_render_passes()[g_currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.type != ShaderType::GX) {
    return nullptr;
  }
  return &last.data.draw.gx;
}

static void push_draw_command(ShaderDrawCommand data) {
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_drawCallCount;
}

Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_currentRenderPass < current_render_passes().size()) {
    const auto& size = current_render_passes()[g_currentRenderPass].targetSize;
    return {size.width, size.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

bool clip_viewport_to_target(Viewport& vp, float maxW, float maxH) noexcept {
  if (vp.left >= 0.f && vp.top >= 0.f && vp.left + vp.width <= maxW && vp.top + vp.height <= maxH) {
    return false;
  }
  const float right = std::min(vp.left + vp.width, maxW), bottom = std::min(vp.top + vp.height, maxH);
  vp.left = std::clamp(vp.left, 0.f, maxW);
  vp.top = std::clamp(vp.top, 0.f, maxH);
  vp.width = std::max(0.f, right - vp.left);
  vp.height = std::max(0.f, bottom - vp.top);
  return true;
}

void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_cachedViewport = cmd;
  }
}

void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_cachedScissor = cmd;
  }
}

template <>
void push_draw_command(clear::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Clear, .clear = data});
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, clear::create_pipeline);
}

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat) {
  // Resolve current render pass
  auto& prevPass = current_render_passes()[g_currentRenderPass];
  prevPass.resolveTarget = std::move(texture);
  prevPass.resolveRect = rect;
  prevPass.resolveFormat = resolveFormat;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const auto srcW = static_cast<float>(prevPass.targetSize.width);
  const auto srcH = static_cast<float>(prevPass.targetSize.height);
  const std::array uvTransform{
      static_cast<float>(rect.x) / srcW,
      static_cast<float>(rect.y) / srcH,
      static_cast<float>(rect.width) / srcW,
      static_cast<float>(rect.height) / srcH,
  };
  prevPass.resolveUniformRange = push_uniform(uvTransform);

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  const bool nextClearsColor = clearColor && clearAlpha;
  const bool nextClearsDepth = clearDepth;

  // [MP6] aurora-patches/0020 -- cut the per-copy multisample cost on tilers.
  //
  // Every GXCopyTex splits the EFB pass here, and the EFB attachments are
  // sample-count-sized (color 4x + depth 4x at MSAA 4x). The split's real cost
  // is NOT the resolve (that runs on-tile and writes one 1x-sized texture); it
  // is that colorStoreOp/depthStoreOp default to Store, so the pass break
  // flushes both multisampled attachments to main memory, and the continuation
  // pass's loadOp: Load drags them straight back in. That is 8x the bytes of a
  // non-MSAA break, per break, and on a tile-based mobile GPU it is the frame.
  //
  // Two cases can be paid less:
  //
  // (1) Back-to-back copies. A screen that copies several sub-rects of one
  //     finished EFB image (MP6's file select copies four 320x168 quadrants
  //     with no draw between them) gets one pass per copy, and all but the
  //     first are EMPTY: no draws, no clears. An empty pass's resolve output is
  //     byte-identical to the previous pass's, which already landed in the very
  //     same resolve target (set_efb_targets hangs it off every EFB pass), so
  //     the whole render pass is dead work. Skip it: the copy alone is encoded,
  //     reading the still-current resolve target. 4 copies then cost 1 pass
  //     break instead of 4.
  //
  // (2) The continuation clears. GXCopyTex(dest, GX_TRUE) makes the next pass
  //     start with loadOp: Clear, so the samples this pass would store are
  //     overwritten before anything reads them. StoreOp::Discard lets the
  //     driver keep them in tile memory and never write them out; WebGPU still
  //     writes resolveTarget (the resolve is specified independently of
  //     storeOp), so the copy is unaffected.
  //
  // Both are gated on msaaSamples > 1: the non-multisampled path is untouched.
  if (msaaSamples > 1) {
    if (prevPass.msResolveCurrent && !prevPass.hasDraws && !prevPass.clearColor && !prevPass.clearDepth &&
        !prevPass.captureDepthSnapshot && !prevPass.snapshotColorDst && !prevPass.snapshotDepthDst) {
      prevPass.skipRenderPass = true;
    } else {
      if (nextClearsColor) {
        prevPass.colorStoreOp = wgpu::StoreOp::Discard;
      }
      if (nextClearsDepth && !prevPass.captureDepthSnapshot && !prevPass.snapshotDepthDst) {
        prevPass.depthStoreOp = wgpu::StoreOp::Discard;
      }
    }
  }
  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);

  RenderPass newPass{
      .label = pass_label("EFB"),
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthStencilView = prevPass.depthStencilView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = nextClearsColor,
      .clearDepth = nextClearsDepth,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
  };
  // [MP6] prevPass carries a resolveTarget, so by the time this pass begins the
  // resolve target holds resolve(colorView) -- whether prevPass resolved for
  // real or was itself skipped because the value was already there. This is the
  // ONLY place the flag is set, which is what keeps the skip above confined to
  // consecutive GXCopyTex calls on an otherwise untouched EFB.
  newPass.msResolveCurrent = msaaSamples > 1;
  current_render_passes().emplace_back(std::move(newPass));
  ++g_currentRenderPass;

  if (!newPass.clearColor && (clearColor || clearAlpha)) {
    // If we're only clearing color _or_ alpha, perform a clear draw
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = false, // Depth cleared via render attachment
        }),
        .color =
            wgpu::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void replay_copy_clear(bool clearColor, bool clearAlpha, bool clearDepth, Vec4<float> clearColorValue,
                       float clearDepthValue) {
  if (!clearColor && !clearAlpha && !clearDepth) {
    return;
  }
  if (g_retainRecording && g_retainSuppress == 0) {
    RetainedOp op;
    op.copyClear = true;
    op.clearColor = clearColor;
    op.clearAlpha = clearAlpha;
    op.clearDepth = clearDepth;
    op.clearColorValue = clearColorValue;
    op.clearDepthValue = clearDepthValue;
    g_retained.ops.push_back(op);
  }
  // The boundary is recorded as one op; the commands it pushes are its own.
  struct SuppressScope {
    SuppressScope() { ++g_retainSuppress; }
    ~SuppressScope() { --g_retainSuppress; }
  } suppress;

  /* Retained replay suppresses the copy destination, but GXCopyTex(clear=true)
   * is also an EFB pass boundary. Preserve that visible clear without creating
   * or revising a CPU-addressed texture cache entry. */
  auto& prevPass = current_render_passes()[g_currentRenderPass];
  const auto msaaSamples = prevPass.msaaSamples;
  const bool nextClearsColor = clearColor && clearAlpha;
  if (msaaSamples > 1) {
    if (nextClearsColor) {
      prevPass.colorStoreOp = wgpu::StoreOp::Discard;
    }
    if (clearDepth && !prevPass.captureDepthSnapshot && !prevPass.snapshotDepthDst) {
      prevPass.depthStoreOp = wgpu::StoreOp::Discard;
    }
  }
  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);

  RenderPass newPass{
      .label = pass_label("EFB replay-copy clear"),
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthStencilView = prevPass.depthStencilView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = nextClearsColor,
      .clearDepth = clearDepth,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
  };
  current_render_passes().emplace_back(std::move(newPass));
  ++g_currentRenderPass;

  if (!nextClearsColor && (clearColor || clearAlpha)) {
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = false,
        }),
        .color =
            wgpu::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

static size_t count_palette_convs(const FramePacket& frame) {
  size_t n = 0;
  for (const auto& pass : frame.renderPasses) n += pass.paletteConvs.size();
  return n;
}

void retained_invalidate() noexcept { g_retained.valid = false; }

void retained_allow_custom_draws(bool allow) noexcept { g_retainAllowCustom = allow; }

bool retained_begin_record() noexcept {
  g_retained.valid = false;
  g_retainRecording = false;
  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX || g_inOffscreen) return false;
  auto& frame = current_frame_packet();
  const auto& pass = frame.renderPasses[g_currentRenderPass];
  g_retained.ops.clear();
  g_retained.bindGroups.clear();
  g_retained.startPass = g_currentRenderPass;
  g_retained.startPassCount = frame.renderPasses.size();
  g_retained.startCommandCount = pass.commands.size();
  g_retained.targetSize = pass.targetSize;
  g_retained.msaaSamples = pass.msaaSamples;
  g_retained.startViewport = g_cachedViewport;
  g_retained.startScissor = g_cachedScissor;
  g_retained.start = current_high_water(frame);
  g_retained.textureCopies = frame.textureCopies.size();
  g_retained.encoderTasks = frame.encoderTasks.size();
  g_retained.paletteConvs = count_palette_convs(frame);
  g_retained.drawCalls = g_drawCallCount;
  g_retained.mergedDrawCalls = g_mergedDrawCallCount;
  ByteBuffer* mapped[4] = {&frame.verts, &frame.uniforms, &frame.indices, &frame.storage};
  for (size_t i = 0; i < 4; ++i) {
    // Same offsets as the mapped buffer; the bytes before the region are never read.
    auto& host = g_retainHost[i];
    host.clear();
    if (mapped[i]->size() != 0) host.append_uninitialized(mapped[i]->size());
    std::swap(host, *mapped[i]);
  }
  g_retainRecording = true;
  g_retainTainted = false;
  return true;
}

// Swaps the mapped buffers back in and writes the region into them. A region
// that outgrew a mapped buffer aborts here, as the unrecorded decode would have.
static void retained_restore_buffers(FramePacket& frame) {
  ByteBuffer* mapped[4] = {&frame.verts, &frame.uniforms, &frame.indices, &frame.storage};
  for (size_t i = 0; i < 4; ++i) {
    std::swap(g_retainHost[i], *mapped[i]);
    const auto& host = g_retainHost[i];
    const size_t from = mapped[i]->size();
    if (host.size() > from) mapped[i]->append(host.data() + from, host.size() - from);
  }
}

static void copy_region(std::vector<uint8_t>& out, const ByteBuffer& buf, uint32_t from, uint32_t to) {
  if (to <= from) {
    out.clear();
    return;
  }
  out.assign(buf.data() + from, buf.data() + to);
}

bool retained_end_record() noexcept {
  if (!g_retainRecording) return false;
  g_retainRecording = false;
  if (g_recordingFrame == nullptr) return false;
  auto& frame = current_frame_packet();
  const auto end = current_high_water(frame);
  retained_restore_buffers(frame);
  if (g_retainTainted || g_currentRenderPass == UINT32_MAX || g_inOffscreen) {
    return false;
  }
  size_t copyClears = 0;
  for (const auto& op : g_retained.ops) copyClears += op.copyClear ? 1 : 0;
  if (frame.textureCopies.size() != g_retained.textureCopies ||
      frame.encoderTasks.size() != g_retained.encoderTasks ||
      count_palette_convs(frame) != g_retained.paletteConvs ||
      end.textureUpload != g_retained.start.textureUpload ||
      end.textureUploadCount != g_retained.start.textureUploadCount ||
      frame.renderPasses.size() != g_retained.startPassCount + copyClears ||
      g_currentRenderPass != g_retained.startPass + copyClears) {
    return false;
  }
  for (size_t i = g_retained.startPass; i < frame.renderPasses.size(); ++i) {
    if (frame.renderPasses[i].has_consumer()) return false;
  }
  for (auto& op : g_retained.ops) {
    if (op.copyClear) continue;
    if (op.pass >= frame.renderPasses.size() || op.index >= frame.renderPasses[op.pass].commands.size()) return false;
    const auto& cmd = frame.renderPasses[op.pass].commands[op.index];
    if (cmd.type != op.type) return false;
    // Copied now, not at push time: a later draw may have merged into it.
    op.data = cmd.data;
    if (cmd.type == CommandType::Draw && cmd.data.draw.type == ShaderType::GX &&
        cmd.data.draw.gx.bindGroups.textureBindGroup) {
      g_retained.bindGroups.push_back(cmd.data.draw.gx.bindGroups.textureBindGroup);
    }
  }
  std::sort(g_retained.bindGroups.begin(), g_retained.bindGroups.end());
  g_retained.bindGroups.erase(std::unique(g_retained.bindGroups.begin(), g_retained.bindGroups.end()),
                              g_retained.bindGroups.end());
  g_retained.end = end;
  g_retained.endViewport = g_cachedViewport;
  g_retained.endScissor = g_cachedScissor;
  g_retained.drawCalls = g_drawCallCount - g_retained.drawCalls;
  g_retained.mergedDrawCalls = g_mergedDrawCallCount - g_retained.mergedDrawCalls;
  copy_region(g_retained.verts, g_retainHost[0], g_retained.start.verts, end.verts);
  copy_region(g_retained.uniforms, g_retainHost[1], g_retained.start.uniforms, end.uniforms);
  copy_region(g_retained.indices, g_retainHost[2], g_retained.start.indices, end.indices);
  copy_region(g_retained.storage, g_retainHost[3], g_retained.start.storage, end.storage);
  g_retained.valid = true;
  return true;
}

uint8_t* retained_uniform_bytes(uint32_t& base, uint32_t& size) noexcept {
  base = g_retained.start.uniforms;
  size = static_cast<uint32_t>(g_retained.uniforms.size());
  return g_retained.uniforms.data();
}

static RetainedRegion g_retainedSnapshot;

void retained_snapshot() noexcept { g_retainedSnapshot = g_retained; }

static bool same_command(const RetainedOp& a, const RetainedOp& b) {
  if (a.copyClear != b.copyClear) return false;
  if (a.copyClear) {
    return a.clearColor == b.clearColor && a.clearAlpha == b.clearAlpha && a.clearDepth == b.clearDepth &&
           std::memcmp(&a.clearColorValue, &b.clearColorValue, sizeof(a.clearColorValue)) == 0 &&
           a.clearDepthValue == b.clearDepthValue;
  }
  if (a.type != b.type) return false;
  switch (a.type) {
  case CommandType::SetViewport:
    return a.data.setViewport == b.data.setViewport;
  case CommandType::SetScissor:
    return a.data.setScissor == b.data.setScissor;
  case CommandType::Draw: {
    const auto& x = a.data.draw;
    const auto& y = b.data.draw;
    if (x.type != y.type) return false;
    if (x.type == ShaderType::GX) {
      const auto& p = x.gx;
      const auto& q = y.gx;
      return p.pipeline == q.pipeline && p.vertRange.offset == q.vertRange.offset &&
             p.vertRange.size == q.vertRange.size && p.idxRange.offset == q.idxRange.offset &&
             p.idxRange.size == q.idxRange.size && p.uniformRange.offset == q.uniformRange.offset &&
             p.uniformRange.size == q.uniformRange.size && p.vtxCount == q.vtxCount &&
             p.indexCount == q.indexCount && p.instanceCount == q.instanceCount &&
             p.bindGroups.textureBindGroup == q.bindGroups.textureBindGroup && p.dstAlpha == q.dstAlpha &&
             p.vtxFmt == q.vtxFmt && p.mergeable == q.mergeable;
    }
    if (x.type == ShaderType::Clear) {
      return x.clear.pipeline == y.clear.pipeline &&
             std::memcmp(&x.clear.color, &y.clear.color, sizeof(x.clear.color)) == 0;
    }
    return false;
  }
  default:
    return false;
  }
}

const char* retained_compare_snapshot() noexcept {
  const auto& a = g_retainedSnapshot;
  const auto& b = g_retained;
  if (!b.valid) return "fresh decode was not recordable";
  if (a.ops.size() != b.ops.size()) return "command count";
  for (size_t i = 0; i < a.ops.size(); ++i) {
    if (!same_command(a.ops[i], b.ops[i])) return "command contents";
  }
  if (a.verts != b.verts) return "vertex bytes";
  if (a.indices != b.indices) return "index bytes";
  if (a.storage != b.storage) return "storage bytes";
  if (a.uniforms.size() != b.uniforms.size()) return "uniform size";
  if (a.uniforms != b.uniforms) {
    static char buf[160];
    size_t i = 0;
    while (a.uniforms[i] == b.uniforms[i]) ++i;
    std::snprintf(buf, sizeof(buf), "uniform bytes (first at region offset %zu of %zu)", i, a.uniforms.size());
    return buf;
  }
  if (!(a.endViewport == b.endViewport) || !(a.endScissor == b.endScissor)) return "end viewport/scissor";
  if (a.drawCalls != b.drawCalls || a.mergedDrawCalls != b.mergedDrawCalls) return "draw counts";
  return nullptr;
}

bool retained_can_reuse() noexcept {
  if (!g_retained.valid || g_recordingFrame == nullptr || g_inOffscreen ||
      g_currentRenderPass != g_retained.startPass) {
    return false;
  }
  const auto& frame = current_frame_packet();
  if (frame.renderPasses.size() != g_retained.startPassCount) return false;
  const auto& pass = frame.renderPasses[g_currentRenderPass];
  const auto now = current_high_water(frame);
  return pass.commands.size() == g_retained.startCommandCount && pass.msaaSamples == g_retained.msaaSamples &&
         pass.targetSize.width == g_retained.targetSize.width &&
         pass.targetSize.height == g_retained.targetSize.height && g_cachedViewport == g_retained.startViewport &&
         g_cachedScissor == g_retained.startScissor && now.verts == g_retained.start.verts &&
         now.uniforms == g_retained.start.uniforms && now.indices == g_retained.start.indices &&
         now.storage == g_retained.start.storage;
}

bool retained_reuse() noexcept {
  if (!retained_can_reuse()) return false;
  auto& frame = current_frame_packet();
  // Byte offsets match the recording exactly (checked above), so every Range
  // and every offset baked into the uniforms stays valid as recorded.
  // ByteBuffer::append(.., 0) on an empty buffer drops its capacity: skip it.
  const auto append = [](ByteBuffer& target, const std::vector<uint8_t>& bytes) {
    if (!bytes.empty()) target.append(bytes.data(), bytes.size());
  };
  append(frame.verts, g_retained.verts);
  append(frame.uniforms, g_retained.uniforms);
  append(frame.indices, g_retained.indices);
  append(frame.storage, g_retained.storage);
  const auto drawCalls = g_drawCallCount, mergedDrawCalls = g_mergedDrawCallCount;
  for (const auto& op : g_retained.ops) {
    if (op.copyClear) {
      replay_copy_clear(op.clearColor, op.clearAlpha, op.clearDepth, op.clearColorValue, op.clearDepthValue);
    } else {
      push_command(op.type, op.data);
    }
  }
  g_drawCallCount = drawCalls + g_retained.drawCalls;
  g_mergedDrawCallCount = mergedDrawCalls + g_retained.mergedDrawCalls;
  g_cachedViewport = g_retained.endViewport;
  g_cachedScissor = g_retained.endScissor;
  {
    // Keep the region's texture bind groups as fresh as the decode that
    // produced them would have (bind_group_ref refreshes lastUsedFrame).
    std::lock_guard lock{g_bindGroupCacheMutex};
    for (const auto id : g_retained.bindGroups) {
      const auto it = g_cachedBindGroups.find(id);
      if (it != g_cachedBindGroups.end()) it->second.lastUsedFrame = g_frameIndex;
    }
  }
  return true;
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  auto& renderPass = current_render_passes()[g_currentRenderPass];
  ASSERT(!renderPass.sealed, "Attempted to append palette conversion to sealed render pass {}", g_currentRenderPass);
  renderPass.paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_inOffscreen; }

uint32_t get_sample_count() noexcept {
  CHECK(g_currentRenderPass != UINT32_MAX, "get_sample_count called outside of a frame");
  return current_render_passes()[g_currentRenderPass].msaaSamples;
}

void clear_offscreen_cache() { g_offscreenCache.clear(); }

void clear_caches() noexcept {
  clear_offscreen_cache();
  g_snapshotPassDepth = {};
  gx::clear_bind_group_memo();
  std::lock_guard lock{g_bindGroupCacheMutex};
  g_cachedBindGroups.clear();
}

wgpu::Device device() noexcept { return g_device; }

wgpu::Queue queue() noexcept { return g_queue; }

wgpu::TextureFormat color_format() noexcept { return webgpu::g_graphicsConfig.surfaceConfiguration.format; }

wgpu::TextureFormat depth_format() noexcept { return webgpu::g_graphicsConfig.depthFormat; }

uint32_t sample_count() noexcept { return webgpu::g_graphicsConfig.msaaSamples; }

bool uses_reversed_z() noexcept { return gx::UseReversedZ; }

DrawTypeId register_draw_type(const DrawTypeDescriptor& desc) {
  if (desc.draw == nullptr) {
    Log.warn("register_draw_type: draw callback is null");
    return InvalidDrawType;
  }

  std::lock_guard lock{g_runtimeDrawTypeMutex};
  uint32_t idx;
  if (!g_freeDrawTypeSlots.empty()) {
    idx = g_freeDrawTypeSlots.back();
    g_freeDrawTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeDrawTypes.size());
    g_runtimeDrawTypes.emplace_back();
  }
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.draw = desc.draw;
  slot.userdata = desc.userdata;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_draw_type(DrawTypeId type) noexcept {
  std::lock_guard lock{g_runtimeDrawTypeMutex};
  if (find_runtime_draw_type(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label.clear();
  slot.draw = nullptr;
  slot.userdata = nullptr;
  ++slot.generation;
  g_freeDrawTypeSlots.push_back(idx);
}

bool push_custom_draw(DrawTypeId type, const void* payload, size_t payloadSize) {
  if (type == InvalidDrawType) {
    Log.warn("push_custom_draw: invalid draw type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_custom_draw: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_custom_draw: non-zero payload size with null payload");
    return false;
  }
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    if (find_runtime_draw_type(type) == nullptr) {
      Log.warn("push_custom_draw: unregistered draw type {:#x}", type);
      return false;
    }
  }

  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("push_custom_draw: called outside an active render pass");
    return false;
  }

  CustomDrawCommand draw{};
  draw.type = type;
  draw.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(draw.payload.data(), payload, payloadSize);
  }
  push_command(CommandType::CustomDraw, Command::Data{.customDraw = draw});
  ++g_drawCallCount;
  return true;
}

static OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    it->second.lastUse = ++g_offscreenClock; // [MP6] touch for LRU
    return it->second;
  }
  // [MP6] Evict the least-recently-used size(s) before inserting a new one, so
  // the cache never grows past MP6_OFFSCREEN_CACHE_MAX distinct sizes. The
  // entry about to be created is not a candidate (it is inserted below and
  // immediately stamped newest), and an offscreen entry is only in use inside
  // its own begin/end bracket -- during which it is the most-recently-used --
  // so eviction can never drop a target the current pass still needs.
  while (g_offscreenCache.size() >= MP6_OFFSCREEN_CACHE_MAX) {
    auto lru = g_offscreenCache.begin();
    for (auto it = g_offscreenCache.begin(); it != g_offscreenCache.end(); ++it) {
      if (it->second.lastUse < lru->second.lastUse) {
        lru = it;
      }
    }
    g_offscreenCache.erase(lru);
  }
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor colorDesc{
      .label = "Offscreen Color",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = colorFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto colorTexture = g_device.CreateTexture(&colorDesc);
  auto colorView = colorTexture.CreateView();
  webgpu::TextureWithSampler color{
      .texture = std::move(colorTexture),
      .view = std::move(colorView),
      .size = size,
      .format = colorFormat,
  };
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor depthDesc{
      .label = "Offscreen Depth",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = depthFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto depthTexture = g_device.CreateTexture(&depthDesc);
  auto depthView = depthTexture.CreateView();
  webgpu::TextureWithSampler depth{
      .texture = std::move(depthTexture),
      .view = std::move(depthView),
      .size = size,
      .format = depthFormat,
  };
  OffscreenCacheEntry entry{
      .color = std::move(color),
      .depth = std::move(depth),
      .lastUse = ++g_offscreenClock, // [MP6] newest
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

static void begin_offscreen_target(uint32_t width, uint32_t height, bool snapshotTarget,
                                   const wgpu::TextureView& readOnlyDepth = {}) {
  ZoneScoped;
  CHECK(g_currentRenderPass != UINT32_MAX, "begin_offscreen called outside of a frame");

  // If the current EFB pass has no resolve target, its output is unobservable.
  // Suspend it so that we can resume it after the offscreen pass.
  if (!g_inOffscreen) {
    auto& currentPass = current_render_passes()[g_currentRenderPass];
    if (!currentPass.resolveTarget) {
      g_suspendedEfbPass = std::move(currentPass);
      current_render_passes().pop_back();
      --g_currentRenderPass;
    } else {
      enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);
    }
    g_suspendedEfbViewport = g_cachedViewport;
    g_suspendedEfbScissor = g_cachedScissor;
  }

  // Create offscreen textures
  if (snapshotTarget) {
    g_offscreenColor = acquire_pass_snapshot(width, height, true, false).color;
    const auto format = webgpu::g_graphicsConfig.depthFormat;
    if (!readOnlyDepth && (!g_snapshotPassDepth.texture || g_snapshotPassDepth.size.width != width ||
        g_snapshotPassDepth.size.height != height || g_snapshotPassDepth.format != format)) {
      const wgpu::TextureDescriptor desc{
          .label = "Snapshot Pass Depth",
          .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
          .size = {width, height, 1}, .format = format,
      };
      auto texture = g_device.CreateTexture(&desc);
      auto view = texture.CreateView();
      g_snapshotPassDepth = {.texture = std::move(texture), .view = std::move(view),
                             .size = {width, height, 1}, .format = format};
    }
    g_offscreenDepth = readOnlyDepth ? webgpu::TextureWithSampler{} : g_snapshotPassDepth;
  } else {
    auto offscreenEntry = get_offscreen_textures(width, height);
    g_offscreenColor = std::move(offscreenEntry.color);
    g_offscreenDepth = std::move(offscreenEntry.depth);
  }

  // Start a new pass with offscreen targets
  RenderPass newPass{
      .label = pass_label("Offscreen"),
      .colorView = g_offscreenColor.view,
      .depthStencilView = readOnlyDepth ? readOnlyDepth : g_offscreenDepth.view,
      .copySourceTexture = g_offscreenColor.texture,
      .copySourceView = g_offscreenColor.view,
      .copySourceDepthView = readOnlyDepth ? readOnlyDepth : g_offscreenDepth.view,
      .targetSize = {width, height, 1},
      .msaaSamples = 1,
      .clearColorValue = {0.f, 0.f, 0.f, 0.f},
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearColor = true,
      .clearDepth = !readOnlyDepth,
      .hasDepth = true,
      .hasStencil = false,
      .depthReadOnly = bool(readOnlyDepth),
  };
  newPass.colorSnapshotTarget = snapshotTarget;
  current_render_passes().emplace_back(std::move(newPass));
  ++g_currentRenderPass;

  g_inOffscreen = true;

  g_cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void begin_offscreen(uint32_t width, uint32_t height) {
  begin_offscreen_target(width, height, false);
}

void end_offscreen() {
  ZoneScoped;
  CHECK(g_inOffscreen, "end_offscreen called without begin_offscreen");

  // Mark current render pass as discardable if there is no consumer
  auto& offscreenPass = current_render_passes()[g_currentRenderPass];
  offscreenPass.discardable = !offscreenPass.has_consumer();

  // Offscreen depth is cleared on every reuse. Only a depth consumer needs a
  // store; AO's color-only coverage export and ordinary shadow color copies do
  // not. Never discard depth before a requested snapshot/peek/GX depth copy.
  if (!offscreenPass.depthReadOnly && !offscreenPass.snapshotDepthDst &&
      !offscreenPass.captureDepthSnapshot &&
      (!offscreenPass.resolveTarget || !gx::is_depth_format(offscreenPass.resolveFormat))) {
    offscreenPass.depthStoreOp = wgpu::StoreOp::Discard;
  }

  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);

  g_inOffscreen = false;
  g_offscreenColor = {};
  g_offscreenDepth = {};

  // Resume suspended EFB pass, or start a new one (load existing content)
  if (g_suspendedEfbPass) {
    current_render_passes().emplace_back(std::move(*g_suspendedEfbPass));
    g_suspendedEfbPass.reset();
  } else {
    auto& pass = current_render_passes().emplace_back();
    pass.label = pass_label("EFB");
    pass.clearColor = false;
    pass.clearDepth = false;
  }
  ++g_currentRenderPass;
  set_efb_targets(current_render_passes()[g_currentRenderPass]);

  g_cachedViewport = g_suspendedEfbViewport;
  g_cachedScissor = g_suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

bool create_pass(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    Log.warn("create_pass: invalid size {}x{}", width, height);
    return false;
  }

  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("create_pass: called outside an active render pass");
    return false;
  }
  if (g_inOffscreen) {
    Log.warn("create_pass: an offscreen pass is already active (nesting is unsupported)");
    return false;
  }

  begin_offscreen_target(width, height, true);
  return true;
}

bool create_pass_from_efb_depth(BorrowedPassDepth& out) {
  out = {};
  gx::fifo::drain();
  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX || g_inOffscreen) {
    return false;
  }
  const auto& pass = current_render_passes()[g_currentRenderPass];
  if (!pass.hasDepth || !pass.copySourceDepthView || !pass.targetSize.width ||
      !pass.targetSize.height || !pass.msaaSamples) {
    return false;
  }
  // Copy values before resolve_pass appends a continuation and invalidates
  // references into the pass vector. Queue order owns the attachment contents:
  // EFB writes, offscreen reads, then resumed EFB writes.
  BorrowedPassDepth source{pass.copySourceDepthView, pass.targetSize.width,
                           pass.targetSize.height, pass.msaaSamples};
  ResolvedTargets unused;
  if (!resolve_pass({.color = false, .depth = false}, unused) ||
      !create_pass(source.width, source.height)) {
    return false;
  }
  out = std::move(source);
  return true;
}

bool create_pass_with_readonly_efb_depth(uint32_t& width, uint32_t& height) {
  width = height = 0;
  gx::fifo::drain();
  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX || g_inOffscreen) {
    return false;
  }
  const auto& pass = current_render_passes()[g_currentRenderPass];
  if (!pass.hasDepth || !pass.depthStencilView || !pass.targetSize.width ||
      !pass.targetSize.height || pass.msaaSamples != 1) {
    return false;
  }
  const auto depth = pass.depthStencilView;
  const auto size = pass.targetSize;
  ResolvedTargets unused;
  if (!resolve_pass({.color = false, .depth = false}, unused)) return false;
  // Own the view across continuation creation; queue order preserves its
  // contents until this read-only bracket closes and EFB rendering resumes.
  begin_offscreen_target(size.width, size.height, true, depth);
  width = size.width; height = size.height;
  return true;
}

bool ao_compatibility_mode() noexcept {
#ifdef __ANDROID__
  // 0.4.10 starts on the user's Qualcomm phone but fails with AO on Mali.
  // Keep unverified Android vendors on the previous schedule as well.
  return webgpu::g_adapterInfo.vendorID != 0x5143;
#else
  return false;
#endif
}

uint64_t efb_depth_write_serial() {
  gx::fifo::drain();
  return gx::fifo::efb_depth_write_serial();
}

bool resolve_pass(const ResolveDesc& desc, ResolvedTargets& out) {
  out = {};
  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("resolve_pass: called outside an active render pass");
    return false;
  }

  bool wantDepth = desc.depth;
  if (wantDepth && !tex_copy_conv::snapshot_depth_supported()) {
    Log.warn("resolve_pass: depth snapshots are unsupported on this device");
    wantDepth = false;
  }

  auto& prevPass = current_render_passes()[g_currentRenderPass];
  const uint32_t width = prevPass.targetSize.width;
  const uint32_t height = prevPass.targetSize.height;
  const bool reuseDepth = !ao_compatibility_mode() && wantDepth && !g_inOffscreen && !prevPass.has_content() &&
                          prevPass.unchangedDepthSnapshot;
  if (reuseDepth) out.depth = prevPass.unchangedDepthSnapshot;
  const bool copyDepth = wantDepth && !reuseDepth;
  const bool directColor = desc.color && g_inOffscreen && prevPass.colorSnapshotTarget;
  if (directColor) {
    out.color = prevPass.copySourceView;
    out.colorFormat = g_offscreenColor.format;
    prevPass.colorSnapshotExported = true;
  }
  // Requesting no snapshots is a plain pass break (or offscreen close, discarding its output).
  const bool copyColor = desc.color && !directColor;
  if (copyColor || copyDepth) {
    auto& entry = acquire_pass_snapshot(width, height, copyColor, copyDepth);
    if (copyColor) {
      prevPass.snapshotColorDst = entry.color.texture;
      out.color = entry.color.view;
      out.colorFormat = entry.color.format;
    }
    if (copyDepth) {
      prevPass.snapshotDepthDst = entry.depth.view;
      out.depth = entry.depth.view;
    }
  }
  out.width = width;
  out.height = height;

  if (g_inOffscreen) {
    // Seal the offscreen pass and resume the EFB.
    end_offscreen();
    return true;
  }

  // Seal the current EFB pass and continue on a new one that loads the existing contents.
  // EFB writes persist into the loading continuation, so content keeps the pass alive even
  // without a consumer.
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  // A depth-only consumer never reads resolved color. Preserve the MSAA
  // samples for the loading continuation; it still resolves normally for a
  // later color copy or presentation, including an empty final continuation.
  prevPass.deferColorResolve = !ao_compatibility_mode() && !desc.color && !prevPass.resolveTarget;
  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);
  resume_efb_pass_loading(prevPass);
  return true;
}

static void resume_efb_pass_loading(const RenderPass& prevPass) {
  RenderPass newPass{
      .label = pass_label("EFB"),
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthStencilView = prevPass.depthStencilView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = prevPass.msaaSamples,
      .unchangedDepthSnapshot = ao_compatibility_mode() ? nullptr :
          (prevPass.snapshotDepthDst ? prevPass.snapshotDepthDst :
           (!prevPass.has_content() ? prevPass.unchangedDepthSnapshot : nullptr)),
      .clearColor = false,
      .clearDepth = false,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
  };
  current_render_passes().emplace_back(std::move(newPass));
  ++g_currentRenderPass;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

EncoderTaskId register_encoder_task_type(const EncoderTaskDescriptor& desc) {
  if (desc.callback == nullptr) {
    Log.warn("register_encoder_task_type: callback is null");
    return InvalidEncoderTask;
  }

  std::lock_guard lock{g_runtimeDrawTypeMutex};
  uint32_t idx;
  if (!g_freeEncoderTaskTypeSlots.empty()) {
    idx = g_freeEncoderTaskTypeSlots.back();
    g_freeEncoderTaskTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeEncoderTaskTypes.size());
    g_runtimeEncoderTaskTypes.emplace_back();
  }
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.callback = desc.callback;
  slot.userdata = desc.userdata;
  slot.readsDepth = desc.readsDepth;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_encoder_task_type(EncoderTaskId type) noexcept {
  std::lock_guard lock{g_runtimeDrawTypeMutex};
  if (find_runtime_encoder_task_type(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label.clear();
  slot.callback = nullptr;
  slot.userdata = nullptr;
  ++slot.generation;
  g_freeEncoderTaskTypeSlots.push_back(idx);
}

bool push_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize) {
  if (type == InvalidEncoderTask) {
    Log.warn("push_encoder_task: invalid encoder task type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_encoder_task: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_encoder_task: non-zero payload size with null payload");
    return false;
  }
  bool readsDepth = false;
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    const auto* slot = find_runtime_encoder_task_type(type);
    if (slot == nullptr) {
      Log.warn("push_encoder_task: unregistered encoder task type {:#x}", type);
      return false;
    }
    readsDepth = slot->readsDepth;
  }

  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("push_encoder_task: called outside an active render pass");
    return false;
  }
  if (g_inOffscreen) {
    Log.warn("push_encoder_task: unsupported while an offscreen pass is active");
    return false;
  }

  // Seal the current EFB pass, record the task between it and a continuation
  // pass that loads the existing contents. EFB writes persist into the continuation, so
  // content keeps the sealed pass alive even without a consumer.
  auto& frame = current_frame_packet();
  auto& prevPass = current_render_passes()[g_currentRenderPass];
  if (readsDepth && (!prevPass.hasDepth || !prevPass.copySourceDepthView)) return false;
  prevPass.discardable = !readsDepth && !prevPass.has_consumer() && !prevPass.has_content();
  if (readsDepth) prevPass.depthStoreOp = wgpu::StoreOp::Store;
  enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);

  const auto taskIndex = static_cast<uint32_t>(frame.encoderTasks.size());
  auto& task = frame.encoderTasks.emplace_back(EncoderTask{.type = type});
  if (readsDepth) {
    task.depth = prevPass.copySourceDepthView;
    task.targetSize = prevPass.targetSize;
    task.sampleCount = prevPass.msaaSamples;
  }
  task.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(task.payload.data(), payload, payloadSize);
  }
  enqueue_op(frame, g_recordingFrameSlot, capture_frame_op(frame, FrameOpType::EncoderTask, taskIndex));

  resume_efb_pass_loading(prevPass);
  return true;
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::GX, .gx = data});
}

#ifdef AURORA_ENABLE_RMLUI
template <>
void push_draw_command(rmlui::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Rml, .rml = data});
}
#endif

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, gx::create_pipeline);
}

#ifdef AURORA_ENABLE_RMLUI
template <>
PipelineRef pipeline_ref(const rmlui::PipelineConfig& config) {
  return find_pipeline(ShaderType::Rml, config, rmlui::create_pipeline);
}
#endif

void initialize() {
  g_frameIndex = 0;
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
  render_worker::initialize();
  // This appears to take a while and blocks the render thread for periods of time
  // render_worker::set_event_pump([] {
  //   if (g_instance) {
  //     g_instance.ProcessEvents();
  //   }
  // });
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();
  texture_replacement::initialize();

  // For uniform & storage buffer offset alignments
  g_device.GetLimits(&g_cachedLimits);

  const auto createBuffer = [](wgpu::Buffer& out, wgpu::BufferUsage usage, uint64_t size, const char* label) {
    if (size <= 0) {
      return;
    }
    const wgpu::BufferDescriptor descriptor{
        .label = label,
        .usage = usage,
        .size = size,
    };
    out = g_device.CreateBuffer(&descriptor);
  };
  createBuffer(g_uniformBuffer, wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, UniformBufferSize,
               "Shared Uniform Buffer");
  createBuffer(g_vertexBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
               VertexBufferSize, "Shared Vertex Buffer");
  createBuffer(g_indexBuffer, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, IndexBufferSize,
               "Shared Index Buffer");
  createBuffer(g_storageBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, StorageBufferSize,
               "Shared Storage Buffer");
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    const auto label = fmt::format("Staging Buffer {}", i);
    createBuffer(g_stagingBuffers[i],
#ifdef __EMSCRIPTEN__
                 wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc,
#else
                 wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
#endif
 StagingBufferSize,
                 label.c_str());
  }
  for (auto& state : s_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
  for (size_t slot = 0; slot < g_stagingBuffers.size(); ++slot) {
    map_staging_buffer(slot);
  }

  {
    constexpr std::array layoutEntries{
        // Vertex data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        // Storage data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Static bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_staticBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_vertexBuffer,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_storageBuffer,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Static bind group",
        .layout = g_staticBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_staticBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  {
    constexpr std::array layoutEntries{
        // Uniform buffer (dynamic offset)
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::Uniform,
                    .hasDynamicOffset = true,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Uniform bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_uniformBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_uniformBuffer,
            .size = gx::MaxUniformSize,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Uniform bind group",
        .layout = g_uniformBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_uniformBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  gx::initialize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize_pipeline();
#endif
  initialize_pipeline_cache();
}

#ifdef __EMSCRIPTEN__
// Replaces a shared buffer with one at least twice `needed` for the frames
// after this one (this frame's passes were encoded against the old one), and
// the bind group that names it. Cached bind groups that named the old buffer
// are keyed by it, so they are simply not found again and expire with the cache.
static void grow_web_buffer(WebRegion region, uint64_t needed) {
  static constexpr std::array<const char*, WebRegionCount> Labels{
      "Shared Vertex Buffer", "Shared Uniform Buffer", "Shared Index Buffer", "Shared Storage Buffer"};
  static constexpr std::array<wgpu::BufferUsage, WebRegionCount> Usages{
      wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
      wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
      wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
  };
  auto& state = g_webRegions[region];
  uint64_t capacity = state.capacity;
  while (capacity < needed * 2) {
    capacity *= 2;
  }
  Log.warn("{}: a frame used {} bytes of {}; growing to {}", Labels[region], needed, state.capacity, capacity);
  const wgpu::BufferDescriptor descriptor{
      .label = Labels[region],
      .usage = Usages[region],
      .size = capacity,
  };
  auto buffer = g_device.CreateBuffer(&descriptor);
  state.capacity = capacity;
  switch (region) {
  case WebUniforms: {
    g_uniformBuffer = std::move(buffer);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_uniformBuffer,
            .size = gx::MaxUniformSize,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Uniform bind group",
        .layout = g_uniformBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_uniformBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
    break;
  }
  case WebIndices:
    g_indexBuffer = std::move(buffer);
    break;
  case WebVerts:
  case WebStorage: {
    (region == WebVerts ? g_vertexBuffer : g_storageBuffer) = std::move(buffer);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_vertexBuffer,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_storageBuffer,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Static bind group",
        .layout = g_staticBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_staticBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
    break;
  }
  default:
    break;
  }
}
#endif

void shutdown() {
  render_worker::synchronize();
  render_worker::shutdown();
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
  shutdown_pipeline_cache();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
  gx::shutdown();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown_pipeline();
#endif

  {
    std::lock_guard lock{g_bindGroupCacheMutex};
    g_cachedBindGroups.clear();
  }
  {
    std::lock_guard lock{g_samplerCacheMutex};
    g_cachedSamplers.clear();
  }
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    g_runtimeDrawTypes.clear();
    g_freeDrawTypeSlots.clear();
  }
  for (auto& pool : g_passSnapshotPools) {
    pool = {};
  }
  g_vertexBuffer = {};
  g_uniformBuffer = {};
  g_indexBuffer = {};
  g_storageBuffer = {};
  g_stagingBuffers.fill({});
  for (auto& packet : g_framePackets) {
    packet = {};
  }
  g_recordingFrame = nullptr;
  g_currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_offscreenColor = {};
  g_offscreenDepth = {};
  g_snapshotPassDepth = {};
  g_staticBindGroup = {};
  g_staticBindGroupLayout = {};
  g_uniformBindGroup = {};
  g_uniformBindGroupLayout = {};
  g_inOffscreen = false;
  g_frameIndex = UINT32_MAX;
  g_frameSlots.reset();
  g_stagingSlots.reset();
  for (auto& state : s_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
}

static bool wait_for_staging_buffer(size_t slot) {
  ZoneScopedN("Wait for buffer map");
  map_staging_buffer(slot);
  while (true) {
    const auto observed = render_worker::progress_version();
    const auto mappingState = s_mappingStates[slot].load(std::memory_order_acquire);
    if (mappingState == BufferMapState::Mapped) {
      return true;
    }
    if (mappingState == BufferMapState::Unmapped) {
      return false;
    }
    wait_for_gpu_progress(observed, std::chrono::milliseconds{1});
  }
}

static size_t acquire_frame_slot() {
  ZoneScopedN("Acquire frame slot");
  const auto waitStart = PresentClock::now();
  while (true) {
    const auto observed = render_worker::progress_version();
    if (const auto slot = g_frameSlots.try_acquire()) {
      const auto waitDuration = PresentClock::now() - waitStart;
      const double waitMs = std::chrono::duration<double, std::milli>{waitDuration}.count();
      TracyPlot("aurora: frameSlotWaitMs", waitMs);
      return *slot;
    }
    wait_for_gpu_progress(observed, std::chrono::microseconds{100});
  }
}

static std::optional<size_t> acquire_mapped_staging_buffer() {
  ZoneScopedN("Acquire mapped staging buffer");
  while (true) {
    const auto observed = render_worker::progress_version();
    if (auto slot = g_stagingSlots.try_acquire()) {
      if (wait_for_staging_buffer(*slot)) {
        return *slot;
      }
      g_stagingSlots.release(*slot);
      return std::nullopt;
    }
    wait_for_gpu_progress(observed, std::chrono::microseconds{100});
  }
}

static void recycle_frame_packet(FramePacket& frame) {
  // Only after its encoding work has finished, or before the frame is queued.
  // Retain bounded CPU command capacity, never handles/payloads or live draws.
  constexpr size_t MaxRetainedBytes = 2 * 1024 * 1024;
  constexpr size_t MaxRetainedLists = 32;
  auto spare = std::move(frame.recycledCommands);
  auto retainedResources = std::move(frame.retainedFrameResources);
  // Release resource pins but keep vector capacity so steady-state retained
  // geometry does not allocate once per frame.
  retainedResources.clear();
  size_t retainedBytes = 0;
  for (const auto& list : spare) retainedBytes += list.capacity() * sizeof(Command);
  for (auto& pass : frame.renderPasses) {
    const auto bytes = pass.commands.capacity() * sizeof(Command);
    if (bytes && retainedBytes + bytes <= MaxRetainedBytes && spare.size() < MaxRetainedLists) {
      pass.commands.clear();
      spare.push_back(std::move(pass.commands));
      retainedBytes += bytes;
    }
  }
  frame = {};
  frame.recycledCommands = std::move(spare);
  frame.retainedFrameResources = std::move(retainedResources);
}

bool begin_frame() {
  ZoneScoped;
  // pace_frame_start();
  size_t frameSlot;
  std::optional<size_t> stagingSlot;
  bool tryQueue = false;
  if (g_reservedFrameSlot && g_reservedStagingSlot) {
    frameSlot = *g_reservedFrameSlot;
    stagingSlot = *g_reservedStagingSlot;
    tryQueue = g_reservedTryQueue;
    g_reservedFrameSlot.reset();
    g_reservedStagingSlot.reset();
    g_reservedTryQueue = false;
  } else {
    frameSlot = acquire_frame_slot();
    stagingSlot = acquire_mapped_staging_buffer();
  }
  if (!stagingSlot) {
    g_frameSlots.release(frameSlot);
    return false;
  }

  auto& frame = g_framePackets[frameSlot];
  recycle_frame_packet(frame);
  frame.frameId = g_nextFrameId++;
  frame.frameIndex = g_frameIndex;
  frame.hidden = std::exchange(g_hideNextFrame, false);
  frame.stagingBuffer = *stagingSlot;
  g_recordingFrame = &frame;
  g_recordingFrameSlot = frameSlot;
  g_passSnapshotPools[frameSlot].used = 0;

  size_t bufferOffset = 0;
  const auto& stagingBuf = g_stagingBuffers[*stagingSlot];
  const auto mapBuffer = [&](ByteBuffer& buf, uint64_t size) {
    if (size <= 0) {
      return;
    }
    buf = ByteBuffer{static_cast<u8*>(stagingBuf.GetMappedRange(bufferOffset, size)), static_cast<size_t>(size)};
    bufferOffset += size;
  };
#ifdef __EMSCRIPTEN__
  // A frame that outgrows a GPU buffer grows its host buffer instead of
  // aborting; end_frame then grows the GPU buffer.
  const auto hostBuffer = [](ByteBuffer& buf, WebRegion region) {
    auto& state = g_webRegions[region];
    buf = std::move(state.host);
    if (buf.data() == nullptr) {
      buf = ByteBuffer{static_cast<size_t>(state.capacity)};
    }
    buf.clear();
  };
  hostBuffer(frame.verts, WebVerts);
  hostBuffer(frame.uniforms, WebUniforms);
  hostBuffer(frame.indices, WebIndices);
  hostBuffer(frame.storage, WebStorage);
  (void)mapBuffer;
#else
  mapBuffer(frame.verts, VertexBufferSize);
  mapBuffer(frame.uniforms, UniformBufferSize);
  mapBuffer(frame.indices, IndexBufferSize);
  mapBuffer(frame.storage, StorageBufferSize);
#endif
  if constexpr (UseTextureBuffer) {
    mapBuffer(frame.textureUpload, FrameTextureUploadSize);
  }

  g_drawCallCount = 0;
  g_mergedDrawCallCount = 0;
  g_suspendedEfbPass.reset();

  current_render_passes().emplace_back();
  auto& pass = current_render_passes()[0];
  pass.label = pass_label("EFB");
  set_efb_targets(pass);
  pass.clearColorValue = gx::g_gxState.clearColor;
  pass.clearDepthValue = gx::clear_depth_value();
  g_currentRenderPass = 0;
  // Refresh render viewport/scissor from logical in case FB size changed
  g_cachedViewport = gx::map_logical_viewport(gx::g_gxState.logicalViewport);
  g_cachedScissor = gx::map_logical_scissor(gx::g_gxState.logicalScissor);
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
  begin_pipeline_frame();
  auto beginWork = [frameSlot] {
    static constexpr wgpu::CommandEncoderDescriptor EncoderDescriptor{.label = "Redraw encoder"};
    g_framePackets[frameSlot].encoder = g_device.CreateCommandEncoder(&EncoderDescriptor);
    webgpu::gpu_prof::frame_begin(g_framePackets[frameSlot].encoder);
  };
  if (tryQueue) {
    if (!render_worker::try_enqueue_begin_frame(frame.frameId, std::move(beginWork))) {
      /* Nonblocking replay admission is all-or-nothing. Nothing observed this
       * packet, so return both permits and restore the between-frame state. */
      recycle_frame_packet(frame);
      g_recordingFrame = nullptr;
      g_currentRenderPass = UINT32_MAX;
      g_suspendedEfbPass.reset();
      g_cpuFrameStart = {};
      --g_nextFrameId;
      g_stagingSlots.release(*stagingSlot);
      g_frameSlots.release(frameSlot);
      return false;
    }
  } else {
    render_worker::enqueue_begin_frame(frame.frameId, std::move(beginWork));
  }
  g_cpuFrameStart = PresentClock::now();
  return true;
}

bool wait_for_replay_resources(std::chrono::nanoseconds timeout) {
  const auto ready = [] {
    if (g_recordingFrame || g_reservedFrameSlot || g_reservedStagingSlot ||
        g_frameSlots.free_count() == 0 || !render_worker::can_enqueue()) return false;
    const auto slot = g_stagingSlots.first_free();
    return slot && s_mappingStates[*slot].load(std::memory_order_acquire) == BufferMapState::Mapped;
  };
  const auto observed = render_worker::progress_version();
  if (ready()) return true;
  if (timeout.count() <= 0) return false;
  // Dispatch callbacks on an idle worker too; don't wait for a map callback
  // without giving backends that require ProcessEvents a chance to deliver it.
  if (render_worker::is_idle()) enqueue_process_events();
  render_worker::wait_for_progress(observed, timeout);
  return ready();
}

void hide_next_frame() noexcept { g_hideNextFrame = true; }

bool recording_frame_hidden() noexcept { return g_recordingFrame != nullptr && g_recordingFrame->hidden; }

bool try_begin_frame() {
  ZoneScopedN("Try begin replay frame");
  if (g_recordingFrame != nullptr || g_reservedFrameSlot || g_reservedStagingSlot) {
    return false;
  }
  const auto frameSlot = g_frameSlots.try_acquire();
  if (!frameSlot) {
    return false;
  }
  const auto stagingSlot = g_stagingSlots.try_acquire();
  if (!stagingSlot) {
    g_frameSlots.release(*frameSlot);
    return false;
  }
  if (s_mappingStates[*stagingSlot].load(std::memory_order_acquire) != BufferMapState::Mapped) {
    g_stagingSlots.release(*stagingSlot);
    g_frameSlots.release(*frameSlot);
    return false;
  }
  g_reservedFrameSlot = *frameSlot;
  g_reservedStagingSlot = *stagingSlot;
  g_reservedTryQueue = true;
  return begin_frame();
}

void finish() {
  ZoneScoped;
  if (g_recordingFrame == nullptr) {
    return;
  }
  ASSERT(!g_inOffscreen, "finish called while offscreen rendering is active");
  if (g_currentRenderPass != UINT32_MAX) {
    auto& frame = current_frame_packet();
    frame.uniforms.append_zeroes(gx::MaxUniformSize);
    auto& pass = frame.renderPasses[g_currentRenderPass];
    pass.captureDepthSnapshot = true;
    pass.finalEfb = true;
    enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);
    g_currentRenderPass = UINT32_MAX;
  }
}

void end_frame(EndFrameCallback callback) {
  ZoneScoped;
  ASSERT(!g_inOffscreen, "end_frame called while offscreen rendering is active");
  ASSERT(g_currentRenderPass == UINT32_MAX, "end_frame called before finish finalized the current render pass");
  if (g_cpuFrameStart.time_since_epoch().count() != 0) {
    const auto cpuFrameTime = PresentClock::now() - g_cpuFrameStart;
    update_ema(g_cpuFrameTimeNs, duration_ns(cpuFrameTime));
    const double cpuFrameTimeMs = std::chrono::duration<double, std::milli>{cpuFrameTime}.count();
    TracyPlot("aurora: cpuFrameTimeMs", cpuFrameTimeMs);
  }
  auto& frame = current_frame_packet();
  frame.stats.drawCallCount = g_drawCallCount;
  frame.stats.mergedDrawCallCount = g_mergedDrawCallCount;
  frame.stats.lastVertSize = frame.verts.size();
  frame.stats.lastUniformSize = frame.uniforms.size();
  frame.stats.lastIndexSize = frame.indices.size();
  frame.stats.lastStorageSize = frame.storage.size();
  frame.stats.lastTextureUploadSize = frame.textureUpload.size();

  const size_t frameSlot = g_recordingFrameSlot;
  const uint64_t frameId = frame.frameId;
  g_currentRenderPass = UINT32_MAX;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  gx::sweep_idle_textures();
  destroy_retired_textures();
  end_pipeline_frame();
  ++g_frameIndex;
  g_recordingFrame = nullptr;

#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (!g_debugGroupStack.empty()) {
    for (auto& it : std::ranges::reverse_view(g_debugGroupStack)) {
      Log.warn("Debug group was not popped at end of frame: {}", it);
    }
    g_debugGroupStack.clear();
  }

  if (g_debugMarkers.size() > 0) {
    g_debugMarkers.clear();
  }
#endif

  const size_t stagingSlot = frame.stagingBuffer;
  render_worker::enqueue_end_frame(frameId, [frameSlot, stagingSlot, callback = std::move(callback)]() mutable {
    auto& packet = g_framePackets[frameSlot];
#ifdef __EMSCRIPTEN__
    {
      // This frame's passes bound the current buffers: its bytes go there.
      // Bytes past a buffer's end belong to draws whose ranges it cannot
      // hold; WebGPU drops that one frame (a validation error, not fatal on
      // the web), and a buffer more than half full is replaced by a larger
      // one for later frames.
      const auto upload = [&](ByteBuffer& host, WebRegion region, const wgpu::Buffer& dst) {
        auto& state = g_webRegions[region];
        const uint64_t needed = AURORA_ALIGN(host.size(), 4);
        if (needed > host.size()) {
          host.append_zeroes(needed - host.size());
        }
        const uint64_t used = std::min<uint64_t>(needed, state.capacity);
        if (used > 0) {
          g_queue.WriteBuffer(dst, 0, host.data(), used);
        }
        if (needed > state.capacity / 2) {
          grow_web_buffer(region, needed);
        }
        if (state.host.data() == nullptr) {
          state.host = std::move(host);
        }
      };
      upload(packet.verts, WebVerts, g_vertexBuffer);
      upload(packet.uniforms, WebUniforms, g_uniformBuffer);
      upload(packet.indices, WebIndices, g_indexBuffer);
      upload(packet.storage, WebStorage, g_storageBuffer);
      // Texture uploads were written with Queue::WriteTexture when queued.
    }
#else
    g_stagingBuffers[stagingSlot].Unmap();
#endif
    s_mappingStates[stagingSlot].store(BufferMapState::Unmapped, std::memory_order_release);
    auto encoder = std::move(packet.encoder);
    const auto stats = packet.stats;
    recycle_frame_packet(packet);
    publish_completed_stats(stats);
    if (callback) {
      callback(encoder);
    }
    g_frameSlots.release(frameSlot);
    render_worker::notify_progress();
    expire_cached_bind_groups();
    map_staging_buffer(stagingSlot, true);
    process_events();
  });
}

uint32_t current_frame() noexcept { return g_frameIndex; }

static void expire_cached_bind_groups() {
  std::lock_guard lock{g_bindGroupCacheMutex};
  const auto frameIndex = g_frameIndex.load(std::memory_order_relaxed);
  if (g_cachedBindGroups.empty() || frameIndex == UINT32_MAX || frameIndex % BindGroupCacheSweepPeriod != 0) {
    return;
  }

  ZoneScoped;
  for (auto it = g_cachedBindGroups.begin(); it != g_cachedBindGroups.end();) {
    if (frameIndex - it->second.lastUsedFrame > BindGroupCacheRetainFrames) {
      g_cachedBindGroups.erase(it++);
    } else {
      ++it;
    }
  }
}

static constexpr uint64_t VertexStagingOffset = 0;
static constexpr uint64_t UniformStagingOffset = VertexStagingOffset + VertexBufferSize;
static constexpr uint64_t IndexStagingOffset = UniformStagingOffset + UniformBufferSize;
static constexpr uint64_t StorageStagingOffset = IndexStagingOffset + IndexBufferSize;
#ifdef __EMSCRIPTEN__
static constexpr uint64_t TextureUploadStagingOffset = 0; // the web's staging holds only texture uploads
#else
static constexpr uint64_t TextureUploadStagingOffset = StorageStagingOffset + StorageBufferSize;
#endif

static constexpr uint32_t align_down_copy_offset(uint32_t value) noexcept { return value & ~3u; }

static void copy_staging_buffer_range(wgpu::CommandEncoder& cmd, const FramePacket& frame, uint32_t& copied,
                                      uint32_t highWater, uint64_t stagingOffset, const wgpu::Buffer& dst) {
  if (highWater <= copied) {
    return;
  }
#ifdef __EMSCRIPTEN__
  // Already written to dst by Queue::WriteBuffer before the frame's commands.
  (void)cmd, (void)frame, (void)stagingOffset, (void)dst;
  copied = std::max(copied, highWater);
  return;
#endif
  const uint32_t copyStart = align_down_copy_offset(copied);
  const uint32_t copyEnd = AURORA_ALIGN(highWater, 4);
  cmd.CopyBufferToBuffer(g_stagingBuffers[frame.stagingBuffer], stagingOffset + copyStart, dst, copyStart,
                         copyEnd - copyStart);
  copied = highWater;
}

static bool needs_staging_copy(const FramePacket& frame, const FrameOp& op) {
  const auto& highWater = op.highWater;
  if (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage) {
    return true;
  }
  if constexpr (UseTextureBuffer) {
    return highWater.textureUploadCount > frame.copied.textureUploadCount;
  }
  return false;
}

static void copy_staging_to_high_water(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  if (!needs_staging_copy(frame, op)) {
    return;
  }
  const webgpu::gpu_prof::Zone zone{cmd, "Staging copies"};
  const auto& highWater = op.highWater;
  copy_staging_buffer_range(cmd, frame, frame.copied.verts, highWater.verts, VertexStagingOffset, g_vertexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, highWater.uniforms, UniformStagingOffset,
                            g_uniformBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.indices, highWater.indices, IndexStagingOffset, g_indexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.storage, highWater.storage, StorageStagingOffset, g_storageBuffer);

  if constexpr (UseTextureBuffer) {
    for (const auto* upload : op.textureUploads) {
      const auto& item = *upload;
      const wgpu::TexelCopyBufferInfo buf{
          .layout =
              wgpu::TexelCopyBufferLayout{
                  .offset = item.buffer ? item.layout.offset : item.layout.offset + TextureUploadStagingOffset,
                  .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                  .rowsPerImage = item.layout.rowsPerImage,
              },
          .buffer = item.buffer ? item.buffer : g_stagingBuffers[frame.stagingBuffer],
      };
      cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
    }
    frame.copied.textureUpload = highWater.textureUpload;
    frame.copied.textureUploadCount = highWater.textureUploadCount;
  }
}

static void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  copy_staging_to_high_water(cmd, frame, op);
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(cmd, frame, *op.renderPass, op.index);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      const webgpu::gpu_prof::Zone zone{cmd, "Texture copy"};
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
    break;
  case FrameOpType::EncoderTask:
    if (op.encoderTask != nullptr) {
      execute_encoder_task(cmd, *op.encoderTask);
    }
    break;
  }
}

static wgpu::StoreOp depth_store_op(const RenderPass& pass, bool captureDepthSnapshot) {
  // The next frame clears EFB depth. Only completed-frame depth readers need
  // its final store; intermediate AO/task/continuation dependencies still do.
  if (pass.finalEfb && !captureDepthSnapshot && !pass.snapshotDepthDst &&
      (!pass.resolveTarget || !gx::is_depth_format(pass.resolveFormat))) {
    return wgpu::StoreOp::Discard;
  }
  return pass.depthStoreOp;
}

static void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex) {
  ZoneScoped;
  if (!passInfo.sealed) {
    return;
  }

  for (const auto& conv : passInfo.paletteConvs) {
    tex_palette_conv::run(cmd, conv);
  }
  if (passInfo.discardable) {
    // This pass has no effect and can be safely discarded (e.g. an empty EFB segment between two back-to-back pass
    // breaks, or an unresolved offscreen pass).
    return;
  }
  // [MP6] 0008: a hidden frame is never presented, so a pass whose output
  // nothing else reads (no GX copy, snapshot, export or depth peek) is dropped.
  if (frame.hidden && !passInfo.has_consumer() && !passInfo.captureDepthSnapshot) {
    return;
  }

  // Decide before encoding the attachment store. A request arriving after
  // this check remains pending for the next frame, never reads discarded depth.
  const bool captureDepthSnapshot = passInfo.captureDepthSnapshot && depth_peek::needs_frame_snapshot();

  // [MP6] aurora-patches/0020: an empty back-to-back-copy segment contributes
  // nothing to the EFB and its resolve target is already current, so encoding
  // the render pass would only cost a store + reload of the multisampled
  // color+depth attachment and a redundant resolve. Fall straight through to
  // the copy. Never set when msaaSamples == 1.
  if (!passInfo.skipRenderPass) {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = passInfo.colorView,
            .resolveTarget = passInfo.deferColorResolve ? nullptr : passInfo.resolveView,
            .loadOp = passInfo.colorLoadOp != wgpu::LoadOp::Undefined
                          ? passInfo.colorLoadOp
                          : (passInfo.clearColor ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
            .storeOp = passInfo.colorStoreOp,
            .clearValue =
                {
                    .r = passInfo.clearColorValue.x(),
                    .g = passInfo.clearColorValue.y(),
                    .b = passInfo.clearColorValue.z(),
                    .a = passInfo.clearColorValue.w(),
                },
        },
    };
    wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
    const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
    if (passInfo.depthStencilView) {
      depthStencilAttachment = {
          .view = passInfo.depthStencilView,
          .depthLoadOp = passInfo.hasDepth && !passInfo.depthReadOnly ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                  ? passInfo.depthLoadOp
                                                  : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                           : wgpu::LoadOp::Undefined,
          .depthStoreOp = passInfo.hasDepth && !passInfo.depthReadOnly
                              ? depth_store_op(passInfo, captureDepthSnapshot) : wgpu::StoreOp::Undefined,
          .depthClearValue = passInfo.clearDepthValue,
          .depthReadOnly = passInfo.depthReadOnly,
          .stencilLoadOp = passInfo.hasStencil && !passInfo.depthReadOnly ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
          .stencilStoreOp = passInfo.hasStencil && !passInfo.depthReadOnly ? passInfo.stencilStoreOp : wgpu::StoreOp::Undefined,
          .stencilClearValue = passInfo.stencilClearValue,
          .stencilReadOnly = passInfo.depthReadOnly,
      };
      depthStencilAttachmentPtr = &depthStencilAttachment;
    }
    const auto label = passInfo.label.empty() ? fmt::format("Render pass {}", passIndex)
                                              : fmt::format("{} {}", passInfo.label, passIndex);
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = label.c_str(),
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
        .depthStencilAttachment = depthStencilAttachmentPtr,
        .timestampWrites = webgpu::gpu_prof::pass_writes(passInfo.label.empty() ? "Scene" : passInfo.label),
    };

    auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
    render_pass(pass, frame, passInfo);
    pass.End();
  }

  if (captureDepthSnapshot) {
    depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView, passInfo.targetSize, passInfo.msaaSamples);
  }

  if (passInfo.resolveTarget) {
    const auto& dstSize = passInfo.resolveTarget->size;
    const bool needsConversion = tex_copy_conv::needs_conversion(passInfo.resolveFormat);
    const bool needsScaling = dstSize.width != static_cast<uint32_t>(passInfo.resolveRect.width) ||
                              dstSize.height != static_cast<uint32_t>(passInfo.resolveRect.height);
    const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
    const tex_copy_conv::ConvRequest convReq{
        .fmt = passInfo.resolveFormat,
        .srcView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView,
        .uniformRange = passInfo.resolveUniformRange,
        .dst = passInfo.resolveTarget,
        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,
    };
    if (needsConversion) {
      tex_copy_conv::run(cmd, convReq);
    } else if (needsScaling) {
      tex_copy_conv::blit(cmd, convReq);
    } else {
      const webgpu::gpu_prof::Zone zone{cmd, "EFB copy"};
      const wgpu::TexelCopyTextureInfo src{
          .texture = passInfo.copySourceTexture,
          .origin =
              wgpu::Origin3D{
                  .x = static_cast<uint32_t>(passInfo.resolveRect.x),
                  .y = static_cast<uint32_t>(passInfo.resolveRect.y),
              },
      };
      const wgpu::TexelCopyTextureInfo dst{
          .texture = passInfo.resolveTarget->texture,
      };
      const wgpu::Extent3D size{
          .width = static_cast<uint32_t>(passInfo.resolveRect.width),
          .height = static_cast<uint32_t>(passInfo.resolveRect.height),
          .depthOrArrayLayers = 1,
      };
      cmd.CopyTextureToTexture(&src, &dst, &size);
    }
    if (passInfo.resolveTarget->mipCount > 1) {
      // Renderer-generated mip chain (GX_AURORA_SET_COPY_MIP_GEN): refresh
      // levels 1..N from the just-resolved level 0. Single-level targets (the
      // default) never reach this.
      tex_copy_conv::generate_mips(cmd, *passInfo.resolveTarget);
    }
  }

  if (passInfo.snapshotColorDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Pass snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotColorDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.targetSize.width,
        .height = passInfo.targetSize.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
  if (passInfo.snapshotDepthDst) {
    tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, passInfo.snapshotDepthDst);
  }
}

void after_submit() noexcept { depth_peek::after_submit(); }

void gpu_synchronize() { render_worker::synchronize(); }

void synchronize() { render_worker::synchronize(); }

void after_present() noexcept {
  const auto now = PresentClock::now();
  const int64_t nowNs = timestamp_ns(now);
  const int64_t previousPresentNs = g_lastPresentNs.exchange(nowNs, std::memory_order_acq_rel);
  if (previousPresentNs != 0) {
    update_ema(g_presentPeriodNs, nowNs - previousPresentNs);
    const double presentPeriodMs = static_cast<double>(g_presentPeriodNs.load(std::memory_order_acquire)) / 1'000'000.0;
    TracyPlot("aurora: presentPeriodMs", presentPeriodMs);
  }
  std::lock_guard lock{g_presentStatsMutex};
  g_presentTimes.push_back(now);
  prune_present_times(now);
}

float calculate_fps() noexcept {
  const auto now = PresentClock::now();
  std::lock_guard lock{g_presentStatsMutex};
  prune_present_times(now);
  if (g_presentTimes.size() < 2) {
    return 0.f;
  }

  const auto elapsed = std::chrono::duration<float>(g_presentTimes.back() - g_presentTimes.front()).count();
  if (elapsed <= 0.f) {
    return 0.f;
  }
  return static_cast<float>(g_presentTimes.size() - 1) / elapsed;
}

static void apply_viewport(const wgpu::RenderPassEncoder& pass, const Viewport& vp, const wgpu::Extent3D& size) {
  const float minDepth = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
  const float maxDepth = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
  Viewport clipped = vp;
#ifdef __EMSCRIPTEN__
  // [MP6] Safari still enforces the older WebGPU rule that the viewport lies
  // inside the attachments, and one call outside it invalidates the whole
  // command encoder (every later frame is dropped: the board's pause screen,
  // whose 640x481 viewport overhangs a 480-line target, froze the picture).
  // Chrome allows the overhang. Clip it to the target on the web; the GX
  // uniform writers fold the same clip into the projection
  // (gx::viewport_clip_projection), so the picture keeps its place and size.
  clip_viewport_to_target(clipped, static_cast<float>(size.width), static_cast<float>(size.height));
#else
  (void)size;
#endif
  pass.SetViewport(clipped.left, clipped.top, clipped.width, clipped.height, minDepth, maxDepth);
}

static void apply_scissor(const wgpu::RenderPassEncoder& pass, const ClipRect& sc, const wgpu::Extent3D& size) {
  const auto x = std::clamp(static_cast<uint32_t>(sc.x), 0u, size.width);
  const auto y = std::clamp(static_cast<uint32_t>(sc.y), 0u, size.height);
  const auto w = std::clamp(static_cast<uint32_t>(sc.width), 0u, size.width - x);
  const auto h = std::clamp(static_cast<uint32_t>(sc.height), 0u, size.height - y);
  pass.SetScissorRect(x, y, w, h);
}

static DrawContext make_draw_context(const RenderPass& passInfo) {
  return {
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = g_vertexBuffer,
      .indexBuffer = g_indexBuffer,
      .uniformBuffer = g_uniformBuffer,
      .storageBuffer = g_storageBuffer,
      .colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format,
      .depthFormat = webgpu::g_graphicsConfig.depthFormat,
      .sampleCount = passInfo.msaaSamples,
      .targetWidth = passInfo.targetSize.width,
      .targetHeight = passInfo.targetSize.height,
  };
}

static void render_custom_draw(const CustomDrawCommand& draw, const wgpu::RenderPassEncoder& pass,
                               const RenderPass& passInfo) {
  DrawCallback callback = nullptr;
  void* userdata = nullptr;
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    const auto* slot = find_runtime_draw_type(draw.type);
    if (slot == nullptr) {
      // Unregistered between record and replay; the command is a no-op.
      return;
    }
    callback = slot->draw;
    userdata = slot->userdata;
  }

  const auto context = make_draw_context(passInfo);
  callback(context, pass, draw.payload.data(), draw.payloadSize, userdata);
}

static void execute_encoder_task(wgpu::CommandEncoder& cmd, const EncoderTask& task) {
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    const auto* slot = find_runtime_encoder_task_type(task.type);
    if (slot == nullptr) {
      // Unregistered between record and encode; the task is a no-op.
      return;
    }
    callback = slot->callback;
    userdata = slot->userdata;
  }

  const EncoderTaskContext context{
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = g_vertexBuffer,
      .indexBuffer = g_indexBuffer,
      .uniformBuffer = g_uniformBuffer,
      .storageBuffer = g_storageBuffer,
      .depth = task.depth,
      .targetWidth = task.targetSize.width,
      .targetHeight = task.targetSize.height,
      .sampleCount = task.sampleCount,
  };
  callback(context, cmd, task.payload.data(), task.payloadSize, userdata);
}

static void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, const RenderPass& passInfo) {
  ZoneScoped;
  g_currentPipeline = UINTPTR_MAX;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif
  Viewport currentViewport{};
  ClipRect currentScissor{};
  bool hasViewport = false;
  bool hasScissor = false;
  gx::RenderState gxState{};
#ifdef AURORA_ENABLE_RMLUI
  rmlui::RmlPassBindingState rmlState{};
  // A successful Rml draw owns group 0 (and possibly group 2) until the
  // first following GX/custom transition restores the pass defaults. Clear
  // does not touch bind groups, so this remains true across Clear and also
  // covers a later Rml bind failure after an earlier successful Rml draw.
  bool rmlBindingsActive = false;
  const auto restore_non_rml_bindings = [&] {
    if (!rmlBindingsActive) {
      return;
    }
    pass.SetBindGroup(0, g_staticBindGroup);
    pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
    rmlBindingsActive = false;
  };
#endif

  // Bind static bind group for the whole pass
  pass.SetBindGroup(0, g_staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  for (const auto& cmd : passInfo.commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      apply_viewport(pass, vp, passInfo.targetSize);
      currentViewport = vp;
      hasViewport = true;
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      apply_scissor(pass, sc, passInfo.targetSize);
      currentScissor = sc;
      hasScissor = true;
    } break;
    case CommandType::Draw: {
      const auto& draw = cmd.data.draw;
      switch (draw.type) {
      case ShaderType::Clear:
        gxState = {};
#ifdef AURORA_ENABLE_RMLUI
        rmlState.invalidate();
#endif
        clear::render(draw.clear, pass, passInfo.targetSize);
        break;
      case ShaderType::GX:
#ifdef AURORA_ENABLE_RMLUI
        restore_non_rml_bindings();
        rmlState.invalidate();
#endif
        gx::render(draw.gx, pass, gxState);
        break;
#ifdef AURORA_ENABLE_RMLUI
      case ShaderType::Rml:
        gxState = {};
        rmlui::render(draw.rml, pass, rmlState);
        if (rmlState.pipelineValid) {
          rmlBindingsActive = true;
        }
        break;
#endif
      }
    } break;
    case CommandType::CustomDraw: {
      render_custom_draw(cmd.data.customDraw, pass, passInfo);
      g_currentPipeline = UINTPTR_MAX;
      gxState = {};
#ifdef AURORA_ENABLE_RMLUI
      rmlState.invalidate();
      rmlBindingsActive = false;
#endif
      pass.SetBindGroup(0, g_staticBindGroup);
      pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      if (hasViewport) {
        apply_viewport(pass, currentViewport, passInfo.targetSize);
      }
      if (hasScissor) {
        apply_scissor(pass, currentScissor, passInfo.targetSize);
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(g_debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

void render_pass(const wgpu::RenderPassEncoder& pass, u32 idx) {
  auto& frame = current_frame_packet();
  render_pass(pass, frame, frame.renderPasses[idx]);
}

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass) {
  if (ref == g_currentPipeline) {
    return true;
  }
  wgpu::RenderPipeline pipeline;
  if (!get_pipeline(ref, pipeline)) {
    return false;
  }
  pass.SetPipeline(pipeline);
  g_currentPipeline = ref;
  return true;
}

static Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  const auto begin = target.size();
  if (length > 0) {
    target.append(data, length);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}

static Range map(ByteBuffer& target, size_t length, size_t alignment) {
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  auto begin = target.size();
  if (length > 0) {
    target.append_zeroes(length);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}

// For our public API, warn instead of fatal-ing when called outside an active recording frame.
static bool check_recording(const char* name) {
  if (g_recordingFrame == nullptr)
    UNLIKELY {
      Log.warn("{}: called outside an active frame", name);
      return false;
    }
  return true;
}

void retain_frame_resource(std::shared_ptr<const void> resource) {
  if (!resource || !check_recording("retain_frame_resource")) {
    return;
  }
  // This vector is appended only while recording. The worker never reads it;
  // recycle_frame_packet clears it after all queued encoding operations have
  // completed, keeping raw pointers embedded in DrawData valid until then.
  if (g_retainRecording) g_retainTainted = true;
  current_frame_packet().retainedFrameResources.emplace_back(std::move(resource));
}

Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_verts")) {
    return {};
  }
  return push(current_frame_packet().verts, data, length, alignment);
}

IndexReservation reserve_indices(size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("reserve_indices")) {
    return {};
  }
  auto& target = current_frame_packet().indices;
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  const auto begin = target.size();
  auto* data = target.append_uninitialized(length);
  return {{static_cast<uint32_t>(begin), static_cast<uint32_t>(length)}, data};
}

Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_indices")) {
    return {};
  }
  return push(current_frame_packet().indices, data, length, alignment);
}

UniformWriter begin_uniform(uint32_t capacity) {
  if (!check_recording("begin_uniform")) return {};
  return UniformWriter(current_frame_packet().uniforms, capacity,
                       g_cachedLimits.minUniformBufferOffsetAlignment);
}

Range push_uniform(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_uniform")) {
    return {};
  }
  return push(current_frame_packet().uniforms, data, length, g_cachedLimits.minUniformBufferOffsetAlignment);
}

Range push_storage(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_storage")) {
    return {};
  }
  return push(current_frame_packet().storage, data, length, g_cachedLimits.minStorageBufferOffsetAlignment);
}

Range push_texture_data(const uint8_t* data, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(current_frame_packet().textureUpload, copyBytesPerRow * rowsPerImage, 0);
  u8* dst = current_frame_packet().textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  std::lock_guard lock{g_bindGroupCacheMutex};
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    auto bg = wgpu::BindGroup::Acquire(wgpuDeviceCreateBindGroup(g_device.Get(), &descriptor));
    [[maybe_unused]] auto& cached = g_cachedBindGroups.emplace(id, CachedBindGroup{
                                       .bindGroup = std::move(bg),
                                       .lastUsedFrame = g_frameIndex,
                                   }).first->second;
#ifdef __EMSCRIPTEN__
    hold_bind_group_entries(cached, descriptor);
#endif
  } else {
    it->second.lastUsedFrame = g_frameIndex;
  }
  return id;
}

wgpu::BindGroup find_bind_group(BindGroupRef id) {
  std::lock_guard lock{g_bindGroupCacheMutex};
  const auto it = g_cachedBindGroups.find(id);
  CHECK(it != g_cachedBindGroups.end(), "get_bind_group: failed to locate {:x}", id);
  return it->second.bindGroup;
}

wgpu::BindGroup cached_bind_group(const wgpu::BindGroupDescriptor& descriptor) {
  const auto& native = *reinterpret_cast<const WGPUBindGroupDescriptor*>(&descriptor);
  const auto id = xxh3_hash(native);
  std::lock_guard lock{g_bindGroupCacheMutex};
  auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    it = g_cachedBindGroups.emplace(id, CachedBindGroup{
        .bindGroup = g_device.CreateBindGroup(&descriptor), .lastUsedFrame = g_frameIndex.load(),
    }).first;
#ifdef __EMSCRIPTEN__
    hold_bind_group_entries(it->second, native);
#endif
  } else {
    it->second.lastUsedFrame = g_frameIndex.load();
  }
  return it->second.bindGroup;
}

wgpu::Sampler sampler_ref(const wgpu::SamplerDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  std::lock_guard lock{g_samplerCacheMutex};
  auto it = g_cachedSamplers.find(id);
  if (it == g_cachedSamplers.end()) {
    it = g_cachedSamplers.try_emplace(id, g_device.CreateSampler(&descriptor)).first;
  }
  return it->second;
}

uint32_t align_uniform(uint32_t value) { return AURORA_ALIGN(value, g_cachedLimits.minUniformBufferOffsetAlignment); }

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto idx = g_debugMarkers.size();
  g_debugMarkers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

} // namespace aurora::gfx

void aurora::gfx::push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_debugGroupStack.push_back(std::move(label));
#endif
}
void push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_debugGroupStack.emplace_back(label);
#endif
}
void pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_debugGroupStack.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_debugGroupStack.pop_back();
#endif
}

const AuroraStats* aurora_get_stats() {
  thread_local AuroraStats snapshot{};
  snapshot = aurora::gfx::read_completed_stats();
  return &snapshot;
}
float aurora_get_fps() { return aurora::gfx::calculate_fps(); }
