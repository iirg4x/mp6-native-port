"""Execute the production WGSL on synthetic depth, without the game or saves.

Optional QA dependency: pip install --target build/ao-gpu-test-deps wgpu==0.32.0
Run directly; this is not part of the CPU-only unittest discovery suite.
"""
import json
import math
import statistics
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build/ao-gpu-test-deps'))
sys.path.insert(0, str(ROOT / 'tests'))
from ao_shader_source import shader_source as load_shader_source
import numpy as np
import wgpu


class Renderer:
    def __init__(self, timing=False, mobile=False, shader_path=None, shader_source=None, inline_decals=False,
                  prepared_depth=False, tiled=False, attachment_depth=False,
                  depth_samples=1, attachment_decals=True):
        self.adapter = wgpu.gpu.request_adapter_sync(power_preference='high-performance')
        self.timing = timing
        self.mobile = mobile
        self.inline_decals = inline_decals
        self.prepared_depth = prepared_depth
        self.tiled = tiled
        self.attachment_depth = attachment_depth
        self.depth_samples = depth_samples
        self.attachment_decals = attachment_decals
        self.device = self.adapter.request_device_sync(required_features=['timestamp-query'] if timing else [])
        source = shader_source if shader_source is not None else load_shader_source(shader_path, android=mobile,
                                                                                  prepared_depth=prepared_depth)
        self.shader = self.device.create_shader_module(code=source)
        depth_shader = self.device.create_shader_module(code=load_shader_source(
            compatibility=mobile, prepared_depth=True, attachment_depth=True,
            multisampled_depth=depth_samples>1, decals=attachment_decals)) if attachment_depth else self.shader
        depth_bindings = (1,3,4) if not attachment_depth or attachment_decals else (1,)
        self.depth_layout = self.device.create_bind_group_layout(entries=[
            dict(binding=0, visibility=wgpu.ShaderStage.FRAGMENT, buffer=dict(type='uniform', min_binding_size=80)),
            *[dict(binding=i, visibility=wgpu.ShaderStage.FRAGMENT,
                   texture=dict(sample_type='depth' if attachment_depth and i==1 else 'unfilterable-float',
                                view_dimension='2d', multisampled=attachment_depth and i==1 and depth_samples>1))
               for i in depth_bindings]])
        self.depth_pipeline = self.device.create_render_pipeline(
            layout=self.device.create_pipeline_layout(bind_group_layouts=[self.depth_layout]),
            vertex=dict(module=depth_shader, entry_point='vs_main'),
            fragment=dict(module=depth_shader, entry_point='fs_decal_depth', targets=[dict(format='r32float')]),
            primitive=dict(topology='triangle-list'))
        self.layouts = []
        for bindings in ((1,), (1,2), (1,2,5)):
            if inline_decals:
                bindings = (*bindings, 3, 4)
            entries = [dict(binding=0, visibility=wgpu.ShaderStage.FRAGMENT,
                            buffer=dict(type='uniform', min_binding_size=80))]
            for i in bindings:
                entries.append(dict(binding=i, visibility=wgpu.ShaderStage.FRAGMENT,
                                    texture=dict(sample_type='unfilterable-float', view_dimension='2d')))
            self.layouts.append(self.device.create_bind_group_layout(entries=entries))
        self.pipelines = []
        for i, entry in enumerate(('fs_ao', 'fs_blur_x', 'fs_blur_y', 'fs_composite')):
            layout = self.device.create_pipeline_layout(bind_group_layouts=[self.layouts[2 if i==3 else int(i>0)]])
            self.pipelines.append(self.device.create_render_pipeline(
                layout=layout, vertex=dict(module=self.shader, entry_point='vs_main'),
                fragment=dict(module=self.shader, entry_point=entry, targets=[dict(format='rgba16float',
                    **(dict(blend=dict(color=dict(src_factor='one',dst_factor='src-alpha',operation='add'),
                                     alpha=dict(src_factor='zero',dst_factor='one',operation='add'))) if i==3 else {}))]),
                primitive=dict(topology='triangle-list')))
        if tiled:
            entries = [dict(binding=0,visibility=wgpu.ShaderStage.COMPUTE,buffer=dict(type='uniform',min_binding_size=80))]
            entries += [dict(binding=i,visibility=wgpu.ShaderStage.COMPUTE,
                             texture=dict(sample_type='unfilterable-float',view_dimension='2d')) for i in (1,2)]
            if inline_decals:
                entries += [dict(binding=i,visibility=wgpu.ShaderStage.COMPUTE,
                                 texture=dict(sample_type='unfilterable-float',view_dimension='2d')) for i in (3,4)]
            entries.append(dict(binding=6,visibility=wgpu.ShaderStage.COMPUTE,
                                storage_texture=dict(access='write-only',format='rgba16float',view_dimension='2d')))
            self.denoise_layout = self.device.create_bind_group_layout(entries=entries)
            self.denoise_pipeline = self.device.create_compute_pipeline(
                layout=self.device.create_pipeline_layout(bind_group_layouts=[self.denoise_layout]),
                compute=dict(module=self.shader,entry_point='cs_denoise'))

    def render(self, z, strength=.35, reversed_z=True, viewport=(0, 0, 1, 1),
               scissor=(0, 0, 1, 1), depth_range=(0, 1), ping_pong=False,
               override_filtered=None, decal_before=None, decal_after=None, ao_scale=None, repeat=1,
               foliage=None, scene=None, timing_samples=1):
        h, w = z.shape
        if self.prepared_depth:
            assert (decal_before is not None or self.attachment_depth) and not self.inline_decals
        scale = min(1., (960 if self.mobile else 1920) / max(w, h)) if ao_scale is None else ao_scale
        aw, ah = math.ceil(w*scale), math.ceil(h*scale)
        near, far = 20., 10000.
        def raw_depth(values):
            safe_z = np.maximum(values, near)
            raw = (far - near*far/safe_z) / (far-near)
            raw = np.where(values > 0, raw, 1.)
            raw = depth_range[0] + raw*(depth_range[1] - depth_range[0])
            if reversed_z:
                raw = 1. - raw
            return np.ascontiguousarray(raw, dtype=np.float32)
        def upload_depth(values):
            tex = self.device.create_texture(size=(w, h, 1), format='r32float',
                usage=wgpu.TextureUsage.COPY_DST | wgpu.TextureUsage.TEXTURE_BINDING)
            self.device.queue.write_texture(dict(texture=tex), raw_depth(values),
                dict(bytes_per_row=w*4, rows_per_image=h), (w, h, 1))
            return tex
        params = np.array([math.tan(math.radians(45)/2), w/h, near, far,
                           *viewport, *scissor, *depth_range, float(reversed_z), strength,
                           w, h, aw, ah], dtype=np.float32)
        uniform = self.device.create_buffer_with_data(data=params, usage=wgpu.BufferUsage.UNIFORM)
        depth = upload_depth(z)
        decal_views = None
        if self.inline_decals:
            before = upload_depth(z if decal_before is None else decal_before)
            after = upload_depth(z if decal_after is None else decal_after)
            decal_views = (before.create_view(), after.create_view())
        foreground=np.ascontiguousarray(np.zeros((h,w,4)) if foliage is None else foliage,dtype=np.float32)
        mask=self.device.create_texture(size=(w,h,1),format='rgba32float',
            usage=wgpu.TextureUsage.COPY_DST|wgpu.TextureUsage.TEXTURE_BINDING)
        self.device.queue.write_texture(dict(texture=mask),foreground,
            dict(bytes_per_row=w*16,rows_per_image=h),(w,h,1))
        textures = []
        encoder = self.device.create_command_encoder()
        if self.attachment_depth:
            # Real depth attachment, including deliberately different MSAA
            # samples. The previous Aurora snapshot selected sample zero.
            seed_shader = self.device.create_shader_module(code="""
                @group(0) @binding(0) var raw: texture_2d<f32>;
                @vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
                    let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
                    return vec4f(p[i],0,1);
                }
                @fragment fn fs(@builtin(position) p:vec4f, @builtin(sample_index) s:u32)
                    ->@builtin(frag_depth) f32 {
                    return clamp(textureLoad(raw,vec2i(p.xy),0).r+f32(s)*0.0001,0.0,1.0);
                }""")
            seed_layout=self.device.create_bind_group_layout(entries=[dict(binding=0,
                visibility=wgpu.ShaderStage.FRAGMENT,texture=dict(sample_type='unfilterable-float'))])
            seed_pipeline=self.device.create_render_pipeline(
                layout=self.device.create_pipeline_layout(bind_group_layouts=[seed_layout]),
                vertex=dict(module=seed_shader,entry_point='vs'),
                fragment=dict(module=seed_shader,entry_point='fs',targets=[]),
                depth_stencil=dict(format='depth32float',depth_write_enabled=True,depth_compare='always'),
                multisample=dict(count=self.depth_samples))
            seed_group=self.device.create_bind_group(layout=seed_layout,entries=[dict(binding=0,resource=depth.create_view())])
            attachment=self.device.create_texture(size=(w,h,1),format='depth32float',sample_count=self.depth_samples,
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.TEXTURE_BINDING)
            p=encoder.begin_render_pass(color_attachments=[],depth_stencil_attachment=dict(view=attachment.create_view(),
                depth_load_op='clear',depth_store_op='store',depth_clear_value=0))
            p.set_pipeline(seed_pipeline);p.set_bind_group(0,seed_group);p.draw(3);p.end()
            depth=attachment
        prepare = (decal_before is not None or self.attachment_depth) and not self.inline_decals
        measured = max(1, min(timing_samples, repeat))
        active_passes = ([0,1,3] if self.tiled else [0,1,2,3]) + ([4] if prepare else [])
        query_stride = len(active_passes)*2
        query = self.device.create_query_set(type='timestamp', count=query_stride*measured) if self.timing else None
        def stamps(index, iteration):
            slot = iteration-(repeat-measured)
            query_index = active_passes.index(index)*2
            return dict(timestamp_writes=dict(query_set=query, beginning_of_pass_write_index=slot*query_stride+query_index,
                                             end_of_pass_write_index=slot*query_stride+query_index+1)) if query and slot>=0 else {}
        draws = []
        if prepare:
            before, after = upload_depth(z if decal_before is None else decal_before), upload_depth(z if decal_after is None else decal_after)
            clean = self.device.create_texture(size=(w, h, 1), format='r32float',
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_SRC)
            group = self.device.create_bind_group(layout=self.depth_layout, entries=[
                dict(binding=0, resource=dict(buffer=uniform, size=80)),
                dict(binding=1, resource=depth.create_view()),
                *([dict(binding=3, resource=before.create_view()),
                   dict(binding=4, resource=after.create_view())]
                  if not self.attachment_depth or self.attachment_decals else [])])
            draws.append((4, self.depth_pipeline, group, clean.create_view()))
            render = encoder.begin_render_pass(**stamps(4, 0), color_attachments=[dict(view=clean.create_view(),
                clear_value=(0, 0, 0, 0), load_op='clear', store_op='store')])
            render.set_pipeline(self.depth_pipeline)
            render.set_bind_group(0, group)
            render.draw(3)
            render.end()
            depth = clean
        for i, pipeline in enumerate(self.pipelines):
            if self.tiled and i == 2:
                textures.append(textures[-1])
                continue
            width, height = (w, h) if i == 3 else (aw, ah)
            if i == 2 and ping_pong:
                target = textures[0]
            else:
                target = self.device.create_texture(size=(width, height, 1), format='rgba16float',
                        usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_SRC | wgpu.TextureUsage.COPY_DST |
                              (wgpu.TextureUsage.STORAGE_BINDING if self.tiled and i==1 else 0))
            entries = [dict(binding=0, resource=dict(buffer=uniform, size=80)),
                       dict(binding=1, resource=depth.create_view())]
            if i:
                previous = textures[-1]
                if i == 3 and override_filtered is not None:
                    # Isolate reconstruction from AO generation: a valid dark
                    # background signal must not tint an unrelated foreground.
                    previous = self.device.create_texture(size=(aw, ah, 1), format='rgba16float',
                        usage=wgpu.TextureUsage.COPY_DST | wgpu.TextureUsage.TEXTURE_BINDING)
                    signal = np.ascontiguousarray(override_filtered, dtype=np.float16)
                    self.device.queue.write_texture(dict(texture=previous), signal,
                        dict(bytes_per_row=aw*8, rows_per_image=ah), (aw, ah, 1))
                entries.append(dict(binding=2, resource=previous.create_view()))
            if i==3: entries.append(dict(binding=5,resource=mask.create_view()))
            if decal_views:
                entries.extend(dict(binding=binding, resource=view)
                               for binding, view in zip((3, 4), decal_views))
            compute = self.tiled and i == 1
            if compute:
                entries.append(dict(binding=6,resource=target.create_view()))
                pipeline = self.denoise_pipeline
            group = self.device.create_bind_group(layout=self.denoise_layout if compute else self.layouts[2 if i==3 else int(i>0)], entries=entries)
            draws.append((i, pipeline, group, target.create_view()))
            if compute:
                render = encoder.begin_compute_pass(**stamps(i,0))
                render.set_pipeline(pipeline)
                render.set_bind_group(0,group)
                render.dispatch_workgroups(math.ceil(aw/16),math.ceil(ah/8))
                render.end()
                textures.append(target)
                continue
            if i==3 and scene is not None:
                self.device.queue.write_texture(dict(texture=target),np.ascontiguousarray(scene,dtype=np.float16),
                    dict(bytes_per_row=w*8,rows_per_image=h),(w,h,1))
            render = encoder.begin_render_pass(**stamps(i, 0), color_attachments=[dict(view=target.create_view(),
                     clear_value=(1, 1, 1, 1) if i==3 else (1, 0, 0, 0),
                     load_op='load' if i==3 and scene is not None else 'clear', store_op='store')])
            render.set_pipeline(pipeline)
            render.set_bind_group(0, group)
            render.draw(3)
            render.end()
            textures.append(target)
        # Benchmark resident resources in a continuous batch, not an idle GPU
        # between NumPy scene generation and large readbacks (which measures P8).
        for iteration in range(1, repeat):
            for index, pipeline, group, view in draws:
                if self.tiled and index == 1:
                    render = encoder.begin_compute_pass(**stamps(index,iteration))
                    render.set_pipeline(pipeline)
                    render.set_bind_group(0,group)
                    render.dispatch_workgroups(math.ceil(aw/16),math.ceil(ah/8))
                    render.end()
                    continue
                render = encoder.begin_render_pass(**stamps(index, iteration),
                    color_attachments=[dict(view=view, clear_value=(1, 1, 1, 1) if index==3 else (1, 0, 0, 0), load_op='clear', store_op='store')])
                render.set_pipeline(pipeline)
                render.set_bind_group(0, group)
                render.draw(3)
                render.end()
        if query:
            timing_buffer = self.device.create_buffer(size=8*query_stride*measured, usage=wgpu.BufferUsage.QUERY_RESOLVE | wgpu.BufferUsage.COPY_SRC)
            encoder.resolve_query_set(query, 0, query_stride*measured, timing_buffer, 0)
        self.device.queue.submit([encoder.finish()])
        if query:
            values = np.frombuffer(self.device.queue.read_buffer(timing_buffer), np.uint64)
            passes = 5 if decal_before is not None and not self.inline_decals else 4
            # Retain the historical pass slots; tiled denoise occupies blur X,
            # and the removed blur Y slot is zero. Never resolve unwritten slots.
            def elapsed(s,i):
                if i not in active_passes: return 0.
                at=s*query_stride+active_passes.index(i)*2
                return (int(values[at+1])-int(values[at]))/1e6
            self.last_timing_samples_ms = [[elapsed(s,i)
                                           for i in range(passes)] for s in range(measured)]
            self.last_timings_ms = [statistics.median(row[i] for row in self.last_timing_samples_ms)
                                   for i in range(passes)]
        if prepare:
            clean_raw = self.device.queue.read_texture(dict(texture=depth),
                dict(bytes_per_row=w*4, rows_per_image=h), (w, h, 1))
            self.last_clean_depth = np.frombuffer(clean_raw, np.float32).reshape(h, w).copy()
            expected = raw_depth(z) if decal_before is None else np.where(
                                (raw_depth(z) == raw_depth(decal_after)) &
                                (raw_depth(decal_before) != raw_depth(decal_after)),
                                raw_depth(decal_before), raw_depth(z))
            if self.prepared_depth:
                forward = 1.-expected if reversed_z else expected
                # Match subtraction of the uploaded f32 endpoints, rather than
                # subtracting Python doubles before converting the result.
                depth_span = np.float32(depth_range[1])-np.float32(depth_range[0])
                d = (forward-np.float32(depth_range[0]))/depth_span
                # The GPU contracts f-d*(f-n). Emulate that single rounding;
                # a NumPy multiply followed by subtraction loses far-Z bits.
                denominator=(float(np.float32(far))-d.astype(np.float64)*float(np.float32(far)-np.float32(near))).astype(np.float32)
                expected = np.where((d >= 0.) & (d < .999999),np.float32(near*far)/denominator,0.)
                # GPU division may use a reciprocal multiply. Bound its input
                # rounding before the far-plane subtraction amplifies it.
                delta = 2*np.abs(np.spacing(d))
                def reconstructed(at):
                    divisor=(float(np.float32(far))-at.astype(np.float64)*float(np.float32(far)-np.float32(near))).astype(np.float32)
                    return np.float32(near*far)/divisor
                valid=(d >= 0.) & (d < .999999)
                low=np.where(valid,reconstructed(d-delta),0.)
                high=np.where(valid,reconstructed(d+delta),0.)
                self_error=np.maximum(low-self.last_clean_depth,self.last_clean_depth-high)
                np.testing.assert_array_less(self_error, np.maximum(np.abs(expected)*2e-6,np.finfo(np.float32).tiny))
            else:
                np.testing.assert_array_equal(self.last_clean_depth, expected)
        result = []
        for tex in (textures[0], textures[2], textures[3]):
            size = tex.size
            raw = self.device.queue.read_texture(dict(texture=tex),
                     dict(bytes_per_row=size[0]*8, rows_per_image=size[1]), size)
            result.append(np.frombuffer(raw, np.float16).astype(np.float32).reshape(size[1], size[0], 4))
        return result


