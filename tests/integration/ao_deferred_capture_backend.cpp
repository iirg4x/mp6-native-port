// QA-only deferred framebuffer capture backend.
//
// This translation unit is linked only by the native AO cadence/flicker QA
// launcher.  It deliberately lives below tests/integration: the normal game
// and release targets do not build it.  The caller invokes enqueue immediately
// after aurora_end_frame() and before the next frame begins.  A worker item is
// therefore ordered after that frame's end-frame work without stalling the
// caller on a GPU readback.

#include "webgpu/gpu.hpp"
#include "gfx/render_worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using aurora::webgpu::TextureWithSampler;

constexpr unsigned kMaxFrames = 128;
constexpr uint64_t kMaxPaddedBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMapDeadlineNs = 8ull * 1000ull * 1000ull * 1000ull;
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kHeaderBytes = 96;

enum class MapState : uint8_t {
  Pending = 0,
  Success = 1,
  Failed = 2,
};

struct MapCompletion {
  std::atomic<MapState> state{MapState::Pending};
};

struct FrameRequest {
  std::string outputDir;
  unsigned seq = 0;
  unsigned presentIndex = 0;
  long viTick = 0;
  int replay = 0;
  unsigned long long wallNs = 0;
  unsigned x = 0;
  unsigned y = 0;
  unsigned w = 0;
  unsigned h = 0;
};

struct Geometry {
  bool valid = false;
  uint32_t srcWidth = 0;
  uint32_t srcHeight = 0;
  uint32_t cropX = 0;
  uint32_t cropY = 0;
  uint32_t cropWidth = 0;
  uint32_t cropHeight = 0;
  uint32_t format = 0;
};

struct HeldFrame {
  FrameRequest request;
  uint32_t srcWidth = 0;
  uint32_t srcHeight = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t bytesPerRow = 0;
  uint64_t paddedBytes = 0;
  uint64_t tightBytes = 0;
  wgpu::Buffer readback;
  wgpu::Future future{};
  std::shared_ptr<MapCompletion> completion;
  bool mapped = false;
  bool mapReleased = false;
};

static uint32_t stable_format_code(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return 1;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return 2;
  case wgpu::TextureFormat::BGRA8Unorm:
    return 3;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return 4;
  default:
    return 0;
  }
}

static bool checked_crop(const FrameRequest& request, uint32_t srcWidth, uint32_t srcHeight) {
  if (request.w == 0 || request.h == 0 || request.x >= srcWidth || request.y >= srcHeight) {
    return false;
  }
  // The subtraction form avoids unsigned x+w/y+h overflow and intentionally
  // rejects a crop that would otherwise be silently clamped.
  return request.w <= srcWidth - request.x && request.h <= srcHeight - request.y;
}

static bool checked_sizes(uint32_t width, uint32_t height, uint32_t* bytesPerRow, uint64_t* paddedBytes,
                          uint64_t* tightBytes) {
  const uint64_t tightRow = uint64_t(width) * kBytesPerPixel;
  const uint64_t paddedRow = (tightRow + 255ull) & ~255ull;
  const uint64_t padded = paddedRow * uint64_t(height);
  const uint64_t tight = tightRow * uint64_t(height);
  if (width == 0 || height == 0 || tightRow > std::numeric_limits<uint32_t>::max() ||
      paddedRow > std::numeric_limits<uint32_t>::max() || padded == 0 || tight == 0) {
    return false;
  }
  *bytesPerRow = static_cast<uint32_t>(paddedRow);
  *paddedBytes = padded;
  *tightBytes = tight;
  return true;
}

