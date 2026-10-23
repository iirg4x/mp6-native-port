#pragma once

// QA-only XeGTAO texture readback.  This header is included by the private AO
// translation unit only when a cadence/quality runner explicitly instruments
// it; the normal game and release targets do not build this code.

#include "webgpu/gpu.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace mp6::qa::ao_buffers {

struct Metadata {
    uint64_t frame;
    int64_t tick;
    int32_t camera;
    uint32_t width, height;
    std::array<uint32_t, 20> params;
    std::array<uint32_t, 24> constants;
    std::array<uint32_t, 12> adapter;
};

namespace detail {

constexpr uint64_t kMinTick = 3000;
constexpr uint64_t kMaxTick = 1000000;
constexpr uint32_t kMaxJobs = 2;
constexpr uint32_t kPlanesPerJob = 11;
constexpr uint32_t kMaxPlanes = kMaxJobs * kPlanesPerJob;
constexpr uint64_t kMaxPaddedBytes = 96ull * 1024ull * 1024ull;
constexpr uint64_t kMapDeadlineNs = 8ull * 1000ull * 1000ull * 1000ull;
constexpr uint32_t kBytesPerPixel = 4;

struct StageSpec {
    const char *name;
    uint32_t mip;
};

// Keep this table in capture order.  The parent records clean depth and
// canonical depth before the depth prefilter, then the five actual mip views.
static constexpr std::array<StageSpec, kPlanesPerJob> kStages{{
    {"clean_depth", 0},
    {"canonical_depth", 0},
    {"depth_mip", 0},
    {"depth_mip", 1},
    {"depth_mip", 2},
    {"depth_mip", 3},
    {"depth_mip", 4},
    {"normals", 0},
    {"raw_ao", 0},
    {"edges", 0},
    {"final_ao", 0},
}};

enum class MapState : uint8_t {
    Pending = 0,
    Success = 1,
    Failed = 2,
};

// A map callback can run after a timeout or another map failure.  Keep this
// object, and the buffer captured by the callback, alive until the process has
// finished with the QA state.  cleanupRequested/unmapClaimed make a late
// successful callback able to release its mapping exactly once.
struct MapCompletion {
    std::atomic<MapState> state{MapState::Pending};
    std::atomic<bool> cleanupRequested{false};
    std::atomic<bool> unmapClaimed{false};
};

struct Plane {
    const StageSpec *spec = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytesPerRow = 0;
    uint64_t paddedBytes = 0;
    uint64_t tightBytes = 0;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    wgpu::Buffer readback;
    wgpu::Future future{};
    std::shared_ptr<MapCompletion> completion;
    const uint8_t *mapped = nullptr;
    bool mappedByFinish = false;
};

struct Job {
    bool begun = false;
    uint32_t planeCount = 0;
    Metadata metadata{};
    std::array<Plane, kPlanesPerJob> planes{};
};

struct State {
    bool initialized = false;
    bool enabled = false;
    bool error = false;
    bool finished = false;
    bool active = false;
    bool ignoredSelection = false;
    uint32_t activeSlot = 0;
    uint32_t nextStage = 0;
    uint32_t jobsCount = 0;
    uint64_t targetTick = 0;
    uint64_t paddedBytes = 0;
    const char *errorReason = nullptr;
    std::filesystem::path outputDirectory;
    std::array<Job, kMaxJobs> jobs{};
};

inline State &state() noexcept {
    static State value;
    return value;
}

inline void latch_error(State &s, const char *reason) noexcept {
    if (!s.error) {
        s.error = true;
        s.errorReason = reason;
        std::fprintf(stderr, "[AO-BUFFERS-ERROR] %s\n", reason != nullptr ? reason : "unknown");
        std::fflush(stderr);
    }
}

inline bool parse_tick(const char *raw, uint64_t *value) noexcept {
    if (raw == nullptr || raw[0] == '\0' || value == nullptr) return false;
    uint64_t parsed = 0;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(raw); *cursor != 0; ++cursor) {
        if (*cursor < static_cast<unsigned char>('0') || *cursor > static_cast<unsigned char>('9')) return false;
        const uint64_t digit = static_cast<uint64_t>(*cursor - static_cast<unsigned char>('0'));
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10ull) return false;
        parsed = parsed * 10ull + digit;
    }
    *value = parsed;
    return true;
}

