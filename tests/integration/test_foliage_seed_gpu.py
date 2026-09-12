"""Exact foliage seed coverage from borrowed EFB depth vs an R32 snapshot."""
import unittest
from test_ao_gpu import wgpu, np, load_shader_source


class FoliageSeed(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = wgpu.gpu.request_adapter_sync(power_preference='high-performance').request_device_sync()

    def compare(self, samples, width, height, depth_format):
        d = self.device
        vertex = """
        @vertex fn vs_main(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
            let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
            return vec4f(p[i],0,1);
        }
        """
        seed_input = d.create_shader_module(code=vertex+"""
        @fragment fn fs(@builtin(position) p:vec4f, @builtin(sample_index) s:u32)
            ->@builtin(frag_depth) f32 {
            return fract((floor(p.x)*13.0+floor(p.y)*7.0)/256.0 + f32(s)*0.13);
        }""")
        source = d.create_texture(size=(width,height,1),format=depth_format,sample_count=samples,
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.TEXTURE_BINDING)
        source_view = source.create_view()
        input_pipeline = d.create_render_pipeline(layout='auto',
            vertex=dict(module=seed_input,entry_point='vs_main'),
            fragment=dict(module=seed_input,entry_point='fs',targets=[]),
            depth_stencil=dict(format=depth_format,depth_write_enabled=True,depth_compare='always'),
            multisample=dict(count=samples))
        encoder=d.create_command_encoder()
        p=encoder.begin_render_pass(color_attachments=[],depth_stencil_attachment=dict(view=source_view,
            depth_load_op='clear',depth_store_op='store',depth_clear_value=0))
        p.set_pipeline(input_pipeline);p.draw(3);p.end()

        # The reference copy takes sample zero, exactly like Aurora.
        dtype='texture_depth_multisampled_2d' if samples>1 else 'texture_depth_2d'
        copy_shader=d.create_shader_module(code=vertex+
            '@group(0) @binding(0) var depth: '+dtype+""";
            @fragment fn fs(@builtin(position) p:vec4f)->@location(0) f32 {
                return textureLoad(depth,vec2i(p.xy),0);
            }""")
        snapshot=d.create_texture(size=(width,height,1),format='r32float',
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.TEXTURE_BINDING)
        copy_pipeline=d.create_render_pipeline(layout='auto',
            vertex=dict(module=copy_shader,entry_point='vs_main'),
            fragment=dict(module=copy_shader,entry_point='fs',targets=[dict(format='r32float')]))
        group=d.create_bind_group(layout=copy_pipeline.get_bind_group_layout(0),
            entries=[dict(binding=0,resource=source_view)])
        p=encoder.begin_render_pass(color_attachments=[dict(view=snapshot.create_view(),
            load_op='clear',store_op='store',clear_value=(0,0,0,0))])
        p.set_pipeline(copy_pipeline);p.set_bind_group(0,group);p.draw(3);p.end()

        outputs=[]
        for direct in (False,True):
            shader=d.create_shader_module(code=load_shader_source(attachment_depth=direct,
                multisampled_depth=samples>1,decals=False))
            seed_pipeline=d.create_render_pipeline(layout='auto',
                vertex=dict(module=shader,entry_point='vs_main'),
                fragment=dict(module=shader,entry_point='fs_foliage_seed',targets=[dict(format='rgba8unorm')]),
                depth_stencil=dict(format=depth_format,depth_write_enabled=True,depth_compare='always'))
            color=d.create_texture(size=(width,height,1),format='rgba8unorm',
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT)
            depth=d.create_texture(size=(width,height,1),format=depth_format,
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.TEXTURE_BINDING)
            group=d.create_bind_group(layout=seed_pipeline.get_bind_group_layout(0),
                entries=[dict(binding=1,resource=source_view if direct else snapshot.create_view())])
            p=encoder.begin_render_pass(color_attachments=[dict(view=color.create_view(),
                load_op='clear',store_op='store',clear_value=(1,1,1,1))],
                depth_stencil_attachment=dict(view=depth.create_view(),depth_load_op='clear',
                    depth_store_op='store',depth_clear_value=1))
            p.set_pipeline(seed_pipeline);p.set_bind_group(0,group);p.draw(3);p.end()
            outputs.append(depth)

        # Later EFB writes must not change either private seed target.
        p=encoder.begin_render_pass(color_attachments=[],depth_stencil_attachment=dict(view=source_view,
            depth_load_op='clear',depth_store_op='store',depth_clear_value=.875))
        p.end()
        probe=d.create_shader_module(code=vertex+"""
        @group(0) @binding(0) var depth:texture_depth_2d;
        @fragment fn fs(@builtin(position) p:vec4f)->@location(0) f32 {
            return textureLoad(depth,vec2i(p.xy),0);
        }""")
        pipeline=d.create_render_pipeline(layout='auto',vertex=dict(module=probe,entry_point='vs_main'),
            fragment=dict(module=probe,entry_point='fs',targets=[dict(format='r32float')]))
        readbacks=[]
        for depth in outputs:
            target=d.create_texture(size=(width,height,1),format='r32float',
                usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.COPY_SRC)
            group=d.create_bind_group(layout=pipeline.get_bind_group_layout(0),
                entries=[dict(binding=0,resource=depth.create_view())])
            p=encoder.begin_render_pass(color_attachments=[dict(view=target.create_view(),
                load_op='clear',store_op='store',clear_value=(0,0,0,0))])
            p.set_pipeline(pipeline);p.set_bind_group(0,group);p.draw(3);p.end()
            readbacks.append(target)
        d.queue.submit([encoder.finish()])
        values=[np.frombuffer(d.queue.read_texture(dict(texture=t),
                    dict(bytes_per_row=width*4,rows_per_image=height),(width,height,1)),dtype=np.float32)
                for t in readbacks]
        np.testing.assert_array_equal(*values)
        self.assertGreater(np.ptp(values[0]),.9)

    def test_depth_formats_sizes_and_sample_zero(self):
        for fmt in ('depth32float','depth24plus'):
            for samples in (1,4):
                for width,height in ((129,65),(511,257)):
                    with self.subTest(fmt=fmt,samples=samples,size=(width,height)):
                        self.compare(samples,width,height,fmt)


if __name__ == '__main__':
    unittest.main(verbosity=2)
