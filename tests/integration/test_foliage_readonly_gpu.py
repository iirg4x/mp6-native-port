"""Exact GPU coverage and EFB preservation: private seed vs read-only depth."""
import unittest
from test_ao_gpu import wgpu, np, load_shader_source


class ReadonlyFoliage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device=wgpu.gpu.request_adapter_sync(power_preference='high-performance').request_device_sync()

    def compare(self, width, height, fmt, reverse):
        d=self.device
        vertex='''
        @vertex fn vs_main(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
            let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
            return vec4f(p[i],0.5,1);
        }'''
        populate=d.create_shader_module(code=vertex+'''
        @fragment fn fs(@builtin(position) p:vec4f)->@builtin(frag_depth) f32 {
            return f32((u32(p.x)/7u+u32(p.y)/11u)%5u)*0.25;
        }''')
        def attachment(texture, readonly=False, clear=False, value=0):
            result=dict(view=texture.create_view(),depth_read_only=readonly,stencil_read_only=readonly)
            if not readonly:
                result.update(depth_load_op='clear' if clear else 'load',depth_store_op='store',depth_clear_value=value)
                if 'stencil' in fmt:
                    result.update(stencil_load_op='clear' if clear else 'load',stencil_store_op='store',stencil_clear_value=73)
            return result
        def depth_texture():
            return d.create_texture(size=(width,height,1),format=fmt,
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.TEXTURE_BINDING)
        def color_texture(format='rgba8unorm'):
            return d.create_texture(size=(width,height,1),format=format,
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.COPY_SRC)
        def color_attachment(texture):
            return dict(view=texture.create_view(),load_op='clear',store_op='store',clear_value=(0,0,0,0))
        source=depth_texture()
        source_depth=source.create_view(aspect='depth-only',format='depth24plus' if 'stencil' in fmt else fmt)
        fill=d.create_render_pipeline(layout='auto',vertex=dict(module=populate,entry_point='vs_main'),
            fragment=dict(module=populate,entry_point='fs',targets=[]),
            depth_stencil=dict(format=fmt,depth_write_enabled=True,depth_compare='always'))
        encoder=d.create_command_encoder()
        p=encoder.begin_render_pass(color_attachments=[],depth_stencil_attachment=attachment(source,clear=True))
        p.set_pipeline(fill);p.draw(3);p.end()
        probe=d.create_shader_module(code=vertex+'''
        @group(0) @binding(0) var depth:texture_depth_2d;
        @fragment fn fs(@builtin(position) p:vec4f)->@location(0) f32 {
            return textureLoad(depth,vec2i(p.xy),0);
        }''')
        probe_pipeline=d.create_render_pipeline(layout='auto',vertex=dict(module=probe,entry_point='vs_main'),
            fragment=dict(module=probe,entry_point='fs',targets=[dict(format='r32float')]))
        probe_group=d.create_bind_group(layout=probe_pipeline.get_bind_group_layout(0),
            entries=[dict(binding=0,resource=source_depth)])
        def snapshot():
            target=color_texture('r32float')
            p=encoder.begin_render_pass(color_attachments=[color_attachment(target)])
            p.set_pipeline(probe_pipeline);p.set_bind_group(0,probe_group);p.draw(3);p.end()
            return target
        before=snapshot()
        leaf=d.create_shader_module(code=vertex+'''
        @fragment fn leaf(@builtin(position) p:vec4f)->@location(0) vec4f {
            let alpha=f32((u32(p.x)+u32(p.y))%5u)*0.25;
            if(alpha<1.0/255.0) {discard;}
            return vec4f(0.125,0.75,0.25,alpha);
        }
        @fragment fn water(@builtin(position) p:vec4f)->@location(0) vec4f {
            if((u32(p.x)/9u)%2u==0u) {discard;}
            return vec4f(0,0,0,1);
        }''')
        state=dict(format=fmt,depth_write_enabled=False,
                   depth_compare='greater-equal' if reverse else 'less-equal')
        leaf_pipeline=d.create_render_pipeline(layout='auto',vertex=dict(module=leaf,entry_point='vs_main'),
            fragment=dict(module=leaf,entry_point='leaf',targets=[dict(format='rgba8unorm',blend=dict(
                color=dict(src_factor='src-alpha',dst_factor='one-minus-src-alpha',operation='add'),
                alpha=dict(src_factor='zero',dst_factor='one',operation='add')))]),depth_stencil=state)
        water_pipeline=d.create_render_pipeline(layout='auto',vertex=dict(module=leaf,entry_point='vs_main'),
            fragment=dict(module=leaf,entry_point='water',targets=[dict(format='rgba8unorm',write_mask=wgpu.ColorWrite.ALPHA)]),
            depth_stencil=state)
        outputs=[]
        for readonly in (False,True):
            color=color_texture()
            target=source if readonly else depth_texture()
            if not readonly:
                shader=d.create_shader_module(code=load_shader_source(attachment_depth=True,decals=False))
                seed=d.create_render_pipeline(layout='auto',vertex=dict(module=shader,entry_point='vs_main'),
                    fragment=dict(module=shader,entry_point='fs_foliage_seed',targets=[dict(format='rgba8unorm')]),
                    depth_stencil=dict(format=fmt,depth_write_enabled=True,depth_compare='always'))
                group=d.create_bind_group(layout=seed.get_bind_group_layout(0),entries=[dict(binding=1,resource=source_depth)])
            p=encoder.begin_render_pass(color_attachments=[color_attachment(color)],
                depth_stencil_attachment=attachment(target,readonly,clear=not readonly))
            if not readonly:
                p.set_pipeline(seed);p.set_bind_group(0,group);p.draw(3)
            # Multiple overlapping layers and water: original draw order,
            # alpha discard, blending, channel masks and depth comparison.
            p.set_pipeline(leaf_pipeline);p.draw(3)
            p.set_pipeline(water_pipeline);p.draw(3)
            p.set_pipeline(leaf_pipeline);p.draw(3);p.end()
            outputs.append(color)
        after=snapshot()
        p=encoder.begin_render_pass(color_attachments=[],depth_stencil_attachment=attachment(source,clear=True,value=.875))
        p.end()
        resumed=snapshot()
        d.queue.submit([encoder.finish()])
        def read(texture,dtype):
            return np.frombuffer(d.queue.read_texture(dict(texture=texture),
                dict(bytes_per_row=width*4,rows_per_image=height),(width,height,1)),dtype=dtype)
        np.testing.assert_array_equal(read(outputs[0],np.uint8),read(outputs[1],np.uint8))
        pixels=read(outputs[1],np.uint8).reshape(height,width,4)
        self.assertGreater(int(pixels[...,:3].max()),100)
        self.assertEqual(int(pixels[...,3].max()),255)
        self.assertEqual(int(pixels[...,3].min()),0)
        np.testing.assert_array_equal(read(before,np.float32),read(after,np.float32))
        self.assertGreater(float(np.ptp(read(before,np.float32))),.9)
        np.testing.assert_allclose(read(resumed,np.float32),.875,atol=1/16777215)

    def test_exact_coverage_and_preserved_depth(self):
        for fmt in ('depth32float','depth24plus','depth24plus-stencil8'):
            for reverse in (False,True):
                for size in ((129,65),(511,257)):
                    with self.subTest(format=fmt,reverse=reverse,size=size):
                        self.compare(*size,fmt,reverse)


if __name__=='__main__':
    unittest.main(verbosity=2)