inline bool initialize(State &s) noexcept {
    if (s.initialized) return s.enabled && !s.error;
    s.initialized = true;

    // Absence of the opt-in tick is the normal production path: stay fully
    // inert and do not emit QA logs or touch the filesystem.
    const char *rawTick = std::getenv("MP6_QA_AO_BUFFERS_TICK");
    if (rawTick == nullptr || rawTick[0] == '\0') return false;
    s.enabled = true;

    uint64_t target = 0;
    if (!parse_tick(rawTick, &target) || target < kMinTick || target > kMaxTick) {
        latch_error(s, "invalid_target_tick");
        return false;
    }
    s.targetTick = target;

    const char *rawDirectory = std::getenv("MP6_QA_AO_BUFFERS_DIR");
    if (rawDirectory == nullptr || rawDirectory[0] == '\0') {
        latch_error(s, "missing_output_directory");
        return false;
    }

    try {
        // u8path is intentional: the runner owns this UTF-8 environment
        // value, including on Windows where path::c_str() is wide.
        s.outputDirectory = std::filesystem::u8path(std::string(rawDirectory));
        std::error_code ec;
        if (!std::filesystem::is_directory(s.outputDirectory, ec) || ec) {
            latch_error(s, "output_directory_missing");
            return false;
        }
        std::filesystem::directory_iterator it(s.outputDirectory, ec);
        if (ec) {
            latch_error(s, "output_directory_unreadable");
            return false;
        }
        if (it != std::filesystem::directory_iterator{}) {
            latch_error(s, "output_directory_not_empty");
            return false;
        }
    } catch (...) {
        latch_error(s, "output_directory_invalid_utf8");
        return false;
    }
    return true;
}

inline const char *format_name(wgpu::TextureFormat format) noexcept {
    switch (format) {
    case wgpu::TextureFormat::R32Float:
        return "r32float";
    case wgpu::TextureFormat::R32Uint:
        return "r32uint";
    default:
        return nullptr;
    }
}

inline bool expected_stage(const char *name, uint32_t mip, uint32_t index) noexcept {
    return index < kStages.size() && name != nullptr && std::strcmp(name, kStages[index].name) == 0 &&
           mip == kStages[index].mip;
}

inline bool checked_copy_size(uint32_t width, uint32_t height, uint32_t *bytesPerRow,
                              uint64_t *paddedBytes, uint64_t *tightBytes) noexcept {
    if (width == 0 || height == 0 || bytesPerRow == nullptr || paddedBytes == nullptr || tightBytes == nullptr)
        return false;
    const uint64_t tightRow = static_cast<uint64_t>(width) * kBytesPerPixel;
    const uint64_t paddedRow = (tightRow + 255ull) & ~255ull;
    const uint64_t padded = paddedRow * static_cast<uint64_t>(height);
    const uint64_t tight = tightRow * static_cast<uint64_t>(height);
    if (tightRow == 0 || paddedRow == 0 || padded == 0 || tight == 0 ||
        paddedRow > std::numeric_limits<uint32_t>::max())
        return false;
    *bytesPerRow = static_cast<uint32_t>(paddedRow);
    *paddedBytes = padded;
    *tightBytes = tight;
    return true;
}

inline bool has_copy_source_usage(const wgpu::Texture &texture) noexcept {
    const uint64_t usage = static_cast<uint64_t>(texture.GetUsage());
    const uint64_t copySource = static_cast<uint64_t>(wgpu::TextureUsage::CopySrc);
    return (usage & copySource) != 0;
}

inline void request_cleanup(Plane &plane) noexcept {
    if (!plane.completion) return;
    plane.completion->cleanupRequested.store(true, std::memory_order_release);
    if (plane.completion->state.load(std::memory_order_acquire) == MapState::Success &&
        !plane.completion->unmapClaimed.exchange(true, std::memory_order_acq_rel)) {
        if (plane.readback) plane.readback.Unmap();
    }
}

