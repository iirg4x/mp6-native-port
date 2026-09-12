#pragma once
#include <string>
#include "ambient_occlusion_denoise_shader.hpp"

/* Port GTAO: analytic horizon integration adapted from Jimenez et al. (2016)
 * and Intel XeGTAO. Scalar integration only, not the complete XeGTAO pipeline.
 * Intel's MIT notice is embedded below and shipped in res/licenses/XeGTAO.txt.
 * MP6 retains R32 depth, deterministic sampling and its dual-plane denoiser. */
inline std::string ambient_occlusion_shader(bool compatibility, bool preparedDepth = false,
                                          bool attachmentDepth = false, bool multisampledDepth = false,
                                          bool decals = true) {
std::string shader = R"wgsl(
// Copyright (C) 2016-2021, Intel Corporation
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
struct Params {
    projection: vec4f, // tan(fov/2), aspect, near, far
    viewport: vec4f,
    scissor: vec4f,
    depth: vec4f, // GX min, max, reversed Z, maximum darkening
    sizes: vec4f, // full width/height, AO width/height
};
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var sceneDepth: texture_2d<f32>;
// Visibility + view normal. NEVER store linear depth in a half-float channel:
// its quantization produces contour bands on the board's sloping surfaces.
@group(0) @binding(2) var obscurance: texture_2d<f32>;
@group(0) @binding(3) var decalBefore: texture_2d<f32>;
@group(0) @binding(4) var decalAfter: texture_2d<f32>;
@group(0) @binding(5) var foliageColor: texture_2d<f32>;

@vertex fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let xy = array<vec2f, 3>(vec2f(-1, 1), vec2f(-1, -3), vec2f(3, 1));
    return vec4f(xy[i], 0, 1);
}

// The game's depth/alpha tests produced this exact full-resolution coverage,
// including filtered low-alpha texels. Only remove a decal while its depth is
// STILL the visible depth: later characters/effects must never be erased.
// This is a private AO input, never written back to the game's depth buffer.
@fragment fn fs_decal_depth(@builtin(position) pixel: vec4f) -> @location(0) f32 {
    let at = vec2i(pixel.xy);
    let current = textureLoad(sceneDepth, at, 0).r;
    let before = textureLoad(decalBefore, at, 0).r;
    let after = textureLoad(decalAfter, at, 0).r;
)wgsl";
shader += preparedDepth ? R"wgsl(
    return prepare_depth(select(current, before, current == after && before != after));
)wgsl" : R"wgsl(
    return select(current, before, current == after && before != after);
)wgsl";
shader += R"wgsl(
}

struct FoliageSeed {
    @location(0) color: vec4f,
    @builtin(frag_depth) depth: f32,
};
@fragment fn fs_foliage_seed(@builtin(position) pixel: vec4f) -> FoliageSeed {
    return FoliageSeed(vec4f(0), textureLoad(sceneDepth,vec2i(pixel.xy),0).r);
}

fn foliage_visibility(pixel: vec2f, visibility: f32) -> vec4f {
    let at=clamp(vec2i(pixel),vec2i(0),vec2i(textureDimensions(foliageColor))-1);
    // Water's visible-fragment mask shares the existing full-resolution
    // coverage target. Do not blur it or apply terrain AO through its color.
    if (textureLoad(foliageColor,at,0).a > 0.0) { return vec4f(0,0,0,1); }
    let foreground=textureLoad(foliageColor,at,0).rgb;
    // Scene = background * transmission + premultiplied foreground.
    // ONE / SRC_ALPHA blending yields Scene*V + foreground*(1-V), preserving
    // the foreground exactly while shading only the background contribution.
    // Mixing V with transmission instead still darkens colored alpha edges.
    return vec4f(foreground*(1.0-visibility),visibility);
}

fn inside(uv: vec2f) -> bool {
    let lo = max(p.viewport.xy, p.scissor.xy);
    let hi = min(p.viewport.xy + p.viewport.zw, p.scissor.xy + p.scissor.zw);
    return all(uv >= lo) && all(uv < hi) && all(uv >= vec2f(0)) && all(uv < vec2f(1));
}

