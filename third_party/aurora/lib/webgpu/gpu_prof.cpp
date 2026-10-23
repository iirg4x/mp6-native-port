#include "gpu_prof.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Safari (WebKit) advertises timestamp-query, but a frame that resolves pass
// timestamps came back "encoder state is not valid" and killed the game
// within seconds on an iPhone. Off there unless the page sets
// Module.mp6GpuTimestamps (?gputimestamps=1) to retry it.
EM_JS(int, aurora_web_timestamps_blocked, (), {
  const ua = navigator.userAgent || '';
  const webkit = ua.indexOf('AppleWebKit') >= 0 && ua.indexOf('Chrome') < 0 && ua.indexOf('Edg/') < 0;
  return webkit && !Module.mp6GpuTimestamps ? 1 : 0;
});
#endif

#include "../internal.hpp"
#include "gpu.hpp"
#include <aurora/aurora.h>
#include <aurora/gpu_timing_snapshot.h>

#include <tracy/Tracy.hpp>

#ifdef TRACY_ENABLE

#include <tracy/TracyC.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

namespace aurora::webgpu::gpu_prof {
namespace {
Module Log("aurora::webgpu::gpu_prof");

// Each zone consumes a begin/end timestamp pair.
// The last pair is reserved for the frame zone.
constexpr uint32_t MaxZones = 127;
constexpr uint32_t QueryCount = MaxZones * 2 + 2;
constexpr uint32_t FrameBeginQuery = MaxZones * 2;
constexpr uint32_t FrameEndQuery = MaxZones * 2 + 1;
constexpr uint64_t ReadbackSize = QueryCount * sizeof(uint64_t);
constexpr size_t RingDepth = 4;
constexpr uint8_t ContextId = 0;
constexpr uint64_t SaneFrameGapNs = UINT64_C(5'000'000'000);

enum class EventKind : uint8_t {
  ZoneBegin,
  PassBegin,
  End,
};

struct Event {
  const char* name; // static lifetime; nullptr for End
  uint32_t query;
  EventKind kind;
};

enum class SlotState : uint8_t {
  Free,
  Recording,
  InFlight,
  Mapped,
  Failed,
};

struct Slot {
  wgpu::Buffer readback;
  std::vector<Event> events;
  uint32_t passCount = 0;
  int64_t submitNs = 0;
  std::atomic<SlotState> state{SlotState::Free};
};

struct TimestampBounds {
  uint64_t begin = 0;
  uint64_t end = 0;

  bool valid() const { return begin != 0 && end > begin; }
};

bool g_enabled = false;
bool g_timestampsEnabled = false;
wgpu::QuerySet g_querySet;
wgpu::Buffer g_resolveBuffer;
std::array<Slot, RingDepth> g_slots;
size_t g_recordSlot = 0;
size_t g_emitSlot = 0;
bool g_frameActive = false;
bool g_framePending = false;
uint32_t g_zoneCount = 0;

bool g_contextEmitted = false;
uint16_t g_queryId = 0;
uint64_t g_lastEmittedTs = 0;
uint64_t g_lastFrameEnd = 0;

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Tracy keeps references to zone names; intern them for static lifetime.
const char* intern_name(std::string_view name) {
  static absl::flat_hash_map<std::string, const char*> names;
  const auto it = names.find(name);
  if (it != names.end()) {
    return it->second;
  }
  char* stable = new char[name.size() + 1];
  std::memcpy(stable, name.data(), name.size());
  stable[name.size()] = '\0';
  names.emplace(name, stable);
  return stable;
}

// tracy::GpuContextType not exposed through TracyC.h
uint8_t tracy_context_type(wgpu::BackendType backend) {
  switch (backend) {
  case wgpu::BackendType::OpenGL:
  case wgpu::BackendType::OpenGLES:
    return 1; // OpenGl
  case wgpu::BackendType::Vulkan:
    return 2; // Vulkan
  case wgpu::BackendType::D3D12:
    return 4; // Direct3D12
  case wgpu::BackendType::D3D11:
    return 5; // Direct3D11
  case wgpu::BackendType::Metal:
    return 6; // Metal
  default:
    return 7; // Custom
  }
}

void emit_context(const Slot& slot, uint64_t frameBegin) {
  // Tracy has no notion of WebGPU's opaque timestamp epoch, so the context
  // anchor pairs a GPU timestamp with "now" at emission. Shift the timestamp
  // by the time elapsed since this frame was submitted, so the frame zone
  // lands at the submit point on the timeline instead of trailing it by the
  // readback latency. Residual error is the GPU's submit-to-execute delay.
  const int64_t anchor = static_cast<int64_t>(frameBegin) + (now_ns() - slot.submitNs);
  ___tracy_emit_gpu_new_context_serial({
      .gpuTime = anchor,
      .period = 1.0f,
      .context = ContextId,
      .flags = 0,
      .type = tracy_context_type(g_backendType),
  });
  const std::string name =
      fmt::format("{} ({})", std::string_view{g_adapterInfo.device}, magic_enum::enum_name(g_backendType));
  ___tracy_emit_gpu_context_name_serial({
      .context = ContextId,
      .name = name.c_str(),
      .len = static_cast<uint16_t>(std::min<size_t>(name.size(), UINT16_MAX)),
  });
}

void emit_zone_begin(const char* name, uint64_t gpuNs) {
  const uint64_t srcloc = ___tracy_alloc_srcloc_name(0, "aurora", 6, "gpu_prof", 8, name, std::strlen(name), 0);
  const uint16_t queryId = g_queryId++;
  ___tracy_emit_gpu_zone_begin_alloc_serial({.srcloc = srcloc, .queryId = queryId, .context = ContextId});
  ___tracy_emit_gpu_time_serial({.gpuTime = int64_t(gpuNs), .queryId = queryId, .context = ContextId});
}

void emit_zone_end(uint64_t gpuNs) {
  const uint16_t queryId = g_queryId++;
  ___tracy_emit_gpu_zone_end_serial({.queryId = queryId, .context = ContextId});
  ___tracy_emit_gpu_time_serial({.gpuTime = int64_t(gpuNs), .queryId = queryId, .context = ContextId});
}

TimestampBounds event_bounds(const Slot& slot, const uint64_t* ts) {
  TimestampBounds bounds;
  for (const auto& event : slot.events) {
    const uint64_t t = ts[event.query];
    if (t == 0) {
      continue;
    }
    if (bounds.begin == 0 || t < bounds.begin) {
      bounds.begin = t;
    }
    if (t > bounds.end) {
      bounds.end = t;
    }
  }
  return bounds;
}

void emit_frame(Slot& slot) {
  const auto* ts = static_cast<const uint64_t*>(slot.readback.GetConstMappedRange(0, ReadbackSize));
  if (ts == nullptr) {
    return;
  }

  // Prefer recorded work bounds. Some backends expose encoder timestamps but
  // report startup/epoch values that are not stable enough to anchor Tracy's
  // GPU context, while pass timestamps are already what the emitted zones use.
  const TimestampBounds zoneBounds = event_bounds(slot, ts);
  uint64_t frameBegin = zoneBounds.begin;
  uint64_t frameEnd = zoneBounds.end;
  if (!zoneBounds.valid() && g_timestampsEnabled) {
    frameBegin = ts[FrameBeginQuery];
    frameEnd = ts[FrameEndQuery];
  }
  if (frameBegin == 0 || frameEnd <= frameBegin) {
    return;
  }

  const uint64_t lastFrameEnd = std::exchange(g_lastFrameEnd, frameEnd);
  if (lastFrameEnd != 0) {
    if (frameBegin < lastFrameEnd) {
      if (frameEnd <= lastFrameEnd || frameEnd - lastFrameEnd >= SaneFrameGapNs) {
        return;
      }
      // Some Dawn/Metal timestamp query begins can remain pinned to an old
      // value while end timestamps advance normally. Use consecutive frame
      // ends to keep Tracy's timeline monotonic in that case.
      frameBegin = lastFrameEnd;
    } else if (frameBegin - lastFrameEnd >= SaneFrameGapNs) {
      return;
    }
  }

  if (!TracyIsConnected) {
    return;
  }

  if (!g_contextEmitted) {
    if (lastFrameEnd == 0) {
      return;
    }
    emit_context(slot, frameBegin);
    g_contextEmitted = true;
  }

  // Tracy requires GPU zones within a context to be properly nested in
  // time, and treats large backward jumps as timer wraparound. The recorded
  // events mirror encode order, which matches GPU execution order. Clamp
  // to keep the stream monotonic.
  uint64_t prev = std::max(g_lastEmittedTs, frameBegin);
  const uint64_t endBound = std::max(frameEnd, prev);
  const auto clamped = [&prev, endBound](uint64_t t) {
    prev = std::min(std::max(t, prev), endBound);
    return prev;
  };

  emit_zone_begin(intern_name("Frame"), clamped(frameBegin));
  uint32_t depth = 0;
  uint64_t topLevelEnd = prev;
  uint64_t idleNs = 0;
  // Zones whose timestamps were never written resolve to 0 (e.g. Metal
  // cannot sample encoder-level timestamps); drop those to keep the stream
  // balanced.
  std::bitset<MaxZones> dropped;
  for (const auto& event : slot.events) {
    if (event.kind == EventKind::End) {
      if (dropped[event.query / 2]) {
        continue;
      }
      const uint64_t t = clamped(ts[event.query]);
      emit_zone_end(t);
      if (--depth == 0) {
        topLevelEnd = t;
      }
    } else {
      if (ts[event.query] == 0 && ts[event.query + 1] == 0) {
        dropped[event.query / 2] = true;
        continue;
      }
      const uint64_t t = clamped(ts[event.query]);
      if (depth++ == 0 && t > topLevelEnd) {
        idleNs += t - topLevelEnd;
      }
      emit_zone_begin(event.name, t);
    }
  }
  const uint64_t end = clamped(frameEnd);
  if (end > topLevelEnd) {
    idleNs += end - topLevelEnd;
  }
  emit_zone_end(end);
  g_lastEmittedTs = prev;

  TracyPlot("aurora: gpuFrameMs", double(frameEnd - frameBegin) * 1e-6);
  TracyPlot("aurora: gpuIdleMs", double(idleNs) * 1e-6);
  TracyPlot("aurora: gpuPasses", int64_t(slot.passCount));
}

Slot& record_slot() { return g_slots[g_recordSlot]; }

uint32_t alloc_zone() {
  if (!g_frameActive || g_zoneCount >= MaxZones) {
    return UINT32_MAX;
  }
  return g_zoneCount++;
}
} // namespace

void initialize() {
  g_enabled = g_device.HasFeature(wgpu::FeatureName::TimestampQuery);
  if (!g_enabled) {
    Log.info("Timestamp queries unsupported; GPU profiling disabled");
    return;
  }
  g_timestampsEnabled = true; // TODO: check if allow_unsafe_apis enabled?
  constexpr wgpu::QuerySetDescriptor querySetDescriptor{
      .label = "GPU profiler timestamps",
      .type = wgpu::QueryType::Timestamp,
      .count = QueryCount,
  };
  g_querySet = g_device.CreateQuerySet(&querySetDescriptor);
  constexpr wgpu::BufferDescriptor resolveDescriptor{
      .label = "GPU profiler resolve",
      .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
      .size = ReadbackSize,
  };
  g_resolveBuffer = g_device.CreateBuffer(&resolveDescriptor);
  constexpr wgpu::BufferDescriptor readbackDescriptor{
      .label = "GPU profiler readback",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = ReadbackSize,
  };
  for (auto& slot : g_slots) {
    slot.readback = g_device.CreateBuffer(&readbackDescriptor);
    slot.events.reserve(MaxZones * 2);
    slot.state = SlotState::Free;
  }
  g_recordSlot = 0;
  g_emitSlot = 0;
  g_framePending = false;
  TracyPlotConfig("aurora: gpuFrameMs", tracy::PlotFormatType::Number, false, true, 0);
  TracyPlotConfig("aurora: gpuIdleMs", tracy::PlotFormatType::Number, false, true, 0);
  TracyPlotConfig("aurora: gpuPasses", tracy::PlotFormatType::Number, true, true, 0);
  Log.info("GPU profiling enabled ({} zones max)", MaxZones);
}

void shutdown() {
  g_querySet = {};
  g_resolveBuffer = {};
  for (auto& slot : g_slots) {
    slot.readback = {};
    slot.events.clear();
    slot.passCount = 0;
    slot.state = SlotState::Free;
  }
  g_enabled = false;
  g_timestampsEnabled = false;
  g_frameActive = false;
  g_framePending = false;
}

void frame_begin(const wgpu::CommandEncoder& encoder) {
  if (!g_enabled) {
    return;
  }
  auto& slot = record_slot();
  if (slot.state != SlotState::Free) {
    g_frameActive = false;
    return;
  }
  slot.state = SlotState::Recording;
  slot.events.clear();
  slot.passCount = 0;
  g_zoneCount = 0;
  g_frameActive = true;
  if (g_timestampsEnabled) {
    encoder.WriteTimestamp(g_querySet, FrameBeginQuery);
  }
}

void frame_end(const wgpu::CommandEncoder& encoder) {
  if (!g_enabled || !g_frameActive) {
    return;
  }
  g_frameActive = false;
  auto& slot = record_slot();
  if (slot.events.empty() && !g_timestampsEnabled) {
    slot.state = SlotState::Free;
    return;
  }
  if (g_timestampsEnabled) {
    encoder.WriteTimestamp(g_querySet, FrameEndQuery);
  }
  encoder.ResolveQuerySet(g_querySet, 0, QueryCount, g_resolveBuffer, 0);
  encoder.CopyBufferToBuffer(g_resolveBuffer, 0, slot.readback, 0, ReadbackSize);
  g_framePending = true;
}

void after_submit() {
  if (!g_enabled) {
    return;
  }
  if (g_framePending) {
    g_framePending = false;
    auto& slot = record_slot();
    slot.submitNs = now_ns();
    slot.state = SlotState::InFlight;
    slot.readback.MapAsync(wgpu::MapMode::Read, 0, ReadbackSize, wgpu::CallbackMode::AllowSpontaneous,
                           [&slot](wgpu::MapAsyncStatus status, wgpu::StringView) {
                             slot.state =
                                 status == wgpu::MapAsyncStatus::Success ? SlotState::Mapped : SlotState::Failed;
                           });
    g_recordSlot = (g_recordSlot + 1) % RingDepth;
  }
  {
    ZoneScopedN("ProcessEvents");
    g_instance.ProcessEvents();
  }
  while (true) {
    auto& slot = g_slots[g_emitSlot];
    const auto state = slot.state.load(std::memory_order_acquire);
    if (state == SlotState::Mapped) {
      emit_frame(slot);
      slot.readback.Unmap();
    } else if (state != SlotState::Failed) {
      break;
    }
    slot.state.store(SlotState::Free, std::memory_order_release);
    g_emitSlot = (g_emitSlot + 1) % RingDepth;
  }
}

const wgpu::PassTimestampWrites* pass_writes(std::string_view name) {
  const uint32_t index = alloc_zone();
  if (index == UINT32_MAX) {
    return nullptr;
  }
  auto& slot = record_slot();
  slot.events.push_back({intern_name(name), index * 2, EventKind::PassBegin});
  slot.events.push_back({nullptr, index * 2 + 1, EventKind::End});
  ++slot.passCount;

  static std::array<wgpu::PassTimestampWrites, 4> writes;
  static size_t writesIndex = 0;
  auto& out = writes[writesIndex++ % writes.size()];
  out = {
      .querySet = g_querySet,
      .beginningOfPassWriteIndex = index * 2,
      .endOfPassWriteIndex = index * 2 + 1,
  };
  return &out;
}

Zone::Zone(const wgpu::CommandEncoder& encoder, std::string_view name) {
  if (!g_timestampsEnabled) {
    return;
  }
  const uint32_t index = alloc_zone();
  if (index == UINT32_MAX) {
    return;
  }
  record_slot().events.push_back({intern_name(name), index * 2, EventKind::ZoneBegin});
  encoder.WriteTimestamp(g_querySet, index * 2);
  m_encoder = &encoder;
  m_endQuery = index * 2 + 1;
}

Zone::~Zone() {
  if (m_encoder == nullptr) {
    return;
  }
  record_slot().events.push_back({nullptr, m_endQuery, EventKind::End});
  m_encoder->WriteTimestamp(g_querySet, m_endQuery);
}

} // namespace aurora::webgpu::gpu_prof

#else

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <aurora/aurora.h>

namespace aurora::webgpu::gpu_prof {
namespace {
// All GPU objects/recording state belong to the render worker. Only requests
// and the published value snapshot cross threads. Never wait for readback.
constexpr uint32_t MaxPasses = 256, QueryCount = MaxPasses * 2;
constexpr uint64_t BufferSize = QueryCount * sizeof(uint64_t);
constexpr size_t RingDepth = 4, HistorySize = 120;
enum State { Pending, Mapped, Failed };
struct Completion { std::atomic<int> state{Pending}; };
struct Slot {
  wgpu::QuerySet queries;
  wgpu::Buffer resolve, readback;
  std::shared_ptr<Completion> completion;
  std::array<std::array<char, 64>, MaxPasses> names{};
  uint32_t count = 0;
  uint64_t generation = 0;
  bool overflow = false;
};
struct Sample {
  double span = 0, between = 0;
  std::array<double, AURORA_GPU_STAT_ROWS> rows{};
};
std::array<Slot, RingDepth> slots;
Slot* recording = nullptr;
Slot* submitted = nullptr;
std::atomic<uint64_t> request{0}; // bit 0 = enabled, upper bits = generation
uint64_t generation = 0;
std::atomic<bool> supported{false};
std::atomic<bool> diagnostic{false};
uint32_t diagnosticFrames = 0;
uint32_t diagnosticWindowDropped = 0;
std::mutex publishMutex;
AuroraGpuStats published{};
uint64_t publishedGeneration = 0;
// Only the pending flag is inspected at idle frame boundaries. All other
// snapshot request/publication state is protected by publishMutex.
std::atomic<uint64_t> snapshotPending{0};
uint64_t snapshotNextId = 0, snapshotWantedId = 0, snapshotWantedGeneration = 0;
AuroraGpuTimingSnapshot publishedSnapshot{};
std::array<Sample, HistorySize> history{};
std::array<std::array<char,64>, AURORA_GPU_STAT_ROWS> names{};
uint32_t samples = 0, cursor = 0, nameCount = 0, dropped = 0;
bool mapFailed = false;

void reset_history(uint64_t current) {
  generation = current;
  samples = cursor = nameCount = dropped = 0;
  diagnosticWindowDropped = 0;
  mapFailed = false;
  history = {};
  names = {};
  std::lock_guard lock(publishMutex);
  publishedSnapshot = {};
  // A newer generation may already have queued a request while this worker
  // was clearing the old ring. Preserve that request until its own boundary.
  if (!supported.load() || snapshotWantedGeneration != request.load()) {
    snapshotPending.store(0);
    snapshotWantedId = snapshotWantedGeneration = 0;
  }
}
uint32_t row_for(const char* name) {
  for (uint32_t i=0;i<nameCount;++i)
    if (std::strcmp(names[i].data(),name)==0) return i;
  if (nameCount < AURORA_GPU_STAT_ROWS-1) {
    std::strncpy(names[nameCount].data(),name,63);
    return nameCount++;
  }
  std::strcpy(names[AURORA_GPU_STAT_ROWS-1].data(),"Other passes");
  nameCount = AURORA_GPU_STAT_ROWS;
  return AURORA_GPU_STAT_ROWS-1;
}

// This is deliberately a report-time operation. Per-frame consume() keeps
// only the existing Sample(history120); no sorting or percentile state is
// maintained on the render path.
double window_percentile(const std::array<double, HistorySize>& values, uint32_t count,
                         bool p95) {
  if (!count) return 0.0;
  std::array<double, HistorySize> ordered{};
  std::copy_n(values.begin(), count, ordered.begin());
  std::sort(ordered.begin(), ordered.begin() + count);
  if (!p95) {
    if (count & 1u) return ordered[count / 2];
    return (ordered[count / 2 - 1] + ordered[count / 2]) * 0.5;
  }
  // Nearest-rank p95: rank is ceil(0.95 * count), one-based.
  const uint32_t rank = std::max<uint32_t>(1, (count * 95 + 99) / 100);
  return ordered[rank - 1];
}
void emit_window_report() {
  const uint32_t count = std::min<uint32_t>(samples, HistorySize);
  const uint32_t windowDropped = dropped - diagnosticWindowDropped;
  const bool complete = count == HistorySize;
  const char* status = windowDropped ? "dropped" : (complete ? "valid" : "incomplete");
  std::array<double, HistorySize> values{};
  for (uint32_t j = 0; j < count; ++j) values[j] = history[j].span;
  const double spanMedian = window_percentile(values, count, false);
  const double spanP95 = window_percentile(values, count, true);
  for (uint32_t j = 0; j < count; ++j) values[j] = history[j].between;
  const double betweenMedian = window_percentile(values, count, false);
  const double betweenP95 = window_percentile(values, count, true);
  std::printf("[MP6-GPU-WINDOW] status=%s count=%u dropped=%u "
              "gpu_span_median=%.3f gpu_span_p95=%.3f "
              "between_median=%.3f between_p95=%.3f "
              "gpu_span_definition=first-to-last-timestamps-not-walltime\n",
              status, count, windowDropped, spanMedian, spanP95, betweenMedian, betweenP95);
  for (uint32_t i = 0; i < nameCount; ++i) {
    for (uint32_t j = 0; j < count; ++j) values[j] = history[j].rows[i];
    std::printf("[MP6-GPU-WINDOW-ROW] stage=\"%s\" median=%.3f p95=%.3f\n",
                names[i].data(), window_percentile(values, count, false),
                window_percentile(values, count, true));
  }
  std::fflush(stdout);
  diagnosticWindowDropped = dropped;
}
bool named_ao_pass(const char* name) {
  return std::strcmp(name, "GTAO") == 0 || std::strncmp(name, "AO ", 3) == 0 ||
         std::strncmp(name, "XeGTAO ", 7) == 0;
}
void cancel_timing_snapshot() {
  std::lock_guard lock(publishMutex);
  snapshotPending.store(0);
  snapshotWantedId = snapshotWantedGeneration = 0;
  publishedSnapshot = {};
}
void publish_timing_snapshot(const AuroraGpuStats& stats) {
  if (!snapshotPending.load(std::memory_order_acquire)) return;
  uint64_t id;
  {
    std::lock_guard lock(publishMutex);
    id = snapshotPending.load();
    if (!id || snapshotWantedGeneration != generation || request.load() != generation ||
        !(generation & 1) || !supported.load()) return;
    // A failed readback or collapsed name inventory cannot describe AO sums.
    if (mapFailed || nameCount == AURORA_GPU_STAT_ROWS) {
      snapshotPending.store(0);
      publishedSnapshot = {};
      return;
    }
  }
  AuroraGpuTimingSnapshot out{};
  out.requestId = id;
  out.generation = generation;
  out.stats = stats;
  const uint32_t count = std::min<uint32_t>(samples, HistorySize);
  std::array<double, HistorySize> values{};
  for (uint32_t j = 0; j < count; ++j) values[j] = history[j].span;
  out.spanMedianMs = window_percentile(values, count, false);
  out.spanP95Ms = window_percentile(values, count, true);
  for (uint32_t j = 0; j < count; ++j) values[j] = history[j].between;
  out.betweenMedianMs = window_percentile(values, count, false);
  out.betweenP95Ms = window_percentile(values, count, true);
  values = {};
  for (uint32_t i = 0; i < nameCount; ++i) {
    if (!named_ao_pass(names[i].data())) continue;
    for (uint32_t j = 0; j < count; ++j) values[j] += history[j].rows[i];
  }
  for (uint32_t j = 0; j < count; ++j) {
    out.aoAverageMs += values[j];
    out.aoMaxMs = std::max(out.aoMaxMs, values[j]);
  }
  if (count) out.aoAverageMs /= count;
  out.aoMedianMs = window_percentile(values, count, false);
  out.aoP95Ms = window_percentile(values, count, true);
  std::lock_guard lock(publishMutex);
  if (snapshotPending.load() != id || snapshotWantedId != id ||
      snapshotWantedGeneration != generation || request.load() != generation ||
      !(generation & 1) || !supported.load()) return;
  publishedSnapshot = out;
  snapshotPending.store(0, std::memory_order_release);
}
void publish() {
  AuroraGpuStats out{};
  out.status = mapFailed ? 3 : (samples ? 2 : 1);
  out.sampleCount = samples;
  out.rowCount = nameCount;
  out.droppedFrames = dropped;
  for (uint32_t i=0;i<nameCount;++i) std::memcpy(out.rows[i].name,names[i].data(),64);
  for (uint32_t j=0;j<samples;++j) {
    const auto& s=history[j];
    out.spanAverageMs += s.span;
    out.spanMaxMs = std::max(out.spanMaxMs,s.span);
    out.betweenAverageMs += s.between;
    out.betweenMaxMs = std::max(out.betweenMaxMs,s.between);
    for (uint32_t i=0;i<nameCount;++i) {
      out.rows[i].averageMs += s.rows[i];
      out.rows[i].maxMs = std::max(out.rows[i].maxMs,s.rows[i]);
    }
  }
  if (samples) {
    out.spanAverageMs /= samples;
    out.betweenAverageMs /= samples;
    for (uint32_t i=0;i<nameCount;++i) out.rows[i].averageMs /= samples;
  }
  std::sort(out.rows,out.rows+nameCount,[](const auto& a,const auto& b) {
    return a.averageMs > b.averageMs;
  });
  publish_timing_snapshot(out);
  std::lock_guard lock(publishMutex);
  published=out;
  publishedGeneration=generation;
}
void consume(Slot& slot) {
  const auto* timestamps=static_cast<const uint64_t*>(slot.readback.GetConstMappedRange(0,BufferSize));
  if (!timestamps) { ++dropped; cancel_timing_snapshot(); return; }
  if (!slot.count || slot.overflow) { ++dropped; return; }
  struct Interval { uint64_t begin,end; };
  std::array<Interval,MaxPasses> intervals{};
  // Reject incomplete/reset/invalid query results as a frame, never as zero ms.
  for (uint32_t i=0;i<slot.count;++i) {
    uint64_t a=timestamps[2*i], b=timestamps[2*i+1];
    if (!a || b<a || b-a>5'000'000'000ULL) { ++dropped; return; }
    intervals[i]={a,b};
  }
  Sample sample;
  for (uint32_t i=0;i<slot.count;++i)
    sample.rows[row_for(slot.names[i].data())] += (intervals[i].end-intervals[i].begin)*1e-6;
  std::sort(intervals.begin(),intervals.begin()+slot.count,[](auto a,auto b){return a.begin<b.begin;});
  uint64_t first=intervals[0].begin, end=first, covered=0;
  for (uint32_t i=0;i<slot.count;++i) {
    auto interval=intervals[i];
    if (interval.end>end) {
      covered += interval.end-std::max(end,interval.begin);
      end=interval.end;
    }
  }
  if (end-first>5'000'000'000ULL) { ++dropped; return; }
  sample.span=(end-first)*1e-6;
  sample.between=(end-first-covered)*1e-6;
  history[cursor]=sample;
  cursor=(cursor+1)%HistorySize;
  samples=std::min<uint32_t>(samples+1,HistorySize);
  mapFailed=false;
  if (diagnostic.load() && ++diagnosticFrames % HistorySize == 0) {
    std::printf("[MP6-GPU] span=%.3f between=%.3f ms passes=%u dropped=%u\n",
                sample.span,sample.between,slot.count,dropped);
    for (uint32_t i=0;i<nameCount;++i)
      std::printf("[MP6-GPU] %s=%.3f ms\n",names[i].data(),sample.rows[i]);
    emit_window_report();
  }
}
void drain() {
  for (auto& slot:slots) {
    if (!slot.completion) continue;
    const int state=slot.completion->state.load(std::memory_order_acquire);
    if (state==Pending) continue;
    if (state==Mapped) {
      if (slot.generation==generation && (generation&1)) consume(slot);
      slot.readback.Unmap();
    } else if (slot.generation==generation) {
      ++dropped;
      mapFailed=true;
      cancel_timing_snapshot();
    }
    slot.completion.reset();
  }
}
void allocate(Slot& slot) {
  if (slot.queries) return;
  const wgpu::QuerySetDescriptor q{.label="Live GPU timestamps",.type=wgpu::QueryType::Timestamp,.count=QueryCount};
  slot.queries=g_device.CreateQuerySet(&q);
  const wgpu::BufferDescriptor resolve{.label="GPU timestamp resolve",
      .usage=wgpu::BufferUsage::QueryResolve|wgpu::BufferUsage::CopySrc,.size=BufferSize};
  slot.resolve=g_device.CreateBuffer(&resolve);
  const wgpu::BufferDescriptor read{.label="GPU timestamp readback",
      .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=BufferSize};
  slot.readback=g_device.CreateBuffer(&read);
}
} // namespace

void initialize() {
  supported.store(g_device.HasFeature(wgpu::FeatureName::TimestampQuery)
#ifdef __EMSCRIPTEN__
                  && !aurora_web_timestamps_blocked()
#endif
  );
  const char* env=std::getenv("MP6_GPU_TIMINGS");
  diagnostic.store(env && env[0]=='1');
  if (diagnostic.load()) {
    request.store(3);
    std::printf("[MP6-GPU] timestamps=%s\n",supported.load() ? "supported" : "unsupported");
    std::fflush(stdout);
  }
  reset_history(request.load());
}
void shutdown() {
  supported.store(false);
  recording=submitted=nullptr;
  for (auto& slot:slots) {
    // Callbacks retain only Completion, never a recycled Slot or GPU handle.
    if (slot.readback) slot.readback.Destroy();
    slot=Slot{};
  }
  reset_history(0);
  std::lock_guard lock(publishMutex);
  published={};
  publishedGeneration=0;
}
void frame_begin(const wgpu::CommandEncoder&) {
  recording=nullptr;
  const uint64_t current=request.load(std::memory_order_acquire);
  if (current!=generation) reset_history(current);
  drain();
  if (!(current&1) || !supported.load()) return;
  for (auto& slot:slots) {
    if (slot.completion) continue;
    allocate(slot);
    slot.count=0;
    slot.overflow=false;
    slot.generation=current;
    recording=&slot;
    break;
  }
  if (!recording) ++dropped;
  publish();
}
void frame_end(const wgpu::CommandEncoder& encoder) {
  if (!recording) return;
  auto& slot=*recording;
  recording=nullptr;
  if (!slot.count) return;
  encoder.ResolveQuerySet(slot.queries,0,slot.count*2,slot.resolve,0);
  encoder.CopyBufferToBuffer(slot.resolve,0,slot.readback,0,BufferSize);
  submitted=&slot;
}
void after_submit() {
  if (submitted) {
    auto& slot=*submitted;
    submitted=nullptr;
    slot.completion=std::make_shared<Completion>();
    slot.readback.MapAsync(wgpu::MapMode::Read,0,BufferSize,wgpu::CallbackMode::AllowSpontaneous,
      [completion=slot.completion](wgpu::MapAsyncStatus status,wgpu::StringView) {
        completion->state.store(status==wgpu::MapAsyncStatus::Success ? Mapped : Failed,std::memory_order_release);
      });
  }
  drain();
}
const wgpu::PassTimestampWrites* pass_writes(std::string_view name) {
  if (!recording) return nullptr;
  auto& slot=*recording;
  if (slot.count==MaxPasses) { slot.overflow=true; return nullptr; }
  const uint32_t i=slot.count++;
  auto& label=slot.names[i];
  label.fill(0);
  std::memcpy(label.data(),name.data(),std::min(name.size(),label.size()-1));
  static wgpu::PassTimestampWrites writes;
  writes={.querySet=slot.queries,.beginningOfPassWriteIndex=i*2,.endOfPassWriteIndex=i*2+1};
  return &writes;
}
// Encoder-level timestamps are not portable to every mobile backend.
// Copies and barriers between passes remain in the measured span's residual.
Zone::Zone(const wgpu::CommandEncoder&,std::string_view) {}
Zone::~Zone()=default;
} // namespace aurora::webgpu::gpu_prof

extern "C" void aurora_gpu_stats_set_enabled(bool enabled) {
  using namespace aurora::webgpu::gpu_prof;
  enabled=enabled || diagnostic.load();
  auto old=request.load();
  while (bool(old&1)!=enabled && !request.compare_exchange_weak(old,((old+2)&~1ULL)|uint64_t(enabled))) {}
}
extern "C" void aurora_gpu_stats_read(AuroraGpuStats* out) {
  if (!out) return;
  using namespace aurora::webgpu::gpu_prof;
  std::lock_guard lock(publishMutex);
  *out={};
  if (!supported.load()) return;
  out->status=1;
  if (publishedGeneration==request.load()) *out=published;
}
extern "C" uint64_t aurora_gpu_timing_snapshot_request(void) {
  using namespace aurora::webgpu::gpu_prof;
  std::lock_guard lock(publishMutex);
  const uint64_t current = request.load();
  if (!supported.load() || !(current & 1) || snapshotNextId == UINT64_MAX) return 0;
  snapshotWantedId = ++snapshotNextId;
  snapshotWantedGeneration = current;
  publishedSnapshot = {};
  snapshotPending.store(snapshotWantedId, std::memory_order_release);
  return snapshotWantedId;
}
extern "C" bool aurora_gpu_timing_snapshot_read(uint64_t requestId, AuroraGpuTimingSnapshot* out) {
  if (!out) return false;
  *out = {};
  using namespace aurora::webgpu::gpu_prof;
  std::lock_guard lock(publishMutex);
  const uint64_t current = request.load();
  if (!requestId || !supported.load() || !(current & 1) || snapshotWantedId != requestId ||
      snapshotWantedGeneration != current || publishedSnapshot.requestId != requestId ||
      publishedSnapshot.generation != current) return false;
  *out = publishedSnapshot;
  return true;
}

#endif

namespace aurora::gfx {
const wgpu::PassTimestampWrites* gpu_pass_timestamps(const char* label) noexcept {
  return webgpu::gpu_prof::pass_writes(label);
}
}

#ifdef TRACY_ENABLE
extern "C" void aurora_gpu_stats_set_enabled(bool) {}
extern "C" void aurora_gpu_stats_read(AuroraGpuStats* out) { if (out) *out={}; }
extern "C" uint64_t aurora_gpu_timing_snapshot_request(void) { return 0; }
extern "C" bool aurora_gpu_timing_snapshot_read(uint64_t, AuroraGpuTimingSnapshot* out) {
  if (out) *out = {};
  return false;
}
#endif