inline void unmap_all(State &s) noexcept {
    for (Job &job : s.jobs) {
        for (Plane &plane : job.planes) {
            if (plane.completion) {
                request_cleanup(plane);
            } else if (plane.mappedByFinish && plane.readback) {
                plane.readback.Unmap();
            }
            plane.mappedByFinish = false;
            plane.mapped = nullptr;
        }
    }
}

inline bool make_file_name(uint32_t jobIndex, const Plane &plane, char *name, size_t capacity) noexcept {
    if (name == nullptr || capacity == 0 || plane.spec == nullptr) return false;
    const int length = std::snprintf(name, capacity, "j%03u_%s_m%u.bin", jobIndex, plane.spec->name,
                                     plane.spec->mip);
    return length > 0 && static_cast<size_t>(length) < capacity;
}

inline std::FILE *open_exclusive(const std::filesystem::path &path, bool binary) noexcept {
#ifdef _WIN32
    return _wfopen(path.c_str(), binary ? L"wbx" : L"wx");
#else
    return std::fopen(path.c_str(), binary ? "wbx" : "wx");
#endif
}

inline void remove_created(const std::array<std::filesystem::path, kMaxPlanes + 1> &created,
                           size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) {
        std::error_code ec;
        std::filesystem::remove(created[i], ec);
    }
}

inline bool write_u32_array(std::FILE *file, const uint32_t *values, size_t count) noexcept {
    if (file == nullptr || values == nullptr) return false;
    for (size_t i = 0; i < count; ++i) {
        if (std::fprintf(file, "%s%u", i == 0 ? "" : ",", values[i]) < 0) return false;
    }
    return true;
}

inline bool write_index(std::FILE *file, const State &s,
                        const std::array<std::array<std::string, kPlanesPerJob>, kMaxJobs> &names) noexcept {
    if (file == nullptr) return false;
    if (std::fprintf(file, "{\"version\":1,\"jobs\":[") < 0) return false;
    for (uint32_t j = 0; j < kMaxJobs; ++j) {
        const Job &job = s.jobs[j];
        if (j != 0 && std::fprintf(file, ",") < 0) return false;
        if (std::fprintf(file,
                         "{\"frame\":%llu,\"tick\":%lld,\"camera\":%d,\"width\":%u,\"height\":%u,"
                         "\"params_bits\":[",
                         static_cast<unsigned long long>(job.metadata.frame),
                         static_cast<long long>(job.metadata.tick), static_cast<int>(job.metadata.camera),
                         job.metadata.width, job.metadata.height) < 0)
            return false;
        if (!write_u32_array(file, job.metadata.params.data(), job.metadata.params.size())) return false;
        if (std::fprintf(file, "],\"constants_bits\":[") < 0) return false;
        if (!write_u32_array(file, job.metadata.constants.data(), job.metadata.constants.size())) return false;
        if (std::fprintf(file, "],\"adapter_bits\":[") < 0) return false;
        if (!write_u32_array(file, job.metadata.adapter.data(), job.metadata.adapter.size())) return false;
        if (std::fprintf(file, "],\"planes\":[") < 0) return false;
        for (uint32_t p = 0; p < kPlanesPerJob; ++p) {
            const Plane &plane = job.planes[p];
            const char *format = format_name(plane.format);
            if (p != 0 && std::fprintf(file, ",") < 0) return false;
            if (format == nullptr || plane.spec == nullptr ||
                std::fprintf(file,
                             "{\"stage\":\"%s\",\"mip\":%u,\"width\":%u,\"height\":%u,"
                             "\"format\":\"%s\",\"file\":\"%s\",\"bytes\":%llu}",
                             plane.spec->name, plane.spec->mip, plane.width, plane.height, format,
                             names[j][p].c_str(), static_cast<unsigned long long>(plane.tightBytes)) < 0)
                return false;
        }
        if (std::fprintf(file, "]}") < 0) return false;
    }
    return std::fprintf(file, "],\"complete\":true}\n") >= 0;
}

