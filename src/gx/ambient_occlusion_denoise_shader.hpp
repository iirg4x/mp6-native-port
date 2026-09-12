#pragma once
#include <string_view>

inline std::string_view ambient_occlusion_denoise_shader() {
    return R"wgsl(
// Fused separable AO denoiser. Appended to the shared production WGSL.
// A 16x8 output tile plus a 3-pixel halo uses 10,756 bytes of workgroup memory.
@group(0) @binding(6) var denoised: texture_storage_2d<rgba16float, write>;
var<workgroup> tileSignal: array<vec4f, 308>;
var<workgroup> tilePosition: array<vec4f, 308>;
var<workgroup> horizontal: array<f32, 224>;
var<workgroup> tileHasShading: atomic<u32>;

fn tile_weight(center: vec3f, other: vec3f, normal: vec3f, otherNormal: vec3f, i: i32) -> f32 {
    let spatial = exp2(-f32(i * i) * 0.32);
    return spatial * surface_weight(center, other, normal) *
           surface_weight(other, center, otherNormal) *
           select(0.0, 1.0, other.z < 0.0 && dot(otherNormal, normal) > 0.9);
}

@compute @workgroup_size(16, 8)
fn cs_denoise(@builtin(workgroup_id) group: vec3u,
              @builtin(local_invocation_index) localIndex: u32,
              @builtin(local_invocation_id) local: vec3u) {
    let origin = vec2i(group.xy) * vec2i(16, 8) - vec2i(3);
    let limit = vec2i(p.sizes.zw) - 1;
    if (localIndex == 0u) { atomicStore(&tileHasShading, 0u); }
    workgroupBarrier();
    var anyShading = false;
    for (var index = localIndex; index < 308u; index += 128u) {
        let at = clamp(origin + vec2i(i32(index % 22u), i32(index / 22u)), vec2i(0), limit);
        tileSignal[index] = textureLoad(obscurance, at, 0);
        anyShading = anyShading || tileSignal[index].r != 1.0;
    }
    if (anyShading) { atomicOr(&tileHasShading, 1u); }
    workgroupBarrier();
    let shaded = atomicLoad(&tileHasShading) != 0u;
    if (shaded) {
        for (var index = localIndex; index < 308u; index += 128u) {
            let at = clamp(origin + vec2i(i32(index % 22u), i32(index / 22u)), vec2i(0), limit);
            let uv = ao_uv(at);
            let z = linear_depth(uv);
            tilePosition[index] = vec4f(position(uv, z), 0);
        }
    }
    workgroupBarrier();

    // All rows, including the vertical halo, need the complete horizontal pass.
    for (var index = localIndex; shaded && index < 224u; index += 128u) {
        let centerIndex = (index / 16u) * 22u + index % 16u + 3u;
        let value = tileSignal[centerIndex];
        let center = tilePosition[centerIndex].xyz;
        var result = value.r;
        if (center.z != 0.0 && dot(value.gba, value.gba) >= 0.5) {
            var weighted = 0.0;
            var total = 0.0;
            for (var i = -3; i <= 3; i++) {
                let sampleIndex = u32(i32(centerIndex) + i);
                let sample = tileSignal[sampleIndex];
                let other = tilePosition[sampleIndex].xyz;
                let w = tile_weight(center, other, value.gba, sample.gba, i);
                weighted += (1.0 - sample.r) * w;
                total += w;
            }
            result = 1.0 - weighted / max(total, 1e-6);
        }
        // Match the original RGBA16Float store between blur X and blur Y.
        horizontal[index] = quantizeToF16(result);
    }
    workgroupBarrier();

    let at = vec2i(group.xy) * vec2i(16, 8) + vec2i(local.xy);
    if (any(at > limit)) { return; }
    let centerIndex = (local.y + 3u) * 22u + local.x + 3u;
    if (!shaded) {
        textureStore(denoised, at, tileSignal[centerIndex]);
        return;
    }
    let centerHorizontal = (local.y + 3u) * 16u + local.x;
    let value = tileSignal[centerIndex];
    let center = tilePosition[centerIndex].xyz;
    var result = horizontal[centerHorizontal];
    if (center.z != 0.0 && dot(value.gba, value.gba) >= 0.5) {
        var weighted = 0.0;
        var total = 0.0;
        for (var i = -3; i <= 3; i++) {
            let sampleIndex = u32(i32(centerIndex) + i * 22);
            let sample = tileSignal[sampleIndex];
            let other = tilePosition[sampleIndex].xyz;
            let visibility = horizontal[u32(i32(centerHorizontal) + i * 16)];
            let w = tile_weight(center, other, value.gba, sample.gba, i);
            weighted += (1.0 - visibility) * w;
            total += w;
        }
        result = 1.0 - weighted / max(total, 1e-6);
    }
    textureStore(denoised, at, vec4f(result, value.gba));
}
)wgsl";
}
