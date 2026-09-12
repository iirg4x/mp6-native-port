@fragment fn fs_composite(@builtin(position) pixel: vec4f) -> @location(0) vec4f {
    let uv = pixel.xy / p.sizes.xy;

    let z = linear_depth(uv);
    if (z == 0.0) { return vec4f(0,0,0,1); }

    // Native-resolution GTAO is already filtered at the exact receiver pixel.
    // Do not soften its edges with a redundant bilinear reconstruction.
    if (all(p.sizes.xy == p.sizes.zw)) {

        return foliage_visibility(pixel.xy,textureLoad(obscurance, vec2i(pixel.xy), 0).r);
    }
    let at = uv * p.sizes.zw - 0.5;
    let base = vec2i(floor(at));
    let fraction = fract(at);
    var samples: array<vec4f,4>;
    var coordinates: array<vec2i,4>;
    var anyShading = false;
    for (var y = 0; y <= 1; y++) {
        for (var x = 0; x <= 1; x++) {
            let index = y*2+x;
            coordinates[index] = clamp(base + vec2i(x,y),vec2i(0),vec2i(p.sizes.zw)-1);
            samples[index] = textureLoad(obscurance,coordinates[index],0);
            anyShading = anyShading || samples[index].r < 1.0;
        }
    }
    // A zero signal cannot contribute after any bilateral weighting. Avoid
    // eight normal-depth taps, four neighbor-depth taps and foliage sampling
    // over the large unoccluded parts of the board. No threshold/quality loss.
    if (!anyShading) { return vec4f(0,0,0,1); }

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
            let q = coordinates[y*2+x];
            let sample = samples[y*2+x];
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