def plane(width=512, height=288, distance=400., lift=0.):
    y, x = np.mgrid[:height, :width]
    tan = math.tan(math.radians(45)/2)
    rx = ((x+.5)/width*2-1) * tan * width/height
    ry = -((y+.5)/height*2-1) * tan
    normal = np.array([.10, .65, .753325958])
    normal /= np.linalg.norm(normal)
    # Sloped plane with an optional slightly elevated rectangular decal.
    d = -normal[2]*distance
    denominator = rx*normal[0] + ry*normal[1] - normal[2]
    z = d/denominator
    decal = (np.abs(rx*z) < 65) & (np.abs(ry*z) < 45)
    z = np.where(decal, (d+lift)/denominator, z)
    return z, decal


def feet_scene(width=1024, height=576):
    """Two small rounded shoes resting one unit above a sloped ground plane."""
    z, _ = plane(width, height)
    y, x = np.mgrid[:height, :width]
    tan = math.tan(math.radians(45)/2)
    ray = np.stack((((x+.5)/width*2-1)*tan*width/height,
                    -((y+.5)/height*2-1)*tan, -np.ones_like(z)), axis=-1)
    normal = np.array([.10, .65, .753325958])
    normal /= np.linalg.norm(normal)
    across = np.cross(normal, [0, 1, 0])
    across /= np.linalg.norm(across)
    along = np.cross(normal, across)
    axes = np.stack((across, normal, along), axis=1)
    size = np.array([9., 7., 15.])
    local_ray = (ray @ axes)/size
    a = (local_ray*local_ray).sum(axis=-1)
    ground = ray*z[..., None]
    contact = np.zeros(z.shape, dtype=bool)
    for offset in (-12., 12.):
        base = np.array([0., 0., -400.]) + across*offset
        center = base + normal*8.
        local_origin = (-center @ axes)/size
        b = 2*(local_ray*local_origin).sum(axis=-1)
        c = (local_origin*local_origin).sum()-1
        disc = b*b-4*a*c
        hit = (-b-np.sqrt(np.maximum(disc, 0)))/(2*a)
        z = np.where((disc >= 0) & (hit > 0), np.minimum(z, hit), z)
        on_ground = (ground-base) @ axes
        contact |= (on_ground[..., 0]/17)**2 + (on_ground[..., 2]/23)**2 < 1.
    original, _ = plane(width, height)
    contact &= np.abs(original-z) < .0001
    return z, contact