fn depth_uv(uv: vec2f) -> vec2f {
    return (floor(uv * p.sizes.xy) + 0.5) / p.sizes.xy;
}

)wgsl";
// Board decal cleanup already writes an R32 target. Convert there once, not
// at every horizon/normal/denoise/reconstruction tap. No extra pass, texture,
// precision reduction or change to the compatibility depth-copy schedule.
if (preparedDepth) shader += R"wgsl(
fn prepare_depth(raw: f32) -> f32 {
    let forward = select(raw, 1.0 - raw, p.depth.z > 0.5);
    let d = (forward - p.depth.x) / (p.depth.y - p.depth.x);
    if (d < 0.0 || d >= 0.999999) { return 0.0; }
    let n = p.projection.z;
    let f = p.projection.w;
    return n * f / (f - d * (f - n));
}
)wgsl";
shader += R"wgsl(
fn linear_depth(uv: vec2f) -> f32 {
    let at = clamp(vec2i(uv * p.sizes.xy), vec2i(0), vec2i(p.sizes.xy) - 1);
    let raw = textureLoad(sceneDepth, at, 0).r;
)wgsl";
shader += preparedDepth ? R"wgsl(
    if (!inside(uv)) { return 0.0; }
    return raw;
)wgsl" : R"wgsl(
    let forward = select(raw, 1.0 - raw, p.depth.z > 0.5);
    let d = (forward - p.depth.x) / (p.depth.y - p.depth.x);
    if (!inside(uv) || d < 0.0 || d >= 0.999999) { return 0.0; }
    let n = p.projection.z;
    let f = p.projection.w;
    return n * f / (f - d * (f - n));
)wgsl";
shader += R"wgsl(
}

fn position(uv: vec2f, z: f32) -> vec3f {
    let ndc = (uv - p.viewport.xy) / p.viewport.zw * 2.0 - 1.0;
    return vec3f(ndc * vec2f(p.projection.y, -1.0) * p.projection.x * z, -z);
}

fn derivative(uv: vec2f, z: f32, step: vec2f) -> vec3f {
    let za = linear_depth(uv - step);
    let zb = linear_depth(uv + step);
    let zaa = linear_depth(uv - 2.0 * step);
    let zbb = linear_depth(uv + 2.0 * step);
    // Reciprocal depth is linear on a projected plane. Compare extrapolation
    // error, not camera proximity (which selects silhouettes on slopes).
    let ea = select(1e20, abs(2.0 / max(za, 1e-6) - 1.0 / max(zaa, 1e-6) - 1.0 / z), za > 0.0 && zaa > 0.0);
    let eb = select(1e20, abs(2.0 / max(zb, 1e-6) - 1.0 / max(zbb, 1e-6) - 1.0 / z), zb > 0.0 && zbb > 0.0);
    let center = position(uv, z);
    return select(center - position(uv - step, za), position(uv + step, zb) - center, eb < ea);
}

fn surface_normal(uv: vec2f, z: f32, footprint: vec2f) -> vec3f {
    let center = position(uv, z);
    let vx = derivative(uv, z, vec2f(footprint.x / p.sizes.x, 0));
    let vy = derivative(uv, z, vec2f(0, footprint.y / p.sizes.y));
    var normal = cross(vx, vy);
    let normalLength = length(normal);
    if (normalLength < 0.000001) { return vec3f(0); }
    normal = normal / normalLength;
    return select(normal, -normal, dot(normal, -center) < 0.0);
}

fn horizon_arc(h: f32, n: f32) -> f32 {
    return (cos(n) + 2.0 * h * sin(n) - cos(2.0 * h - n)) * 0.25;
}
)wgsl";
if (!compatibility) shader += R"wgsl(

fn open_horizon_arc(n: f32, low: vec2f) -> f32 {
    // For a viewer-facing projected normal, the open endpoints are n +/- pi/2.
    // Integrate them directly; retain the general formula for FP excursions
    // outside that hemisphere at grazing angles.
    if (abs(n) > 1.570796326795) {
        let h = vec2f(-acos(clamp(low.y, -1.0, 1.0)), acos(clamp(low.x, -1.0, 1.0)));
        return horizon_arc(h.x, n) + horizon_arc(h.y, n);
    }
    return cos(n) + n * sin(n);
}
)wgsl";
shader += R"wgsl(

