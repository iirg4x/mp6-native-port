#pragma once

#include <cstdint>
#include <cstring>

namespace mp6::xegtao {

enum class Quality { Low = 0, Medium = 1, High = 2, Ultra = 3 };
enum class Resolution { Full, Half };
enum class DepthStorage { Float32, Float16 };

// The PC build uses XeGTAO without a launch-time environment override. Android
// remains opt-in until its physical-device quality and cost gates are met.
#if defined(_WIN32) && !defined(__ANDROID__)
inline constexpr bool default_enabled = true;
#else
inline constexpr bool default_enabled = false;
#endif

inline bool requested(const char *overrideValue) {
    if (overrideValue && std::strcmp(overrideValue, "0") == 0) return false;
    if (overrideValue && std::strcmp(overrideValue, "1") == 0) return true;
    return default_enabled;
}

// Runtime policy. The original full-resolution reference path is
// High with two spatial denoise passes; selecting a preset never changes scale.
struct Policy {
    Quality quality = Quality::High;
    uint32_t denoisePasses = 2;
    Resolution resolution = Resolution::Full;
    // Storage only: canonical depth, normal reconstruction and AO arithmetic
    // remain FP32. Float16 requires an enabled device format feature.
    DepthStorage depthStorage = DepthStorage::Float32;
};

struct ParsedPolicy {
    Policy policy{};
    bool invalidQuality = false;
    bool invalidDenoise = false;
    bool invalidResolution = false;
    bool invalidDepthStorage = false;
};

constexpr bool valid_policy(const Policy &policy) {
    return (policy.quality == Quality::Low || policy.quality == Quality::Medium ||
            policy.quality == Quality::High || policy.quality == Quality::Ultra) &&
           (policy.denoisePasses >= 1 && policy.denoisePasses <= 3) &&
           (policy.resolution == Resolution::Full || policy.resolution == Resolution::Half) &&
           (policy.depthStorage == DepthStorage::Float32 || policy.depthStorage == DepthStorage::Float16);
}

constexpr DepthStorage supported_depth_storage(DepthStorage requested, bool r16StorageSupported) {
    return requested == DepthStorage::Float16 && r16StorageSupported ? DepthStorage::Float16 : DepthStorage::Float32;
}

constexpr const char *depth_storage_name(DepthStorage storage) {
    return storage == DepthStorage::Float16 ? "r16float" : "r32float";
}

struct WorkingSize { uint32_t width, height; };

// Ceil division must not overflow at UINT32_MAX. Zero-size inputs are clamped
// here for allocation safety; the caller still validates camera parameters.
constexpr WorkingSize working_size(uint32_t width, uint32_t height, Resolution resolution) {
    width = width ? width : 1;
    height = height ? height : 1;
    if (resolution == Resolution::Half)
        return {width / 2 + width % 2, height / 2 + height % 2};
    return {width, height};
}

// Parse supplied values without reading process state. Missing values preserve
// the reference policy; an invalid value falls back independently to its default.
inline ParsedPolicy parse_policy(const char *quality, const char *denoise,
                                 const char *resolution = nullptr, const char *depthStorage = nullptr) {
    ParsedPolicy result;
    if (quality) {
        if (std::strcmp(quality, "low") == 0) result.policy.quality = Quality::Low;
        else if (std::strcmp(quality, "medium") == 0) result.policy.quality = Quality::Medium;
        else if (std::strcmp(quality, "ultra") == 0) result.policy.quality = Quality::Ultra;
        else if (std::strcmp(quality, "high") != 0) result.invalidQuality = true;
    }
    if (denoise) {
        if (std::strcmp(denoise, "1") == 0) result.policy.denoisePasses = 1;
        else if (std::strcmp(denoise, "3") == 0) result.policy.denoisePasses = 3;
        else if (std::strcmp(denoise, "2") != 0) result.invalidDenoise = true;
    }
    if (resolution) {
        if (std::strcmp(resolution, "half") == 0) result.policy.resolution = Resolution::Half;
        else if (std::strcmp(resolution, "full") != 0) result.invalidResolution = true;
    }
    if (depthStorage) {
        if (std::strcmp(depthStorage, "r16float") == 0) result.policy.depthStorage = DepthStorage::Float16;
        else if (std::strcmp(depthStorage, "r32float") != 0) result.invalidDepthStorage = true;
    }
    return result;
}

constexpr const char *quality_name(Quality quality) {
    switch (quality) {
    case Quality::Low: return "Low";
    case Quality::Medium: return "Medium";
    case Quality::High: return "High";
    case Quality::Ultra: return "Ultra";
    }
    return "High";
}

constexpr const char *main_entry_point(Quality quality) {
    switch (quality) {
    case Quality::Low: return "CSGTAOLow";
    case Quality::Medium: return "CSGTAOMedium";
    case Quality::High: return "CSGTAOHigh";
    case Quality::Ultra: return "CSGTAOUltra";
    }
    return "CSGTAOHigh";
}

// The main stage writes texture zero. Every denoise stage swaps input/output;
// its last stage must still run for the upstream final visibility scaling.
constexpr uint32_t final_ao_index(const Policy &policy) {
    return policy.denoisePasses % 2;
}

} // namespace mp6::xegtao
