#pragma once

namespace mp6::xegtao {

// Experimental half-resolution integration, not a replacement for the native
// resolution reference. XeGTAO's evaluator and denoiser remain unchanged.
// Nearest-anchor coverage deliberately does not dilate silhouettes or invent
// receivers for clear pixels. Subpixel geometry can consequently be missed.
inline constexpr char HalfAdapterShader[] = R"wgsl(
struct AdapterParams {
    depth: vec4f, // upstream depth unpack mul, add, MP6 maximum darkening, unused
    viewport: vec4f,
    scissor: vec4f,
};
@group(0) @binding(0) var<uniform> p: AdapterParams;
@group(0) @binding(1) var cleanDepth: texture_2d<f32>;

@vertex fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let xy = array<vec2f, 3>(vec2f(-1, 1), vec2f(-1, -3), vec2f(3, 1));
    return vec4f(xy[i], 0, 1);
}

fn inside(uv: vec2f) -> bool {
    let lo = max(p.viewport.xy, p.scissor.xy);
    let hi = min(p.viewport.xy + p.viewport.zw, p.scissor.xy + p.scissor.zw);
    return all(uv >= lo) && all(uv < hi) && all(uv >= vec2f(0)) && all(uv < vec2f(1));
}

fn positive_finite(value: f32) -> bool {
    return value > 0.0 && value < 3.4e38;
}

// Zero means invalid here as in the full-resolution prepared depth. Check the
// mask at the sample center, not merely at the destination's low-resolution UV.
fn inverse_depth(at: vec2i) -> f32 {
    let dimensions = vec2i(textureDimensions(cleanDepth));
    if (any(at < vec2i(0)) || any(at >= dimensions)) { return 0.0; }
    if (!inside((vec2f(at) + 0.5) / vec2f(dimensions))) { return 0.0; }
    let z = textureLoad(cleanDepth, at, 0).r;
    if (!positive_finite(z)) { return 0.0; }
    let inverse = 1.0 / z;
    if (!positive_finite(inverse)) { return 0.0; }
    return inverse;
}

fn inverse_slope(at: vec2i, axis: vec2i, center: f32) -> f32 {
    let minus1 = inverse_depth(at - axis);
    let minus2 = inverse_depth(at - 2 * axis);
    let plus1 = inverse_depth(at + axis);
    let plus2 = inverse_depth(at + 2 * axis);
    let minusError = abs(2.0 * minus1 - minus2 - center);
    let plusError = abs(2.0 * plus1 - plus2 - center);
    // Candidate tolerance for roundoff and very small curvature. It is not an
    // upstream quality parameter and requires scene/fixture quality evidence.
    let tolerance = max(1e-8, abs(center) * 2e-4);
    let minusValid = minus1 > 0.0 && minus2 > 0.0 && minusError <= tolerance;
    let plusValid = plus1 > 0.0 && plus2 > 0.0 && plusError <= tolerance;
    if (minusValid && (!plusValid || minusError <= plusError)) {
        return center - minus1;
    }
    if (plusValid) { return plus1 - center; }
    return 0.0;
}

@fragment fn fs_canonical_depth(@builtin(position) pixel: vec4f) -> @location(0) f32 {
    let fullDimensions = textureDimensions(cleanDepth);
    let lowDimensions = (fullDimensions + vec2u(1)) / vec2u(2);
    // Fragment position already includes the half-pixel center. Adding another
    // half pixel here would misalign every downstream reconstruction sample.
    let uv = pixel.xy / vec2f(lowDimensions);
    if (!inside(uv)) { return 1.0; }
    let fullPosition = uv * vec2f(fullDimensions);
    let anchor = vec2i(floor(fullPosition));
    let center = inverse_depth(anchor);
    if (!(center > 0.0)) { return 1.0; }
    let offset = fullPosition - (vec2f(anchor) + 0.5);
    // Reciprocal depth is affine over a projected plane. Continuous one-sided
    // slopes recover depth at the exact low-pixel center, including odd sizes,
    // while rejecting interpolation across a depth step or a clear boundary.
    let slope = vec2f(inverse_slope(anchor, vec2i(1, 0), center),
                      inverse_slope(anchor, vec2i(0, 1), center));
    let inverse = center + dot(offset, slope);
    if (!positive_finite(inverse)) { return 1.0; }
    return clamp(p.depth.y - p.depth.x * inverse, 0.0, 1.0);
}
)wgsl";

} // namespace mp6::xegtao