@fragment fn fs_ao(@builtin(position) pixel: vec4f) -> @location(0) vec4f {
    // Position and sampled depth MUST refer to the very same pixel center.
    // Subpixel UVs combined with nearest depth invent bumps even on a plane.
    let uv = depth_uv(pixel.xy / p.sizes.zw);
    let z = linear_depth(uv);
    if (z == 0.0) { return vec4f(1, 0, 0, 0); }
    let center = position(uv, z);
    let normal = surface_normal(uv, z, max(vec2f(1), ceil(p.sizes.xy / p.sizes.zw)));
    if (dot(normal, normal) < 0.5) { return vec4f(1, 0, 0, 0); }
    let radius = 32.0; // world units, independent of resolution/camera zoom
    let projectedRadius = min(vec2f(0.08), radius / (2.0 * z * p.projection.x) *
                               p.viewport.zw / vec2f(p.projection.y, 1.0));
    let view = normalize(-center);
    let pi = 3.14159265359;
    var occluded = 0.0;
    var unoccluded = 0.0;
    // Three hemisphere slices, four distances on either side (24 depth taps).
    // No animated noise: the port has no TAA history to remove it. Quadratic
    // spacing concentrates work at character contacts and small crevices.
    for (var slice = 0u; slice < 3u; slice++) {
        let angle = (f32(slice) + 0.5) * pi / 3.0;
        let direction = vec3f(cos(angle), sin(angle), 0);
        let axis = normalize(cross(direction, view));
        let tangent = normalize(cross(view, axis));
        let projected = normal - axis * dot(normal, axis);
        let projectedLength = length(projected);
        let n = atan2(dot(projected, tangent), dot(projected, view));
        let low = cos(vec2f(n + pi * 0.5, n - pi * 0.5));
        var horizon = low;
        let screenDirection = vec2f(direction.x, -direction.y) * projectedRadius;
        let minStep = 1.3 / max(length(screenDirection * p.sizes.xy), 1.3);
        for (var step = 0u; step < 4u; step++) {
            let t = (f32(step) + 0.5) / 4.0;
            let offset = screenDirection * (t * t + minStep);
            for (var side = 0u; side < 2u; side++) {
                let suv = depth_uv(uv + offset * select(1.0, -1.0, side == 1u));
                let sz = linear_depth(suv);
                if (sz > 0.0) {
                    let delta = position(suv, sz) - center;
)wgsl";
shader += compatibility ? R"wgsl(
                    let distance = length(delta);
                    // Short-range artistic bias suppresses authored joins;
                    // actual board decals are removed from AO depth separately.
                    let contact = smoothstep(2.0, 6.0, dot(normal, delta));
)wgsl" : R"wgsl(
                    // These samples have exactly zero weight in the original
                    // kernel. Reject them before square roots and integration.
                    let elevation = dot(normal, delta);
                    if (elevation <= 2.0) { continue; }
                    let distance = length(delta);
                    if (distance >= radius) { continue; }
                    let contact = smoothstep(2.0, 6.0, elevation);
)wgsl";
shader += R"wgsl(
                    let falloff = clamp((radius - distance) / (radius * 0.7), 0.0, 1.0);
                    let candidate = dot(delta, view) / max(distance, 1e-6);
                    horizon[side] = max(horizon[side], mix(low[side], candidate, falloff * contact));
                }
            }
        }
)wgsl";
shader += compatibility ? R"wgsl(
        let openAngles = vec2f(-acos(clamp(low.y, -1.0, 1.0)), acos(clamp(low.x, -1.0, 1.0)));
        let angles = vec2f(-acos(clamp(horizon.y, -1.0, 1.0)), acos(clamp(horizon.x, -1.0, 1.0)));
        let openArc = horizon_arc(openAngles.x, n) + horizon_arc(openAngles.y, n);
)wgsl" : R"wgsl(
        let openArc = open_horizon_arc(n, low);
        if (all(horizon == low)) {
            unoccluded += projectedLength * openArc;
            continue;
        }
        let angles = vec2f(-acos(clamp(horizon.y, -1.0, 1.0)), acos(clamp(horizon.x, -1.0, 1.0)));
)wgsl";
shader += R"wgsl(
        let visibleArc = horizon_arc(angles.x, n) + horizon_arc(angles.y, n);
        // Normalize against the same unoccluded slice quadrature. This avoids
        // darkening an entirely flat sloping map from a finite slice count.
        occluded += projectedLength * max(0.0, openArc - visibleArc);
        unoccluded += projectedLength * openArc;
    }
    let amount = clamp(1.7 * occluded / max(unoccluded, 1e-6), 0.0, 1.0);
    return vec4f(1.0 - p.depth.w * amount, normal);
}

fn ao_uv(at: vec2i) -> vec2f {
    return depth_uv((vec2f(at) + 0.5) / p.sizes.zw);
}

