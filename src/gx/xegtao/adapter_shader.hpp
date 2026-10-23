#pragma once

namespace mp6::xegtao {

// Integration only: the GTAO evaluation and denoising live in the unmodified
// upstream HLSL. Both adapters are raster passes, requiring no additional
// storage-format features from the native renderer.
inline constexpr char AdapterShader[] = R"wgsl(
struct AdapterParams {
    depth: vec4f, // upstream depth unpack mul, add, MP6 maximum darkening, unused
    viewport: vec4f,
    scissor: vec4f,
};
@group(0) @binding(0) var<uniform> p: AdapterParams;
@group(0) @binding(1) var cleanDepth: texture_2d<f32>;
@group(0) @binding(2) var finalAO: texture_2d<u32>;
@group(0) @binding(3) var packedNormals: texture_2d<u32>;

@vertex fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let xy = array<vec2f, 3>(vec2f(-1, 1), vec2f(-1, -3), vec2f(3, 1));
    return vec4f(xy[i], 0, 1);
}

fn receiver(pixel: vec2f) -> bool {
    let uv = pixel / vec2f(textureDimensions(cleanDepth));
    let lo = max(p.viewport.xy, p.scissor.xy);
    let hi = min(p.viewport.xy + p.viewport.zw, p.scissor.xy + p.scissor.zw);
    return all(uv >= lo) && all(uv < hi) && all(uv >= vec2f(0)) && all(uv < vec2f(1));
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

@fragment fn fs_canonical_depth(@builtin(position) pixel: vec4f) -> @location(0) f32 {
    let z = textureLoad(cleanDepth, vec2i(pixel.xy), 0).r;
    // The existing preparation already handled reversed Z, GX depth range,
    // MSAA sample zero and exact decal coverage. Zero is its clear sentinel.
    if (!(z > 0.0) || !receiver(pixel.xy)) { return 1.0; }
    return clamp(p.depth.y - p.depth.x / z, 0.0, 1.0);
}

@fragment fn fs_legacy_signal(@builtin(position) pixel: vec4f) -> @location(0) vec4f {
    let at = vec2i(pixel.xy);
    let z = textureLoad(cleanDepth, at, 0).r;
    if (!(z > 0.0) || !receiver(pixel.xy)) { return vec4f(1, 0, 0, 0); }
    let ao = f32(textureLoad(finalAO, at, 0).r) / 255.0;
    let packed = textureLoad(packedNormals, at, 0).r;
    let encoded = vec3f(f32(packed & 2047u) / 2047.0,
                        f32((packed >> 11u) & 2047u) / 2047.0,
                        f32((packed >> 22u) & 1023u) / 1023.0);
    let normal = normalize((encoded * 2.0 - 1.0) * vec3f(1, 1, -1));
    // This is MP6's existing artistic-strength control, after upstream AO.
    // No extra 1.5 factor: XeGTAO's last denoise pass already applies it.
    let obscurance = artistic_obscurance(1.0 - clamp(ao, 0.0, 1.0));
    return vec4f(1.0 - p.depth.z * obscurance, normal);
}
)wgsl";

} // namespace mp6::xegtao
