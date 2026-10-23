#pragma once

// XeGTAO's clamp helper uses assert(), so this must precede the upstream header.
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "policy.hpp"

#include "../../../third_party/xegtao/XeGTAO.h"

static_assert(sizeof(XeGTAO::GTAOConstants) == 96,
              "XeGTAO constants must retain the shader ABI");
static_assert(offsetof(XeGTAO::GTAOConstants, DepthUnpackConsts) == 16,
              "XeGTAO depth constants ABI changed");
static_assert(offsetof(XeGTAO::GTAOConstants, NDCToViewMul) == 32,
              "XeGTAO NDC multiplier ABI changed");
static_assert(offsetof(XeGTAO::GTAOConstants, NDCToViewMul_x_PixelSize) == 48,
              "XeGTAO pixel multiplier ABI changed");
static_assert(offsetof(XeGTAO::GTAOConstants, EffectRadius) == 56,
              "XeGTAO radius ABI changed");
static_assert(offsetof(XeGTAO::GTAOConstants, NoiseIndex) == 92,
              "XeGTAO noise index ABI changed");

namespace mp6::xegtao {

namespace detail {

inline bool finite(float value) {
    return std::isfinite(value);
}

inline bool finite_constants(const XeGTAO::GTAOConstants &constants) {
    const float values[] = {
        constants.ViewportPixelSize.x,
        constants.ViewportPixelSize.y,
        constants.DepthUnpackConsts.x,
        constants.DepthUnpackConsts.y,
        constants.CameraTanHalfFOV.x,
        constants.CameraTanHalfFOV.y,
        constants.NDCToViewMul.x,
        constants.NDCToViewMul.y,
        constants.NDCToViewAdd.x,
        constants.NDCToViewAdd.y,
        constants.NDCToViewMul_x_PixelSize.x,
        constants.NDCToViewMul_x_PixelSize.y,
        constants.EffectRadius,
        constants.EffectFalloffRange,
        constants.RadiusMultiplier,
        constants.Padding0,
        constants.FinalValuePower,
        constants.DenoiseBlurBeta,
        constants.SampleDistributionPower,
        constants.ThinOccluderCompensation,
        constants.DepthMIPSamplingOffset,
    };
    for (float value : values) {
        if (!finite(value))
            return false;
    }
    return true;
}

} // namespace detail

// Populate a standalone host-side XeGTAO constant block. The normalized
// viewport is [x, y, width, height] in full-EFB UV coordinates; width and
// height are the dimensions of the working AO image. The output is unchanged
// when validation fails.
inline bool fill_parameters(
    std::int64_t width,
    std::int64_t height,
    float tanHalfFovY,
    float aspect,
    float nearPlane,
    float farPlane,
    const float normalizedViewport[4],
    XeGTAO::GTAOConstants &output,
    float radius = 32.0f,
    const Policy &policy = {}) {
    constexpr std::int64_t kMaxViewportDimension =
        static_cast<std::int64_t>(std::numeric_limits<int>::max());

    if (width <= 0 || height <= 0 || width > kMaxViewportDimension ||
        height > kMaxViewportDimension || normalizedViewport == nullptr ||
        !valid_policy(policy))
        return false;
    if (!detail::finite(tanHalfFovY) || !detail::finite(aspect) ||
        !detail::finite(nearPlane) || !detail::finite(farPlane) ||
        !detail::finite(radius))
        return false;
    if (!(tanHalfFovY > 0.0f) || !(aspect > 0.0f) || !(nearPlane > 0.0f) ||
        !(farPlane > nearPlane) || !(radius > 0.0f))
        return false;

    for (int index = 0; index < 4; ++index) {
        if (!detail::finite(normalizedViewport[index]))
            return false;
    }
    if (!(normalizedViewport[2] > 0.0f) ||
        !(normalizedViewport[3] > 0.0f))
        return false;

    // XeGTAOUpdateConstants expects a row-major projection with +Z as the
    // canonical forward direction. These are the entries used by its
    // depth-linearization code (XeGTAO.h, GTAOUpdateConstants).
    const float tanHalfFovX = tanHalfFovY * aspect;
    const float depthRange = farPlane - nearPlane;
    const float depthScale = farPlane / depthRange;
    const float depthOffset = -(nearPlane * farPlane) / depthRange;
    const float projectionX = 1.0f / tanHalfFovX;
    const float projectionY = 1.0f / tanHalfFovY;
    const float projection[] = {
        projectionX, 0.0f, 0.0f, 0.0f,
        0.0f, projectionY, 0.0f, 0.0f,
        0.0f, 0.0f, depthScale, 1.0f,
        0.0f, 0.0f, depthOffset, 0.0f,
    };
    for (float value : projection) {
        if (!detail::finite(value))
            return false;
    }

    XeGTAO::GTAOSettings settings;
    settings.Radius = radius;
    settings.QualityLevel = static_cast<int>(policy.quality);
    settings.DenoisePasses = static_cast<int>(policy.denoisePasses);

    XeGTAO::GTAOConstants constants{};
    XeGTAO::GTAOUpdateConstants(
        constants,
        static_cast<int>(width),
        static_cast<int>(height),
        settings,
        projection,
        true,
        0U);

    const float viewportWidth = normalizedViewport[2];
    const float viewportHeight = normalizedViewport[3];
    const float tanHalfFovXFromProjection = constants.CameraTanHalfFOV.x;
    const float tanHalfFovYFromProjection = constants.CameraTanHalfFOV.y;
    const float mulX = 2.0f * tanHalfFovXFromProjection / viewportWidth;
    const float mulY = -2.0f * tanHalfFovYFromProjection / viewportHeight;
    constants.NDCToViewMul = {mulX, mulY};
    constants.NDCToViewAdd = {
        -tanHalfFovXFromProjection - mulX * normalizedViewport[0],
        tanHalfFovYFromProjection - mulY * normalizedViewport[1],
    };
    constants.NDCToViewMul_x_PixelSize = {
        mulX * constants.ViewportPixelSize.x,
        mulY * constants.ViewportPixelSize.y,
    };
    // This helper is deliberately the non-TAA path. Do not inherit a future
    // caller's frame counter or enable temporal noise here.
    constants.NoiseIndex = 0;

    if (!detail::finite_constants(constants))
        return false;

    output = constants;
    return true;
}

} // namespace mp6::xegtao
