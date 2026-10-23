#pragma once
#include <string>

namespace mp6::xegtao {

// Native-resolution integration only. Consume XeGTAO's final visibility in
// the existing EFB continuation, without an intermediate RGBA16F raster pass.
// The upstream evaluator and denoiser are unchanged. This path MUST NOT be
// used for reduced-resolution AO without a separately validated reconstruction.
inline constexpr char CompositeShader[] = R"wgsl(
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
    let at = vec2i(pixel.xy);
    if (!inside(pixel.xy / p.sizes.xy) || !(textureLoad(cleanDepth, at, 0).r > 0.0)) {
        return vec4f(0,0,0,1);
    }
    let ao = clamp(f32(textureLoad(finalAO, at, 0).r) / 255.0, 0.0, 1.0);
    // The final upstream denoise already applies its output scaling. Only
    // MP6's artistic-strength control belongs here, once, in full precision.
    let obscurance = artistic_obscurance(1.0 - ao);
    let visibility = 1.0 - p.depth.w * obscurance;
    return foliage_visibility(pixel.xy, visibility);
}
)wgsl";

// The Full-only preparation experiment uses 1 as clear and 0 as valid near
// depth. Keep the original view-Z shader available for baseline and fallback.
inline std::string composite_shader(bool canonicalDepth = false) {
    std::string shader = CompositeShader;
    if (canonicalDepth) {
        const std::string predicate = "textureLoad(cleanDepth, at, 0).r > 0.0";
        shader.replace(shader.find(predicate), predicate.size(), "textureLoad(cleanDepth, at, 0).r < 1.0");
    }
    return shader;
}

} // namespace mp6::xegtao