inline bool map_and_write(State &s) noexcept {
    bool result = false;
    std::array<std::array<std::filesystem::path, kPlanesPerJob>, kMaxJobs> paths{};
    std::array<std::array<std::string, kPlanesPerJob>, kMaxJobs> names{};
    std::array<std::filesystem::path, kMaxPlanes + 1> created{};
    size_t createdCount = 0;

    try {
        if (!aurora::webgpu::g_instance) {
            latch_error(s, "no_instance");
            return false;
        }

        // Submit every map request before waiting.  The caller has already
        // synchronized the render worker and all frame submissions by the
        // time finish() reaches this point.
        for (Job &job : s.jobs) {
            for (Plane &plane : job.planes) {
                plane.completion = std::make_shared<MapCompletion>();
                const std::shared_ptr<MapCompletion> completion = plane.completion;
                const wgpu::Buffer callbackBuffer = plane.readback;
                plane.future = plane.readback.MapAsync(
                    wgpu::MapMode::Read, 0, plane.paddedBytes, wgpu::CallbackMode::WaitAnyOnly,
                    [completion, callbackBuffer](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        const bool success = status == wgpu::MapAsyncStatus::Success;
                        completion->state.store(success ? MapState::Success : MapState::Failed,
                                                std::memory_order_release);
                        if (success && completion->cleanupRequested.load(std::memory_order_acquire) &&
                            !completion->unmapClaimed.exchange(true, std::memory_order_acq_rel)) {
                            if (callbackBuffer) callbackBuffer.Unmap();
                        }
                    });
                if (plane.future.id == 0) latch_error(s, "map_submit");
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(kMapDeadlineNs);
        bool allMapped = true;
        for (Job &job : s.jobs) {
            for (Plane &plane : job.planes) {
                if (plane.future.id == 0) {
                    allMapped = false;
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                const uint64_t remaining = now >= deadline
                                                ? 0ull
                                                : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                              deadline - now)
                                                                              .count());
                const auto waitStatus = aurora::webgpu::g_instance.WaitAny(plane.future, remaining);
                const MapState mapState = plane.completion->state.load(std::memory_order_acquire);
                if (mapState == MapState::Success) plane.mappedByFinish = true;
                if (waitStatus != wgpu::WaitStatus::Success || mapState != MapState::Success) {
                    allMapped = false;
                    latch_error(s, waitStatus == wgpu::WaitStatus::TimedOut ? "map_timeout" : "map_failure");
                }
            }
        }
        if (!allMapped) return false;

        // Validate every mapped range before opening any output.  This keeps
        // index.json impossible to emit for a partial/invalid capture.
        for (Job &job : s.jobs) {
            for (Plane &plane : job.planes) {
                plane.mapped = static_cast<const uint8_t *>(plane.readback.GetConstMappedRange(0, plane.paddedBytes));
                if (plane.mapped == nullptr) {
                    latch_error(s, "mapped_range");
                    return false;
                }
            }
        }

        std::error_code directoryError;
        if (!std::filesystem::is_directory(s.outputDirectory, directoryError) || directoryError) {
            latch_error(s, "output_directory_missing");
            return false;
        }
        const std::filesystem::path indexPath = s.outputDirectory / "index.json";
        std::error_code existsError;
        if (std::filesystem::exists(indexPath, existsError) || existsError) {
            latch_error(s, "output_exists");
            return false;
        }

        for (uint32_t j = 0; j < kMaxJobs; ++j) {
            for (uint32_t p = 0; p < kPlanesPerJob; ++p) {
                char name[96]{};
                if (!make_file_name(j, s.jobs[j].planes[p], name, sizeof(name))) {
                    latch_error(s, "output_name");
                    return false;
                }
                names[j][p] = name;
                paths[j][p] = s.outputDirectory / name;
                existsError.clear();
                if (std::filesystem::exists(paths[j][p], existsError) || existsError) {
                    latch_error(s, "output_exists");
                    return false;
                }
            }
        }

        // Emit tightly packed rows only after every map passed validation.
        for (uint32_t j = 0; j < kMaxJobs; ++j) {
            for (uint32_t p = 0; p < kPlanesPerJob; ++p) {
                Plane &plane = s.jobs[j].planes[p];
                std::FILE *output = open_exclusive(paths[j][p], true);
                if (output == nullptr) {
                    latch_error(s, "file_open");
                    remove_created(created, createdCount);
                    return false;
                }
                created[createdCount++] = paths[j][p];
                const uint64_t tightRow = static_cast<uint64_t>(plane.width) * kBytesPerPixel;
                bool ok = true;
                for (uint32_t y = 0; ok && y < plane.height; ++y) {
                    ok = std::fwrite(plane.mapped + static_cast<uint64_t>(y) * plane.bytesPerRow, 1,
                                     static_cast<size_t>(tightRow), output) == static_cast<size_t>(tightRow);
                }
                if (std::fclose(output) != 0) ok = false;
                if (!ok) {
                    latch_error(s, "file_write");
                    remove_created(created, createdCount);
                    return false;
                }
            }
        }

        std::FILE *index = open_exclusive(indexPath, false);
        if (index == nullptr) {
            latch_error(s, "index_open");
            remove_created(created, createdCount);
            return false;
        }
        created[createdCount++] = indexPath;
        const bool indexOk = write_index(index, s, names);
        if (std::fclose(index) != 0 || !indexOk) {
            latch_error(s, "index_write");
            remove_created(created, createdCount);
            return false;
        }
        result = true;
    } catch (...) {
        latch_error(s, "capture_output_exception");
        remove_created(created, createdCount);
        result = false;
    }
    return result;
}

} // namespace detail