fn surface_weight(center: vec3f, other: vec3f, normal: vec3f) -> f32 {
    // Tangent-plane distance, not absolute Z: sloping ground must blur just
    // as well as a front-facing wall. Use the original R32 depth throughout.
    let tolerance = max(0.75, -center.z * 0.0005);
    let edge = max(0.0, 1.0 - abs(dot(normal, other - center)) / tolerance);
    return edge * edge;
}

fn blur(pixel: vec2f, axis: vec2i) -> vec4f {
    let at = vec2i(pixel);
    let value = textureLoad(obscurance, at, 0);
)wgsl";
// Keep compatibility devices free of the seven-vector dynamically indexed
// array. A scalar visibility scan can still skip the original bilateral math
// when every tap has exactly zero obscurance. No threshold or lost samples.
if (compatibility) shader += R"wgsl(
    // BEGIN SCALAR ZERO-SIGNAL CHECK
    if (value.r == 1.0) {
        var unshaded = true;
        for (var i = -3; i <= 3; i++) {
            let q = clamp(at + axis * i, vec2i(0), vec2i(p.sizes.zw) - 1);
            if (textureLoad(obscurance, q, 0).r != 1.0) {
                unshaded = false;
                break;
            }
        }
        if (unshaded) { return value; }
    }
    // END SCALAR ZERO-SIGNAL CHECK
)wgsl";
if (!compatibility) shader += R"wgsl(
    // No weighting can turn seven zero obscurance samples into a shadow.
    // Keep the exact kernel and precision, but avoid depth reconstruction and
    // bilateral geometry over unshaded regions (usually most of the board).
    var samples: array<vec4f,7>;
    var anyShading = false;
    for (var i = -3; i <= 3; i++) {
        let q = clamp(at + axis * i, vec2i(0), vec2i(p.sizes.zw) - 1);
        samples[i+3] = textureLoad(obscurance, q, 0);
        anyShading = anyShading || samples[i+3].r != 1.0;
    }
    if (!anyShading) { return value; }
)wgsl";
shader += R"wgsl(
    let uv = ao_uv(at);
    let z = linear_depth(uv);
    if (z == 0.0 || dot(value.gba, value.gba) < 0.5) { return value; }
    let center = position(uv, z);
    var weighted = 0.0;
    var total = 0.0;
    for (var i = -3; i <= 3; i++) {
        let q = clamp(at + axis * i, vec2i(0), vec2i(p.sizes.zw) - 1);
)wgsl";
shader += compatibility ? R"wgsl(        let sample = textureLoad(obscurance, q, 0);
)wgsl" : R"wgsl(        let sample = samples[i+3];
)wgsl";
shader += R"wgsl(
        let suv = ao_uv(q);
        let sz = linear_depth(suv);
        let spatial = exp2(-f32(i * i) * 0.32);
        let other = position(suv, sz);
        let w = spatial * surface_weight(center, other, value.gba) *
                surface_weight(other, center, sample.gba) *
                select(0.0, 1.0, sz > 0.0 && dot(sample.gba, value.gba) > 0.9);
        weighted += (1.0 - sample.r) * w;
        total += w;
    }
    return vec4f(1.0 - weighted / max(total, 1e-6), value.gba);
}

@fragment fn fs_blur_x(@builtin(position) pixel: vec4f) -> @location(0) vec4f {
    return blur(pixel.xy, vec2i(1, 0));
}
@fragment fn fs_blur_y(@builtin(position) pixel: vec4f) -> @location(0) vec4f {
    return blur(pixel.xy, vec2i(0, 1));
}