class ShaderQuality(unittest.TestCase):
    tiled = False
    @classmethod
    def setUpClass(cls):
        cls.renderer = Renderer(tiled=cls.tiled)
        print('GPU:', json.dumps(dict(cls.renderer.adapter.info)), flush=True)

    def test_sloped_planes_have_no_banding_at_near_and_far_depth(self):
        for distance in (150., 400., 2400., 6000.):
            for shape in ((512, 288), (511, 257)):
                z, _ = plane(*shape, distance=distance)
                raw, filtered, output = self.renderer.render(z)
                for value in (raw, filtered, output):
                    self.assertTrue(np.isfinite(value).all())
                    self.assertTrue((value[..., 0] == 1).all())

    def test_high_resolution_cap_keeps_flat_lighting_and_real_contacts(self):
        z, _ = plane(3840, 2160, lift=14.)
        raw, filtered, output = self.renderer.render(z)
        self.assertEqual(raw.shape[:2], (1080, 1920))
        self.assertEqual(output.shape[:2], (2160, 3840))
        self.assertLess(float(output[..., 0].min()), .98)
        self.assertTrue((output[:100, :100, 0] == 1).all())
        self.assertTrue(np.isfinite(filtered).all())

    def test_mobile_budget_keeps_contacts_and_exact_decal_edges(self):
        mobile=Renderer(mobile=True,tiled=self.tiled)
        for width,height in ((1920,1080),(2401,1081)):
            ground,_=plane(width,height)
            raised,mask=plane(width,height,lift=18.)
            y,x=np.indices(ground.shape)
            mask &= (x%7!=0)&(y%11!=0)
            after=np.where(mask,raised,ground)
            feet,contact=feet_scene(width,height)
            final=np.where(feet<ground-.001,feet,after)
            raw,_,reference=mobile.render(feet)
            _,_,output=mobile.render(final,decal_before=ground,decal_after=after)
            self.assertEqual(raw.shape[1],960)
            np.testing.assert_array_equal(output,reference)
            self.assertGreater(float(np.quantile(1-output[...,0][contact],.9)),.05)
            self.assertTrue(np.isfinite(output).all())
            self.assertTrue((output[:40,:40,0]==1).all())

    def test_shallow_floating_decal_is_not_outlined(self):
        for lift in (.25, 1., 1.8):
            z, _ = plane(lift=lift)
            _, _, output = self.renderer.render(z)
            self.assertGreaterEqual(float(output[12:-12, 12:-12, 0].min()), .995)

    def test_real_contact_still_shades_and_strong_is_bounded(self):
        z, _ = plane(lift=14.)
        _, _, subtle = self.renderer.render(z, strength=.20)
        _, _, strong = self.renderer.render(z, strength=.35)
        self.assertLess(float(strong[..., 0].min()), .98)
        self.assertGreaterEqual(float(strong[..., 0].min()), .649)
        self.assertTrue((strong[..., 0] <= subtle[..., 0]+.001).all())
        self.assertTrue((strong[..., 0] <= 1).all())
        self.assertTrue((strong[..., 3] == 1).all())

    def test_reversed_z_and_nondefault_depth_range_agree(self):
        z, _ = plane(lift=14.)
        _, _, forward = self.renderer.render(z, reversed_z=False)
        _, _, reverse = self.renderer.render(z)
        _, _, ranged = self.renderer.render(z, depth_range=(.1, .8))
        self.assertLess(float(np.abs(forward-reverse).max()), .004)
        self.assertLess(float(np.abs(reverse-ranged).max()), .004)

    def test_clear_depth_and_scissor_are_unmodified(self):
        z, _ = plane(lift=14.)
        z[:, :100] = 0
        _, _, output = self.renderer.render(z, scissor=(.25, .25, .5, .5))
        mask = np.ones(z.shape, dtype=bool)
        mask[72:216, 128:384] = False
        self.assertTrue((output[mask] == 1).all())
        _, _, empty = self.renderer.render(np.zeros((128, 256)))
        self.assertTrue((empty == 1).all())

    def test_filter_removes_impulses_without_crossing_silhouettes(self):
        z, _ = plane(lift=14.)
        raw, filtered, output = self.renderer.render(z)
        def roughness(a):
            return float(np.abs(np.diff(a[..., 0], n=2, axis=1)).mean())
        self.assertLess(roughness(filtered), roughness(raw))
        self.assertGreaterEqual(float(filtered[..., 0].min()), float(raw[..., 0].min())-.001)
        self.assertTrue(np.isfinite(output).all())

    def test_ping_pong_matches_distinct_render_targets(self):
        z, _ = plane(lift=14.)
        _, _, reference = self.renderer.render(z)
        _, _, reused = self.renderer.render(z, ping_pong=True)
        self.assertTrue((reference == reused).all())

    def test_small_feet_have_visible_ground_contact(self):
        z, ground_contact = feet_scene()
        _, _, subtle = self.renderer.render(z, strength=.20)
        _, _, strong = self.renderer.render(z, strength=.35)
        subtle_contact = 1-subtle[..., 0][ground_contact]
        strong_contact = 1-strong[..., 0][ground_contact]
        print('Shoe contact: mean/p90/max darkening',
              [(float(a.mean()), float(np.quantile(a, .9)), float(a.max()))
               for a in (subtle_contact, strong_contact)], flush=True)
        self.assertGreater(float(np.quantile(subtle_contact, .9)), .025)
        self.assertGreater(float(np.quantile(strong_contact, .9)), .05)

    def test_grazing_background_cannot_leak_onto_foreground(self):
        z = np.full((128, 256), 450.)
        z[:, 63:193] = 400.
        signal = np.zeros((64, 128, 4), dtype=np.float32)
        foreground = z[1::2, 1::2] == 400
        signal[..., 0] = np.where(foreground, 1., .65)
        signal[..., 2] = ~foreground  # grazing leaf normal
        signal[..., 3] = foreground  # front-facing character normal
        _, _, output = self.renderer.render(z, override_filtered=signal, ao_scale=.5)
        self.assertTrue((output[8:-8, 63:193, 0] == 1).all(),
                        f'background leaked: {output[8:-8,63:193,0].min()}')

    def test_cutout_decal_has_exact_coverage_and_no_ao_halo(self):
        # Deliberately coarse star/cutout with 1px holes and detached fringe.
        # Test a MUCH larger authored gap than the generic contact tolerance.
        ground, _ = plane(511, 257)
        raised, _ = plane(511, 257, lift=18.)
        y, x = np.indices(ground.shape)
        shape = ((abs(x-255) + abs(y-128) < 85) | ((abs(x-255) < 16) & (abs(y-128) < 95)))
        shape &= (x % 7 != 0) & (y % 11 != 0)
        after = np.where(shape, raised, ground)
        for scale in (1., .5):
            for reverse in (False, True):
                _, _, output = self.renderer.render(after, decal_before=ground, decal_after=after,
                                                    reversed_z=reverse, ao_scale=scale)
                self.assertTrue((output[..., 0] == 1).all())

    def test_decal_removal_preserves_later_foreground_and_contact(self):
        ground, _ = plane(512, 288)
        raised, mask = plane(512, 288, lift=3.)
        after = np.where(mask, raised, ground)
        feet, contact = feet_scene(512, 288)
        foreground = feet < ground - .001
        final = np.where(foreground, feet, after)
        _, _, reference = self.renderer.render(feet)
        _, _, output = self.renderer.render(final, decal_before=ground, decal_after=after)
        np.testing.assert_array_equal(output, reference)
        self.assertGreater(float(np.quantile(1-output[..., 0][contact], .9)), .05)

    def test_empty_or_invisible_decal_bracket_changes_nothing(self):
        feet, _ = feet_scene(512, 288)
        _, _, reference = self.renderer.render(feet)
        _, _, output = self.renderer.render(feet, decal_before=feet, decal_after=feet)
        np.testing.assert_array_equal(output, reference)

    def test_foliage_color_preserves_filtered_edges_and_overlapping_leaves(self):
        z=np.full((128,256),400.)
        yy,xx=np.indices(z.shape)
        alpha=np.where((xx%8<4)&(yy%7!=0),1.,0.)
        alpha[:,120:136]=np.linspace(0,1,16)
        # Two colored layers against brown wood. Visibility-only masking can
        # pass a grayscale coverage test while leaking wood AO onto green grass.
        second=np.where((xx%13<6)&(yy%11<5),.4,0.)
        leaf=np.array([.15,.85,.08]); other=np.array([.65,.2,.7])
        wood=np.array([.7,.3,.1])
        foreground=leaf*alpha[...,None]*(1-second[...,None])+other*second[...,None]
        transmission=(1-alpha)*(1-second)
        scene=np.ones((*z.shape,4)); scene[...,:3]=wood*transmission[...,None]+foreground
        scene[...,3]=.375  # The AO blend must not alter scene alpha.
        foliage=np.zeros_like(scene); foliage[...,:3]=foreground
        for scale in (1.,.5):
            signal=np.zeros((int(128*scale),int(256*scale),4),dtype=np.float32)
            signal[...,0]=.65; signal[...,3]=1
            _,_,output=self.renderer.render(z,ao_scale=scale,override_filtered=signal,foliage=foliage,scene=scene)
            expected=wood*transmission[...,None]*.65+foreground
            np.testing.assert_allclose(output[8:-8,8:-8,:3],expected[8:-8,8:-8],atol=.001)
            np.testing.assert_array_equal(output[...,3],scene[...,3])
            old=scene[...,:3]*(1-.35*transmission[...,None])
            self.assertGreater(float(np.max(np.abs(old-expected))),.05)

    def test_water_mask_preserves_color_at_exact_pixels_on_desktop_and_mobile(self):
        z=np.full((129,257),400.)
        yy,xx=np.indices(z.shape)
        water=(xx>80)&(xx<190)&(yy%7!=0)&(xx%11!=0)
        scene=np.ones((*z.shape,4),dtype=np.float32)
        scene[...,:3]=[.125,.625,.875]
        scene[...,3]=.375
        coverage=np.zeros_like(scene)
        coverage[...,3]=water/255.  # Even the smallest surviving alpha is water.
        for renderer in (self.renderer,Renderer(mobile=True,tiled=self.tiled),Renderer(mobile=True,inline_decals=True,tiled=self.tiled)):
            for scale in (1.,.5):
                signal=np.zeros((math.ceil(129*scale),math.ceil(257*scale),4),dtype=np.float32)
                signal[...,0]=.65; signal[...,3]=1
                _,_,result=renderer.render(z,ao_scale=scale,override_filtered=signal,
                                          foliage=coverage,scene=scene)
                np.testing.assert_array_equal(result[water],scene[water])
                self.assertLess(float(result[~water,0].min()),.1)
                np.testing.assert_array_equal(result[...,3],scene[...,3])

    def test_foliage_depth_seed_rejects_grass_behind_opaque_geometry(self):
        device=self.renderer.device
        w,h=64,32
        y,x=np.indices((h,w))
        foreground=(x>=24)&(x<40)
        alpha=np.where(x%8<4,1.,0.).astype(np.float32)
        alpha[12:20,:]=.5
        shader=device.create_shader_module(code='''
            @group(0) @binding(0) var a:texture_2d<f32>;
            @vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
                let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
                return vec4f(p[i],0.5,1);
            }
            @fragment fn fs(@builtin(position) p:vec4f)->@location(0) vec4f {
                let alpha=textureLoad(a,vec2i(p.xy),0).r;
                if (alpha<1.0/255.0) { discard; }
                return vec4f(0.25,0.8,0.125,alpha);
            }''')
        alpha_texture=device.create_texture(size=(w,h,1),format='r32float',
            usage=wgpu.TextureUsage.COPY_DST|wgpu.TextureUsage.TEXTURE_BINDING)
        device.queue.write_texture(dict(texture=alpha_texture),alpha,dict(bytes_per_row=w*4),(w,h,1))
        alpha_layout=device.create_bind_group_layout(entries=[dict(binding=0,
            visibility=wgpu.ShaderStage.FRAGMENT,texture=dict(sample_type='unfilterable-float'))])
        alpha_group=device.create_bind_group(layout=alpha_layout,entries=[dict(binding=0,resource=alpha_texture.create_view())])
        uniform=device.create_buffer(size=80,usage=wgpu.BufferUsage.UNIFORM)
        for reverse in (False,True):
            raw=np.where(foreground,.3,.7).astype(np.float32)
            if reverse: raw=1-raw
            source=device.create_texture(size=(w,h,1),format='r32float',
                usage=wgpu.TextureUsage.COPY_DST|wgpu.TextureUsage.TEXTURE_BINDING)
            device.queue.write_texture(dict(texture=source),raw,dict(bytes_per_row=w*4),(w,h,1))
            depth=device.create_texture(size=(w,h,1),format='depth32float',usage=wgpu.TextureUsage.RENDER_ATTACHMENT)
            color=device.create_texture(size=(w,h,1),format='rgba8unorm',
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.COPY_SRC)
            seed=device.create_render_pipeline(layout=device.create_pipeline_layout(bind_group_layouts=[self.renderer.layouts[0]]),
                vertex=dict(module=self.renderer.shader,entry_point='vs_main'),
                fragment=dict(module=self.renderer.shader,entry_point='fs_foliage_seed',targets=[dict(format='rgba8unorm')]),
                depth_stencil=dict(format='depth32float',depth_write_enabled=True,depth_compare='always'))
            seed_group=device.create_bind_group(layout=self.renderer.layouts[0],entries=[
                dict(binding=0,resource=dict(buffer=uniform,size=80)),dict(binding=1,resource=source.create_view())])
            leaf=device.create_render_pipeline(layout=device.create_pipeline_layout(bind_group_layouts=[alpha_layout]),
                vertex=dict(module=shader,entry_point='vs'),
                fragment=dict(module=shader,entry_point='fs',targets=[dict(format='rgba8unorm',blend=dict(
                    color=dict(src_factor='src-alpha',dst_factor='one-minus-src-alpha',operation='add'),
                    alpha=dict(src_factor='zero',dst_factor='one',operation='add')))]),
                depth_stencil=dict(format='depth32float',depth_write_enabled=False,
                                   depth_compare='greater-equal' if reverse else 'less-equal'))
            encoder=device.create_command_encoder()
            render=encoder.begin_render_pass(color_attachments=[dict(view=color.create_view(),load_op='clear',store_op='store',clear_value=(0,0,0,0))],
                depth_stencil_attachment=dict(view=depth.create_view(),depth_load_op='clear',depth_store_op='store',depth_clear_value=0))
            render.set_pipeline(seed); render.set_bind_group(0,seed_group); render.draw(3)
            render.set_pipeline(leaf); render.set_bind_group(0,alpha_group); render.draw(3); render.end()
            device.queue.submit([encoder.finish()])
            pixels=np.frombuffer(device.queue.read_texture(dict(texture=color),dict(bytes_per_row=w*4),(w,h,1)),np.uint8).reshape(h,w,4)
            expected=np.where(foreground,0.,alpha)[...,None]*np.array([.25,.8,.125])
            np.testing.assert_allclose(pixels[...,:3]/255.,expected,atol=1/255.)

    def test_continuous_water_mask_keeps_foreground_depth_and_unshaded_water_color(self):
        device=self.renderer.device
        w,h=257,129
        y,x=np.indices((h,w))
        foreground=(x>=100)&(x<155)
        # Same private water TEV result as mp6AoFoliageMaskState: no texture
        # alpha test, constant 1 alpha, RGB off, depth test on, depth writes off.
        shader=device.create_shader_module(code='''
            @vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
                let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
                return vec4f(p[i],0.5,1);
            }
            @fragment fn fs()->@location(0) vec4f {return vec4f(0,0,0,1);}
        ''')
        uniform=device.create_buffer(size=80,usage=wgpu.BufferUsage.UNIFORM)
        for reverse in (False,True):
            raw=np.where(foreground,.3,.7).astype(np.float32)
            if reverse:raw=1-raw
            source=device.create_texture(size=(w,h,1),format='r32float',
                usage=wgpu.TextureUsage.COPY_DST|wgpu.TextureUsage.TEXTURE_BINDING)
            device.queue.write_texture(dict(texture=source),raw,dict(bytes_per_row=w*4),(w,h,1))
            depth=device.create_texture(size=(w,h,1),format='depth32float',usage=wgpu.TextureUsage.RENDER_ATTACHMENT)
            color=device.create_texture(size=(w,h,1),format='rgba8unorm',
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.COPY_SRC)
            seed=device.create_render_pipeline(layout=device.create_pipeline_layout(bind_group_layouts=[self.renderer.layouts[0]]),
                vertex=dict(module=self.renderer.shader,entry_point='vs_main'),
                fragment=dict(module=self.renderer.shader,entry_point='fs_foliage_seed',targets=[dict(format='rgba8unorm')]),
                depth_stencil=dict(format='depth32float',depth_write_enabled=True,depth_compare='always'))
            group=device.create_bind_group(layout=self.renderer.layouts[0],entries=[
                dict(binding=0,resource=dict(buffer=uniform,size=80)),dict(binding=1,resource=source.create_view())])
            water=device.create_render_pipeline(layout='auto',vertex=dict(module=shader,entry_point='vs'),
                fragment=dict(module=shader,entry_point='fs',targets=[dict(format='rgba8unorm',write_mask=wgpu.ColorWrite.ALPHA)]),
                depth_stencil=dict(format='depth32float',depth_write_enabled=False,
                                   depth_compare='greater-equal' if reverse else 'less-equal'))
            encoder=device.create_command_encoder()
            p=encoder.begin_render_pass(color_attachments=[dict(view=color.create_view(),load_op='clear',store_op='store',clear_value=(0,0,0,0))],
                depth_stencil_attachment=dict(view=depth.create_view(),depth_load_op='clear',depth_store_op='store',depth_clear_value=0))
            p.set_pipeline(seed);p.set_bind_group(0,group);p.draw(3)
            p.set_pipeline(water);p.draw(3);p.end()
            device.queue.submit([encoder.finish()])
            pixels=np.frombuffer(device.queue.read_texture(dict(texture=color),dict(bytes_per_row=w*4),(w,h,1)),np.uint8).reshape(h,w,4)
            np.testing.assert_array_equal(pixels[...,3],np.where(foreground,0,255))
            np.testing.assert_array_equal(pixels[...,:3],0)
            signal=np.zeros((h,w,4));signal[...,0]=.65;signal[...,3]=1
            scene=np.ones((h,w,4));scene[...,:3]=[.125,.625,.875];scene[...,3]=.375
            _,_,result=self.renderer.render(np.full((h,w),400.),foliage=pixels/255.,
                                            override_filtered=signal,scene=scene)
            np.testing.assert_array_equal(result[~foreground],scene[~foreground])
            self.assertLess(float(result[foreground,0].min()),.1)
            np.testing.assert_array_equal(result[...,3],scene[...,3])


if __name__ == '__main__':
    unittest.main(verbosity=2)