inline bool begin_job(const Metadata &metadata) {
    detail::State &s = detail::state();
    if (s.finished) {
        detail::latch_error(s, "begin_after_finish");
        return false;
    }
    if (!detail::initialize(s) || s.error) return false;
    if (s.active) {
        detail::latch_error(s, "nested_job");
        s.active = false;
        return false;
    }
    s.ignoredSelection = false;

    // Non-selected AO jobs are deliberately inert.  The caller may invoke
    // this function for every camera; only camera zero in the two exact ticks
    // can allocate a readback or affect the QA result.
    if (metadata.camera != 0 || metadata.tick < static_cast<int64_t>(s.targetTick) ||
        metadata.tick > static_cast<int64_t>(s.targetTick + 1)) {
        s.ignoredSelection = true;
        return false;
    }
    const uint32_t slot = metadata.tick == static_cast<int64_t>(s.targetTick) ? 0u : 1u;
    if (metadata.width == 0 || metadata.height == 0) {
        detail::latch_error(s, "invalid_dimensions");
        return false;
    }
    if (s.jobsCount >= detail::kMaxJobs || s.jobs[slot].begun) {
        detail::latch_error(s, "duplicate_selected_job");
        return false;
    }
    for (uint32_t i = 0; i < detail::kMaxJobs; ++i) {
        if (s.jobs[i].begun && s.jobs[i].metadata.frame == metadata.frame) {
            detail::latch_error(s, "duplicate_frame");
            return false;
        }
    }
    s.jobs[slot].begun = true;
    s.jobs[slot].planeCount = 0;
    s.jobs[slot].metadata = metadata;
    s.jobsCount++;
    s.activeSlot = slot;
    s.nextStage = 0;
    s.active = true;
    return true;
}

