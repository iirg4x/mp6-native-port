"""Compare exact AO fast paths against the frozen compatibility implementation."""
import hashlib
import re
import unittest
from test_ao_gpu import Renderer, ROOT, plane, feet_scene, np, load_shader_source
from test_ao_horizon_integral import general_open_integral

REFERENCE_BLUR = '''fn blur(pixel: vec2f, axis: vec2i) -> vec4f {
    let at = vec2i(pixel);
    let value = textureLoad(obscurance, at, 0);
    let uv = ao_uv(at);
    let z = linear_depth(uv);
    if (z == 0.0 || dot(value.gba, value.gba) < 0.5) { return value; }
    let center = position(uv, z);
    var weighted = 0.0;
    var total = 0.0;
    for (var i = -3; i <= 3; i++) {
        let q = clamp(at + axis * i, vec2i(0), vec2i(p.sizes.zw) - 1);
        let sample = textureLoad(obscurance, q, 0);
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

'''

def reference_shader():
    source = re.sub(r'    // BEGIN SCALAR ZERO-SIGNAL CHECK.*?    // END SCALAR ZERO-SIGNAL CHECK\n',
                    '',load_shader_source(compatibility=True),flags=re.S)
    start = source.index('@fragment fn fs_composite(')
    end = source.index('\n}', start) + 2
    reference = (ROOT/'tests/native/ao_composite_reference.wgsl').read_text().rstrip()
    return source[:start] + reference + source[end:]

class ExactFastPaths(unittest.TestCase):
    def test_water_exclusion_matches_reference_at_all_receiver_pixels(self):
        w,h=511,257
        ground,_=plane(w,h)
        y,x=np.indices((h,w))
        z=ground-35*((x//16+y//16)%2)
        z[:8,:]=0
        water=(x>w//2)&(y%7!=0)&(x%11!=0)
        coverage=np.zeros((h,w,4),np.float32)
        # Colored, partly transparent foliage and minimum surviving water
        # coverage share the target. Neither can be tinted by early rejection.
        coverage[...,:3]=np.array([.1,.4,.05])*((x%13<7)&(y%9<5))[...,None]
        coverage[...,3]=water/255.
        scene=np.full((h,w,4),.75,np.float32)
        scene[...,3]=.375
        for compatibility in (False,True):
            for prepared in (False,True):
                source=load_shader_source(compatibility=compatibility,prepared_depth=prepared)
                start=source.index('@fragment fn fs_composite(')
                end=source.index('\n}',start)+2
                reference=(ROOT/'tests/native/ao_composite_reference.wgsl').read_text().rstrip()
                before=Renderer(mobile=True,prepared_depth=prepared,
                                shader_source=source[:start]+reference+source[end:])
                after=Renderer(mobile=True,prepared_depth=prepared,shader_source=source)
                for reverse in (False,True):
                    for scale in (1.,.5):
                        args=dict(ao_scale=scale,reversed_z=reverse,depth_range=(.1,.8),
                                  scissor=(.02,.03,.94,.92),foliage=coverage,scene=scene)
                        if prepared: args.update(decal_before=z,decal_after=z)
                        expected=before.render(z,**args)
                        actual=after.render(z,**args)
                        for a,b in zip(actual,expected): np.testing.assert_array_equal(a,b)
                        np.testing.assert_array_equal(actual[-1][water],scene[water])

    def test_compatibility_restores_original_shader_source(self):
        tokens=lambda text: re.sub(r'\s+','',text)
        android=reference_shader()
        # Checked against the frozen 0.4.20 release checkpoint, including
        # the existing unused denoise entry points appended to the module.
        self.assertEqual(hashlib.sha256(tokens(android).encode()).hexdigest(),
                         '495cb06b4d8526bf12b18723297d21ff57d265ad4b6c9a0f9f565a41ef606e43')
        blur=android[android.index('fn blur('):android.index('@fragment fn fs_blur_x')]
        self.assertEqual(tokens(blur), tokens(REFERENCE_BLUR))
        self.assertNotIn('anyShading',blur)

    def test_scalar_compatibility_filter_preserves_exact_pixels(self):
        before=Renderer(mobile=True,shader_source=reference_shader())
        source=load_shader_source(compatibility=True)
        self.assertNotIn('array<vec4f,7>',source)
        after=Renderer(mobile=True,shader_source=source)
        for w,h in [(511,257),(1280,720),(2960,1848)]:
            ground,_=plane(w,h)
            feet,_=feet_scene(w,h)
            y,x=np.indices(ground.shape)
            for z in (ground,feet,ground-35*((x//16+y//16)%2)):
                for reverse in (False,True):
                    args=dict(reversed_z=reverse)
                    for reference,actual in zip(before.render(z,**args),after.render(z,**args)):
                        np.testing.assert_array_equal(actual,reference)

    def test_exact_pixels_at_native_reduced_and_odd_sizes(self):
        before=Renderer(mobile=True,shader_source=reference_shader())
        # Isolate the exact rejection/filter/composite fast paths from the
        # separately bounded analytic quadrature (test_ao_horizon_integral).
        after=Renderer(mobile=True,shader_source=general_open_integral(load_shader_source()))
        for w,h in [(511,257),(1280,720),(1920,1080),(2401,1081),(2960,1848)]:
            feet,_=feet_scene(w,h)
            ground,_=plane(w,h)
            raised,mask=plane(w,h,lift=18.)
            y,x=np.indices(ground.shape)
            mask &= (x%7!=0)&(y%11!=0)
            decal=np.where(mask,raised,ground)
            final=np.where(feet<ground-.001,feet,decal)
            for reverse in [False,True]:
                args=dict(decal_before=ground,decal_after=decal,reversed_z=reverse)
                expected=before.render(final,**args)
                actual=after.render(final,**args)
                for reference,result in zip(expected,actual):
                    np.testing.assert_array_equal(result,reference)

    def test_rejection_is_exact_on_dense_edges_clipping_and_depth_ranges(self):
        before=Renderer(shader_source=reference_shader())
        after=Renderer(shader_source=general_open_integral(load_shader_source(compatibility=False)))
        w,h=511,257
        y,x=np.indices((h,w))
        ground,_=plane(w,h)
        scenes=[np.zeros((h,w)),ground,ground-35*((x//16+y//16)%2)]
        for z in scenes:
            for reverse in (False,True):
                for scale in (1.,.5):
                    args=dict(reversed_z=reverse,ao_scale=scale,depth_range=(.1,.8),
                              viewport=(.03,.05,.9,.9),scissor=(.15,.1,.7,.8))
                    for reference,actual in zip(before.render(z,**args),after.render(z,**args)):
                        np.testing.assert_array_equal(actual,reference)

if __name__=='__main__': unittest.main(verbosity=2)