@fragment fn fs_composite(@builtin(position) pixel: vec4f) -> @location(0) vec4f {
    let uv = pixel.xy / p.sizes.xy;
)wgsl";
if (compatibility) shader += R"wgsl(
    let z = linear_depth(uv);
    if (z == 0.0) { return vec4f(0,0,0,1); }
)wgsl";
shader += R"wgsl(
    // Native-resolution GTAO is already filtered at the exact receiver pixel.
    // Do not soften its edges with a redundant bilinear reconstruction.
    if (all(p.sizes.xy == p.sizes.zw)) {
)wgsl";
if (!compatibility) shader += R"wgsl(
        let z = linear_depth(uv);
        if (z == 0.0) { return vec4f(0,0,0,1); }
)wgsl";
shader += R"wgsl(
        return foliage_visibility(pixel.xy,textureLoad(obscurance, vec2i(pixel.xy), 0).r);
    }
    let at = uv * p.sizes.zw - 0.5;
    let base = vec2i(floor(at));
    let fraction = fract(at);
)wgsl";
// Four fixed taps, not dynamically indexed private arrays. Keep the original
// tap order and arithmetic while avoiding mobile compiler array spills.
for (int i = 0; i < 4; ++i) {
    const auto n = std::to_string(i);
    shader += "    let q" + n + " = clamp(base + vec2i(" + std::to_string(i % 2) + "," +
              std::to_string(i / 2) + "),vec2i(0),vec2i(p.sizes.zw)-1);\n";
    shader += "    let s" + n + " = textureLoad(obscurance,q" + n + ",0);\n";
}
shader += R"wgsl(
    let anyShading = s0.r < 1.0 || s1.r < 1.0 || s2.r < 1.0 || s3.r < 1.0;
    // A zero signal cannot contribute after any bilateral weighting. Avoid
    // eight normal-depth taps, four neighbor-depth taps and foliage sampling
    // over the large unoccluded parts of the board. No threshold/quality loss.
    if (!anyShading) { return vec4f(0,0,0,1); }
)wgsl";
if (!compatibility) shader += R"wgsl(
    let z = linear_depth(uv);
    if (z == 0.0) { return vec4f(0,0,0,1); }
)wgsl";
shader += R"wgsl(
    let center = position(uv, z);
    // Validate BOTH surfaces at silhouettes, including mobile upsampling.
    let normal = surface_normal(uv, z, vec2f(1));
    if (dot(normal, normal) < 0.5) { return vec4f(0,0,0,1); }
    var weighted = 0.0;
    var total = 0.0;
    // Symmetric bilinear reconstruction of the already-denoised signal.
    // Sample normals reject silhouette bleeding without Z contour bands.
    for (var y = 0; y <= 1; y++) {
        for (var x = 0; x <= 1; x++) {
            let q = clamp(base + vec2i(x,y),vec2i(0),vec2i(p.sizes.zw)-1);
            let sample = select(select(s0,s1,x==1),select(s2,s3,x==1),y==1);
            let suv = ao_uv(q);
            let sz = linear_depth(suv);
            let weight = mix(vec2f(1) - fraction, fraction, vec2f(f32(x), f32(y)));
            let other = position(suv, sz);
            let w = weight.x * weight.y * surface_weight(center, other, normal) *
                    surface_weight(other, center, sample.gba) *
                    smoothstep(0.8, 0.95, dot(normal, sample.gba)) *
                    select(0.0, 1.0, sz > 0.0 && dot(sample.gba, sample.gba) > 0.5);
            weighted += (1.0 - sample.r) * w;
            total += w;
        }
    }
    // Fade unsupported subpixels to original lighting instead of magnifying
    // a tiny surviving weight into a dark jagged silhouette.
    let visibility = 1.0 - weighted / max(total, 1e-6) * smoothstep(0.0, 0.35, total);
    return foliage_visibility(pixel.xy,visibility);
}
)wgsl";
shader += ambient_occlusion_denoise_shader();
// The ordered preparation pass can sample the original attachment directly.
// Sample zero exactly matches Aurora's existing single/MSAA depth snapshot.
// Only this pipeline uses the depth-typed binding; GTAO and reconstruction
// continue reading the same full-precision prepared R32 texture.
if (attachmentDepth) {
    const auto replace = [&](const std::string &from, const std::string &to) {
        size_t at = 0;
        while ((at = shader.find(from, at)) != std::string::npos) {
            shader.replace(at, from.size(), to);
            at += to.size();
        }
    };
    replace("var sceneDepth: texture_2d<f32>;", multisampledDepth ?
            "var sceneDepth: texture_depth_multisampled_2d;" : "var sceneDepth: texture_depth_2d;");
    replace("textureLoad(sceneDepth, at, 0).r", "textureLoad(sceneDepth, at, 0)");
    replace("textureLoad(sceneDepth,vec2i(pixel.xy),0).r", "textureLoad(sceneDepth,vec2i(pixel.xy),0)");
    if (!decals) {
        replace("    let before = textureLoad(decalBefore, at, 0).r;\n", "");
        replace("    let after = textureLoad(decalAfter, at, 0).r;\n", "");
        replace("select(current, before, current == after && before != after)", "current");
    }
}
return shader;
}