inline void capture(const wgpu::Device &device, const wgpu::CommandEncoder &encoder,
                    const wgpu::Texture &texture, const char *stage, uint32_t mip = 0) {
    detail::State &s = detail::state();
    if (!s.enabled || s.error || !s.active) {
        if (s.enabled && !s.error && !s.ignoredSelection && !s.finished && !s.active)
            detail::latch_error(s, "capture_without_job");
        return;
    }
    if (s.finished) {
        detail::latch_error(s, "capture_after_finish");
        return;
    }
    detail::Job &job = s.jobs[s.activeSlot];
    if (!detail::expected_stage(stage, mip, s.nextStage)) {
        detail::latch_error(s, "stage_order");
        return;
    }
    if (!device || !encoder || !texture) {
        detail::latch_error(s, "missing_gpu_handle");
        return;
    }
    if (!detail::has_copy_source_usage(texture)) {
        detail::latch_error(s, "texture_not_copy_src");
        return;
    }
    const wgpu::TextureFormat format = texture.GetFormat();
    if (detail::format_name(format) == nullptr) {
        detail::latch_error(s, "unsupported_texture_format");
        return;
    }
    const uint32_t mipCount = texture.GetMipLevelCount();
    const uint32_t baseWidth = texture.GetWidth();
    const uint32_t baseHeight = texture.GetHeight();
    if (mip >= mipCount || mip >= 32u || baseWidth == 0 || baseHeight == 0) {
        detail::latch_error(s, "invalid_texture_mip");
        return;
    }
    const uint32_t width = baseWidth >> mip;
    const uint32_t height = baseHeight >> mip;
    uint32_t bytesPerRow = 0;
    uint64_t paddedBytes = 0;
    uint64_t tightBytes = 0;
    if (!detail::checked_copy_size(width, height, &bytesPerRow, &paddedBytes, &tightBytes) ||
        paddedBytes > detail::kMaxPaddedBytes || s.paddedBytes > detail::kMaxPaddedBytes - paddedBytes) {
        detail::latch_error(s, "readback_budget");
        return;
    }

    const wgpu::BufferDescriptor descriptor{
        .label = "MP6 AO buffer capture readback",
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
        .size = paddedBytes,
    };
    wgpu::Buffer readback = device.CreateBuffer(&descriptor);
    if (!readback) {
        detail::latch_error(s, "buffer_create");
        return;
    }

    const wgpu::TexelCopyTextureInfo copySource{
        .texture = texture,
        .mipLevel = mip,
        .origin = wgpu::Origin3D{0, 0, 0},
    };
    const wgpu::TexelCopyBufferInfo copyDestination{
        .layout = wgpu::TexelCopyBufferLayout{.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = height},
        .buffer = readback,
    };
    const wgpu::Extent3D copySize{width, height, 1};
    encoder.CopyTextureToBuffer(&copySource, &copyDestination, &copySize);

    detail::Plane &plane = job.planes[job.planeCount];
    plane.spec = &detail::kStages[s.nextStage];
    plane.width = width;
    plane.height = height;
    plane.bytesPerRow = bytesPerRow;
    plane.paddedBytes = paddedBytes;
    plane.tightBytes = tightBytes;
    plane.format = format;
    plane.readback = std::move(readback);
    job.planeCount++;
    s.nextStage++;
    s.paddedBytes += paddedBytes;
}

inline void end_job() {
    detail::State &s = detail::state();
    if (!s.enabled || s.error) {
        s.active = false;
        s.ignoredSelection = false;
        return;
    }
    if (s.ignoredSelection) {
        s.ignoredSelection = false;
        return;
    }
    if (!s.active) {
        detail::latch_error(s, "end_without_job");
        return;
    }
    detail::Job &job = s.jobs[s.activeSlot];
    if (job.planeCount != detail::kPlanesPerJob || s.nextStage != detail::kPlanesPerJob)
        detail::latch_error(s, "plane_inventory");
    s.active = false;
    s.nextStage = 0;
}

inline void finish() {
    detail::State &s = detail::state();
    if (s.finished) return;
    s.finished = true;
    if (!s.initialized || !s.enabled) return;

    bool complete = false;
    try {
        if (s.active) detail::latch_error(s, "job_open_at_finish");
        if (!s.error && (s.jobsCount != detail::kMaxJobs || !s.jobs[0].begun || !s.jobs[1].begun ||
                         s.jobs[0].planeCount != detail::kPlanesPerJob ||
                         s.jobs[1].planeCount != detail::kPlanesPerJob))
            detail::latch_error(s, "job_inventory");
        if (!s.error) complete = detail::map_and_write(s);
    } catch (...) {
        detail::latch_error(s, "finish_exception");
        complete = false;
    }
    detail::unmap_all(s);

    uint32_t planes = 0;
    for (const detail::Job &job : s.jobs) planes += job.planeCount;
    std::printf("[AO-BUFFERS] jobs=%u planes=%u complete=%u bytes=%llu\n", s.jobsCount, planes,
                complete ? 1u : 0u, static_cast<unsigned long long>(s.paddedBytes));
    std::fflush(stdout);
}

} // namespace mp6::qa::ao_buffers
