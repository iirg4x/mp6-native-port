#pragma once

namespace mp6::xegtao {

// Experimental half-resolution reconstruction; the native-resolution reference
// stays separate. Depth-aware four-tap bilateral upsampling follows the general
// approach used by Filament's surface_ambient_occlusion.fs, not copied source.
// https://github.com/google/filament/blob/main/shaders/src/surface_ambient_occlusion.fs
// This depth-only quality candidate still needs silhouette/thin-feature and
// representative-scene validation; it is not an accepted mobile quality mode.
inline constexpr char HalfCompositeShader[] = R"wgsl(
struct Params {
    projection: vec4f,
    viewport: vec4f,
    scissor: vec4f,
    depth: vec4f,
    sizes: vec4f,
};
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var cleanDepth: texture_2d<f32>;
@group(0) @binding(2) var finalAO: texture_2d<u32>;
@group(0) @binding(3) var lowDepth: texture_2d<f32>;
@group(0) @binding(5) var foliageColor: texture_2d<f32>;

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

fn receiver_inverse(at: vec2i, dimensions: vec2i) -> f32 {
    if (any(at < vec2i(0)) || any(at >= dimensions)) { return 0.0; }
    if (!inside((vec2f(at) + 0.5) / vec2f(dimensions))) { return 0.0; }
    let z = textureLoad(cleanDepth, at, 0).r;
    if (!positive_finite(z)) { return 0.0; }
    let inverse = 1.0 / z;
    if (!positive_finite(inverse)) { return 0.0; }
    return inverse;
}

fn receiver_slope(at: vec2i, axis: vec2i, centerInverse: f32, dimensions: vec2i) -> f32 {
    let minus = receiver_inverse(at - axis, dimensions);
    let plus = receiver_inverse(at + axis, dimensions);
    if (!(minus > 0.0) || !(plus > 0.0)) { return 0.0; }
    let backward = centerInverse - minus;
    let forward = plus - centerInverse;
    // A minmod slope preserves projected planes but chooses the flat side of
    // a discontinuity. Opposing slopes at thin features/local extrema return
    // zero, rather than predicting a fictitious surface across the feature.
    if ((backward > 0.0 && forward > 0.0) || (backward < 0.0 && forward < 0.0)) {
        return select(forward, backward, abs(backward) <= abs(forward));
    }
    return 0.0;
}

fn foliage_visibility(pixel: vec2f, visibility: f32) -> vec4f {
    let at=clamp(vec2i(pixel),vec2i(0),vec2i(textureDimensions(foliageColor))-1);
    // Preserve the original receiver contract: alpha marks water, and RGB
    // contains unshaded premultiplied foreground from the ordered replay.
    if (textureLoad(foliageColor,at,0).a > 0.0) { return vec4f(0,0,0,1); }
    let foreground=textureLoad(foliageColor,at,0).rgb;
    return vec4f(foreground*(1.0-visibility),visibility);
}

fn artistic_obscurance(obscurance: f32) -> f32 {
    // MP6 art response, not XeGTAO math: a nonexpansive C1 soft knee that
    // suppresses weak camera-independent seams without amplifying motion.
    // The deadzone removes the weak end of the signal before the same soft
    // knee is applied, keeping the response C1 at both boundaries.
    let signal = max(obscurance - 0.05, 0.0);
    let knee = min(signal, 0.1);
    return signal - knee + 5.0 * knee * knee;
}

@fragment fn fs_composite(@builtin(position) pixel: vec4f) -> @location(0) vec4f {
    let fullDimensions = textureDimensions(cleanDepth);
    let uv = pixel.xy / vec2f(fullDimensions);
    let center = textureLoad(cleanDepth, vec2i(pixel.xy), 0).r;
    if (!inside(uv) || !positive_finite(center)) { return vec4f(0,0,0,1); }
    let centerInverse = 1.0 / center;
    if (!positive_finite(centerInverse)) { return vec4f(0,0,0,1); }
    let receiverAt = vec2i(pixel.xy);
    // Four additional full-depth samples, once per receiver. Reproject its
    // local reciprocal-depth plane to each low tap before testing separation,
    // so steep continuous surfaces do not lose AO merely due to pixel spacing.
    let gradient = vec2f(receiver_slope(receiverAt, vec2i(1, 0), centerInverse, vec2i(fullDimensions)),
                         receiver_slope(receiverAt, vec2i(0, 1), centerInverse, vec2i(fullDimensions)));

    let lowDimensions = vec2i(textureDimensions(finalAO));
    let lowPosition = uv * vec2f(lowDimensions) - 0.5;
    let base = vec2i(floor(lowPosition));
    let fraction = fract(lowPosition);
    var weightedObscurance = 0.0;
    var total = 0.0;
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let at = base + vec2i(x, y);
            if (any(at < vec2i(0)) || any(at >= lowDimensions)) { continue; }
            let sampleUV = (vec2f(at) + 0.5) / vec2f(lowDimensions);
            if (!inside(sampleUV)) { continue; }
            let anchor = vec2i(floor(sampleUV * vec2f(fullDimensions)));
            let anchorUV = (vec2f(anchor) + 0.5) / vec2f(fullDimensions);
            if (!inside(anchorUV)) { continue; }
            // MIP0 contains linear far depth for canonical clear. Verify the
            // original anchor coverage instead of mistaking that for geometry.
            if (!positive_finite(textureLoad(cleanDepth, anchor, 0).r)) { continue; }
            let sampleDepth = textureLoad(lowDepth, at, 0).r;
            if (!positive_finite(sampleDepth)) { continue; }
            let offset = sampleUV * vec2f(fullDimensions) - pixel.xy;
            let predictedInverse = centerInverse + dot(offset, gradient);
            if (!positive_finite(predictedInverse)) { continue; }
            let predictedDepth = 1.0 / predictedInverse;
            if (!positive_finite(predictedDepth)) { continue; }
            let spatial = select(1.0 - fraction.x, fraction.x, x == 1) *
                          select(1.0 - fraction.y, fraction.y, y == 1);
            // Experimental world-space rejection distance: 10% of the current
            // 32-unit AO radius. This is integration policy, not XeGTAO math.
            let difference = (predictedDepth - sampleDepth) / 3.2;
            let weight = spatial * max(0.0, 1.0 - difference * difference);
            let ao = clamp(f32(textureLoad(finalAO, at, 0).r) / 255.0, 0.0, 1.0);
            weightedObscurance += weight * (1.0 - ao);
            total += weight;
        }
    }
    // Fade unsupported subpixels rather than magnifying a tiny surviving tap
    // into a dark silhouette. No valid support is explicitly neutral.
    let obscurance = weightedObscurance / max(total, 1e-6) * smoothstep(0.0, 0.35, total);
    // Final upstream denoise already includes output scaling. Apply MP6's
    // artistic strength exactly once; no additional upstream 1.5 multiplier.
    let visibility = 1.0 - p.depth.w * artistic_obscurance(obscurance);
    return foliage_visibility(pixel.xy, visibility);
}
)wgsl";

} // namespace mp6::xegtao