static void put_u32(std::array<uint8_t, kHeaderBytes>& header, size_t offset, uint32_t value) {
  header[offset + 0] = static_cast<uint8_t>(value & 0xffu);
  header[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  header[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  header[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

static void put_u64(std::array<uint8_t, kHeaderBytes>& header, size_t offset, uint64_t value) {
  put_u32(header, offset, static_cast<uint32_t>(value & 0xffffffffull));
  put_u32(header, offset + 4, static_cast<uint32_t>((value >> 32) & 0xffffffffull));
}

class BurstState {
public:
  BurstState() { m_frames.reserve(kMaxFrames); }

  bool reserve(FrameRequest request) {
    std::lock_guard lock{m_mutex};
    if (!m_accepting || !m_error.empty() || request.outputDir.empty()) {
      return false;
    }
    if (m_outputDir.empty()) {
      m_outputDir = request.outputDir;
    } else if (m_outputDir != request.outputDir) {
      m_error = "output_dir_changed";
      return false;
    }
    if (m_queued >= kMaxFrames) {
      m_error = "frame_limit";
      return false;
    }
    if (!m_haveSequence && request.seq != 0) {
      m_error = "first_seq_not_zero";
      return false;
    }
    if (m_haveSequence) {
      if (m_lastSequence == std::numeric_limits<unsigned>::max() || request.seq != m_lastSequence + 1u) {
        m_error = "noncontiguous_seq";
        return false;
      }
    }
    if (m_havePresentIndex) {
      if (m_lastPresentIndex == std::numeric_limits<unsigned>::max() ||
          request.presentIndex != m_lastPresentIndex + 1u) {
        m_error = "noncontiguous_present_index";
        return false;
      }
    }
    if (request.replay != 0 && request.replay != 1) {
      m_error = "invalid_replay";
      return false;
    }
    if (request.w == 0 || request.h == 0 ||
        request.x > std::numeric_limits<uint32_t>::max() - request.w ||
        request.y > std::numeric_limits<uint32_t>::max() - request.h) {
      m_error = "invalid_crop";
      return false;
    }

    m_haveSequence = true;
    m_lastSequence = request.seq;
    m_havePresentIndex = true;
    m_lastPresentIndex = request.presentIndex;
    ++m_queued;
    return true;
  }

  void reject_queue_admission() {
    std::lock_guard lock{m_mutex};
    m_error = "render_queue_full";
  }

  void rollback_reservation(unsigned seq) {
    std::lock_guard lock{m_mutex};
    if (m_queued == 0 || !m_haveSequence || m_lastSequence != seq || !m_havePresentIndex) {
      m_error = "reservation_rollback";
      return;
    }
    --m_queued;
    if (m_queued == 0) {
      m_haveSequence = false;
      m_lastSequence = 0;
      m_havePresentIndex = false;
      m_lastPresentIndex = 0;
    } else {
      --m_lastSequence;
      --m_lastPresentIndex;
    }
  }

  void stop_accepting() {
    std::lock_guard lock{m_mutex};
    m_accepting = false;
  }

  void capture(FrameRequest request) {
    // Every callback owns this request by value.  Nothing from the enqueue
    // caller's stack, including its output path, is referenced here.
    {
      std::lock_guard lock{m_mutex};
      ++m_started;
      if (!m_error.empty()) {
        return;
      }
    }

    const auto fail = [this](const char* reason) {
      std::lock_guard lock{m_mutex};
      if (m_error.empty()) {
        m_error = reason;
      }
    };

    using namespace aurora::webgpu;
    if (!g_device || !g_queue) {
      fail("no_device");
      return;
    }

    const TextureWithSampler& source = present_source();
    if (!source.texture || source.size.width == 0 || source.size.height == 0) {
      fail("no_present_source");
      return;
    }
    if (!checked_crop(request, source.size.width, source.size.height)) {
      fail("crop_out_of_bounds");
      return;
    }
    const uint32_t format = stable_format_code(source.format);
    if (format == 0) {
      fail("unsupported_format");
      return;
    }

    uint32_t bytesPerRow = 0;
    uint64_t paddedBytes = 0;
    uint64_t tightBytes = 0;
    if (!checked_sizes(request.w, request.h, &bytesPerRow, &paddedBytes, &tightBytes)) {
      fail("invalid_size");
      return;
    }

    {
      std::lock_guard lock{m_mutex};
      if (m_geometry.valid &&
          (m_geometry.srcWidth != source.size.width || m_geometry.srcHeight != source.size.height ||
           m_geometry.cropX != request.x || m_geometry.cropY != request.y ||
           m_geometry.cropWidth != request.w || m_geometry.cropHeight != request.h ||
           m_geometry.format != format)) {
        if (m_error.empty()) {
          m_error = "geometry_changed";
        }
        return;
      }
      if (paddedBytes > kMaxPaddedBytes || m_paddedBytes > kMaxPaddedBytes - paddedBytes) {
        if (m_error.empty()) {
          m_error = "readback_budget";
        }
        return;
      }
      if (!m_geometry.valid) {
        m_geometry = {
            .valid = true,
            .srcWidth = source.size.width,
            .srcHeight = source.size.height,
            .cropX = request.x,
            .cropY = request.y,
            .cropWidth = request.w,
            .cropHeight = request.h,
            .format = format,
        };
      }
      m_paddedBytes += paddedBytes;
    }

    const wgpu::BufferDescriptor readbackDescriptor{
        .label = "AO Deferred Capture Readback Buffer",
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
        .size = paddedBytes,
    };
    wgpu::Buffer readback = g_device.CreateBuffer(&readbackDescriptor);
    if (!readback) {
      fail("buffer_create");
      return;
    }
    wgpu::CommandEncoder encoder = g_device.CreateCommandEncoder();
    if (!encoder) {
      fail("encoder_create");
      return;
    }
    const wgpu::TexelCopyTextureInfo copySource{
        .texture = source.texture,
        .mipLevel = 0,
        .origin = wgpu::Origin3D{request.x, request.y, 0},
    };
    const wgpu::TexelCopyBufferInfo copyDestination{
        .layout = wgpu::TexelCopyBufferLayout{
            .offset = 0,
            .bytesPerRow = bytesPerRow,
            .rowsPerImage = request.h,
        },
        .buffer = readback,
    };
    const wgpu::Extent3D copySize{request.w, request.h, 1};
    encoder.CopyTextureToBuffer(&copySource, &copyDestination, &copySize);
    wgpu::CommandBuffer commands = encoder.Finish();
    if (!commands) {
      fail("encoder_finish");
      return;
    }
    g_queue.Submit(1, &commands);

    // Keep this exact buffer alive until finish has copied its mapped rows to
    // disk.  A new buffer is created for every frame; no slot is recycled
    // while a prior copy may still be in flight.
    {
      std::lock_guard lock{m_mutex};
      const unsigned requestWidth = request.w;
      const unsigned requestHeight = request.h;
      m_frames.push_back(HeldFrame{
          .request = std::move(request),
          .srcWidth = source.size.width,
          .srcHeight = source.size.height,
          .width = requestWidth,
          .height = requestHeight,
          .format = format,
          .bytesPerRow = bytesPerRow,
          .paddedBytes = paddedBytes,
          .tightBytes = tightBytes,
          .readback = std::move(readback),
      });
      ++m_captured;
    }
  }

  void start_mapping() {
    // Called from one queue item, after all per-frame copy items.  MapAsync is
    // deliberately deferred until this point; enqueue never maps or waits.
    std::lock_guard lock{m_mutex};
    if (!m_error.empty()) {
      return;
    }
    if (m_started != m_queued || m_captured != m_queued || m_frames.size() != m_queued) {
      m_error = "capture_dropped";
      return;
    }
    if (!aurora::webgpu::g_device || !aurora::webgpu::g_instance) {
      m_error = "no_device";
      return;
    }
    for (auto& frame : m_frames) {
      frame.completion = std::make_shared<MapCompletion>();
      frame.future = frame.readback.MapAsync(
          wgpu::MapMode::Read, 0, frame.paddedBytes, wgpu::CallbackMode::WaitAnyOnly,
          [completion = frame.completion](wgpu::MapAsyncStatus status, wgpu::StringView) {
            completion->state.store(status == wgpu::MapAsyncStatus::Success ? MapState::Success : MapState::Failed,
                                    std::memory_order_release);
          });
      if (frame.future.id == 0) {
        m_error = "map_submit";
        return;
      }
    }
  }

  bool has_error() const {
    std::lock_guard lock{m_mutex};
    return !m_error.empty();
  }

  std::string error() const {
    std::lock_guard lock{m_mutex};
    return m_error.empty() ? std::string{"none"} : m_error;
  }

  std::string output_dir() const {
    std::lock_guard lock{m_mutex};
    return m_outputDir;
  }

  unsigned queued() const {
    std::lock_guard lock{m_mutex};
    return m_queued;
  }

  unsigned started() const {
    std::lock_guard lock{m_mutex};
    return m_started;
  }

  unsigned captured() const {
    std::lock_guard lock{m_mutex};
    return m_captured;
  }

  uint64_t padded_bytes() const {
    std::lock_guard lock{m_mutex};
    return m_paddedBytes;
  }

  std::vector<HeldFrame>& frames() { return m_frames; }

  void release_buffers() {
    std::lock_guard lock{m_mutex};
    for (auto& frame : m_frames) {
      const bool callbackMapped = frame.completion &&
                                  frame.completion->state.load(std::memory_order_acquire) == MapState::Success;
      if ((frame.mapped || callbackMapped) && !frame.mapReleased) {
        frame.readback.Unmap();
        frame.mapped = false;
        frame.mapReleased = true;
      }
      frame.readback = {};
    }
    m_frames.clear();
  }

private:
  mutable std::mutex m_mutex;
  bool m_accepting = true;
  bool m_haveSequence = false;
  bool m_havePresentIndex = false;
  unsigned m_lastSequence = 0;
  unsigned m_lastPresentIndex = 0;
  unsigned m_queued = 0;
  unsigned m_started = 0;
  unsigned m_captured = 0;
  uint64_t m_paddedBytes = 0;
  std::string m_outputDir;
  std::string m_error;
  Geometry m_geometry;
  std::vector<HeldFrame> m_frames;
};

std::mutex g_sessionMutex;
std::shared_ptr<BurstState> g_session;
bool g_finished = false;

static std::filesystem::path path_from_utf8(const std::string& value) {
#ifdef _WIN32
  return std::filesystem::u8path(value);
#else
  return std::filesystem::path(value);
#endif
}

static FILE* open_path(const std::filesystem::path& path, bool binary) {
#ifdef _WIN32
  return _wfopen(path.c_str(), binary ? L"wb" : L"w");
#else
  return std::fopen(path.c_str(), binary ? "wb" : "w");
#endif
}

static bool write_frame(HeldFrame& frame, FILE* index, unsigned* written, uint64_t* payloadBytes,
                        std::string* error) {
  const auto* mapped = static_cast<const uint8_t*>(frame.readback.GetConstMappedRange(0, frame.paddedBytes));
  if (mapped == nullptr) {
    *error = "mapped_range";
    return false;
  }
  frame.mapped = true;

  std::array<uint8_t, kHeaderBytes> header{};
  std::memcpy(header.data(), "MP6FDUMP", 8);
  put_u32(header, 8, 1u);
  put_u32(header, 12, frame.width);
  put_u32(header, 16, frame.height);
  put_u32(header, 20, kBytesPerPixel);
  put_u32(header, 24, frame.format);
  put_u32(header, 28, frame.request.seq);
  put_u32(header, 32, frame.request.presentIndex);
  put_u32(header, 36, frame.request.replay ? 1u : 0u);
  put_u64(header, 40, static_cast<uint64_t>(frame.request.viTick));
  put_u64(header, 48, static_cast<uint64_t>(frame.request.wallNs));
  put_u32(header, 56, frame.srcWidth);
  put_u32(header, 60, frame.srcHeight);
  put_u32(header, 64, frame.request.x);
  put_u32(header, 68, frame.request.y);
  put_u32(header, 72, frame.width);
  put_u32(header, 76, frame.height);
  put_u32(header, 80, 1u); // downscale=1: deferred capture preserves source rows.

  char name[32]{};
  const int nameLength = std::snprintf(name, sizeof(name), "f%06u.mfd", frame.request.seq);
  if (nameLength <= 0 || static_cast<size_t>(nameLength) >= sizeof(name)) {
    frame.readback.Unmap();
    frame.mapped = false;
    frame.mapReleased = true;
    *error = "output_name";
    return false;
  }
  const std::filesystem::path path = path_from_utf8(frame.request.outputDir) / name;
  FILE* output = open_path(path, true);
  if (output == nullptr) {
    frame.readback.Unmap();
    frame.mapped = false;
    frame.mapReleased = true;
    *error = "file_open";
    return false;
  }

  bool ok = std::fwrite(header.data(), 1, header.size(), output) == header.size();
  const uint64_t tightRow = uint64_t(frame.width) * kBytesPerPixel;
  for (uint32_t y = 0; ok && y < frame.height; ++y) {
    ok = std::fwrite(mapped + uint64_t(y) * frame.bytesPerRow, 1, static_cast<size_t>(tightRow), output) ==
         static_cast<size_t>(tightRow);
  }
  if (std::fclose(output) != 0) {
    ok = false;
  }
  frame.readback.Unmap();
  frame.mapped = false;
  frame.mapReleased = true;
  if (!ok) {
    *error = "file_write";
    return false;
  }

  const int indexResult = std::fprintf(
      index, "%u,%u,%ld,%llu,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", frame.request.seq,
      frame.request.presentIndex, frame.request.viTick, frame.request.wallNs, frame.request.replay ? 1 : 0,
      frame.width, frame.height, frame.srcWidth, frame.srcHeight, frame.request.x, frame.request.y, frame.width,
      frame.height, 1u, frame.format);
  if (indexResult < 0 || std::fflush(index) != 0) {
    *error = "index_write";
    return false;
  }
  ++*written;
  *payloadBytes += frame.tightBytes;
  return true;
}

static int finish_burst(const std::shared_ptr<BurstState>& session) {
  session->stop_accepting();

  // This is the sole queue synchronization in this backend.  Captures queued
  // before it are ordered first; the map requests are issued by the final
  // worker item and completed by WaitAny below.
  bool mapQueued = false;
  if (aurora::gfx::render_worker::can_enqueue()) {
    aurora::gfx::render_worker::enqueue_work([session] { session->start_mapping(); });
    mapQueued = true;
  } else {
    session->reject_queue_admission();
  }
  aurora::gfx::render_worker::synchronize();

  unsigned written = 0;
  uint64_t payloadBytes = 0;
  std::string writeError;
  bool success = mapQueued && !session->has_error() && session->started() == session->queued() &&
                 session->captured() == session->queued() && session->queued() != 0;

  auto& frames = session->frames();
  if (success) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds{kMapDeadlineNs};
    for (auto& frame : frames) {
      if (!frame.completion || frame.future.id == 0) {
        writeError = "map_submit";
        success = false;
        break;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        writeError = "map_timeout";
        success = false;
        break;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
      const auto waitStatus = aurora::webgpu::g_instance.WaitAny(frame.future, static_cast<uint64_t>(remaining));
      const MapState state = frame.completion->state.load(std::memory_order_acquire);
      if (state == MapState::Success) {
        // A successful callback leaves this buffer mapped even if this
        // particular WaitAny call reports an error; cleanup must still Unmap.
        frame.mapped = true;
      }
      if (waitStatus != wgpu::WaitStatus::Success || state != MapState::Success) {
        writeError = waitStatus == wgpu::WaitStatus::TimedOut ? "map_timeout" : "map_failure";
        success = false;
        break;
      }
    }
  }

  FILE* index = nullptr;
  if (success) {
    std::error_code ec;
    const std::filesystem::path directory = path_from_utf8(session->output_dir());
    std::filesystem::create_directories(directory, ec);
    if (ec || !std::filesystem::is_directory(directory, ec) || ec) {
      writeError = "output_dir";
      success = false;
    } else {
      const std::filesystem::path indexPath = directory / "index.csv";
      index = open_path(indexPath, false);
      if (index == nullptr ||
          std::fprintf(index, "seq,present_index,tick,wall_ns,replay,width,height,src_width,src_height,"
                              "crop_x,crop_y,crop_w,crop_h,downscale,format\n") < 0) {
        if (index != nullptr) {
          std::fclose(index);
          index = nullptr;
        }
        writeError = "index_open";
        success = false;
      }
    }
  }
  if (success) {
    for (auto& frame : frames) {
      if (!write_frame(frame, index, &written, &payloadBytes, &writeError)) {
        success = false;
        break;
      }
    }
  }
  if (index != nullptr) {
    if (std::fclose(index) != 0 && writeError.empty()) {
      writeError = "index_close";
      success = false;
    }
    index = nullptr;
  }

  if (!success && writeError.empty()) {
    writeError = session->error();
  }
  const std::string sessionError = session->error();
  const std::string error = success ? std::string{"none"} : (!writeError.empty() ? writeError : sessionError);
  const int errorFlag = success && written == session->queued() ? 0 : 1;
  const uint64_t gpuBytes = session->padded_bytes();
  std::printf("[AO-DEFERRED-CAPTURE] queued=%u captured=%u written=%u bytes=%llu error=%d\n", session->queued(),
              session->captured(), written, static_cast<unsigned long long>(gpuBytes), errorFlag);
  if (errorFlag != 0) {
    std::printf("[AO-DEFERRED-ERROR] %s\n", error.c_str());
  }
  std::fflush(stdout);

  // If a timeout/error left a callback pending, its lambda owns only the
  // shared completion object.  Releasing these buffers is therefore safe and
  // cannot leave a callback pointing at this stack or at a destroyed frame.
  session->release_buffers();
  return errorFlag == 0 ? 1 : 0;
}

} // namespace

extern "C" int mp6_qa_deferred_capture_enqueue(const char* outputDir, unsigned seq, unsigned presentIndex,
                                                 long viTick, int replay, unsigned long long wallNs, unsigned x,
                                                 unsigned y, unsigned w, unsigned h) {
  if (outputDir == nullptr || outputDir[0] == '\0') {
    return 0;
  }
  FrameRequest request{
      .outputDir = std::string{outputDir},
      .seq = seq,
      .presentIndex = presentIndex,
      .viTick = viTick,
      .replay = replay,
      .wallNs = wallNs,
      .x = x,
      .y = y,
      .w = w,
      .h = h,
  };

  std::shared_ptr<BurstState> session;
  {
    std::lock_guard lock{g_sessionMutex};
    if (g_finished) {
      return 0;
    }
    if (!g_session) {
      g_session = std::make_shared<BurstState>();
    }
    session = g_session;
  }
  if (!session->reserve(request)) {
    return 0;
  }
  if (!aurora::gfx::render_worker::can_enqueue()) {
    session->reject_queue_admission();
    session->rollback_reservation(request.seq);
    return 0;
  }

  // Queue ordering is the lifecycle contract: the caller invokes this just
  // after end_frame, so this item observes that completed frame and runs
  // before the next begin-frame item.  No synchronize, WaitAny, map, or file
  // operation is performed on this path.
  aurora::gfx::render_worker::enqueue_work([session, request = std::move(request)]() mutable {
    session->capture(std::move(request));
  });
  return 1;
}

extern "C" int mp6_qa_deferred_capture_finish(void) {
  std::shared_ptr<BurstState> session;
  {
    std::lock_guard lock{g_sessionMutex};
    if (g_finished) {
      return 0;
    }
    g_finished = true;
    session = g_session;
    g_session.reset();
  }
  if (!session) {
    std::printf("[AO-DEFERRED-CAPTURE] queued=0 captured=0 written=0 bytes=0 error=1\n");
    std::printf("[AO-DEFERRED-ERROR] no_session\n");
    std::fflush(stdout);
    return 0;
  }
  return finish_burst(session);
}
