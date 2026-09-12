"""GPU equivalence of replace blending versus disabled blending."""
import unittest
from test_ao_gpu import wgpu,np


class OpaqueBlend(unittest.TestCase):
    def test_unorm_srgb_msaa_and_masked_destination_alpha(self):
        device=wgpu.gpu.request_adapter_sync(power_preference='high-performance').request_device_sync()
        shader=device.create_shader_module(code="""
            @vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
                let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
                return vec4f(p[i],0,1);
            }
            @fragment fn fs(@builtin(position) p:vec4f)->@location(0) vec4f {
                let n=u32(p.y)*64u+u32(p.x);
                return vec4f(f32(n%1024u)/1023.,f32((n*7u)%1024u)/1023.,
                             f32((n*13u)%1024u)/1023.,f32(n%256u)/255.);
            }""")
        w,h=64,64
        identity=dict(src_factor='one',dst_factor='zero',operation='add')
        for fmt in ('rgba8unorm','bgra8unorm','rgba8unorm-srgb','bgra8unorm-srgb'):
            for samples in (1,4):
                for mask in (0,7,8,15):
                    results=[]
                    for blend in (dict(color=identity,alpha=identity),None):
                        tex=device.create_texture(size=(w,h,1),format=fmt,
                            usage=wgpu.TextureUsage.RENDER_ATTACHMENT|wgpu.TextureUsage.COPY_SRC)
                        target=tex if samples==1 else device.create_texture(size=(w,h,1),format=fmt,
                            sample_count=samples,usage=wgpu.TextureUsage.RENDER_ATTACHMENT)
                        # Masked alpha may have destination-alpha override.
                        selected=blend if blend is None or mask&8 else dict(color=identity,
                            alpha=dict(src_factor='constant',dst_factor='zero',operation='add'))
                        pipe=device.create_render_pipeline(layout='auto',vertex=dict(module=shader,entry_point='vs'),
                            fragment=dict(module=shader,entry_point='fs',targets=[dict(format=fmt,write_mask=mask,
                                **(dict(blend=selected) if selected else {}))]),multisample=dict(count=samples))
                        encoder=device.create_command_encoder()
                        p=encoder.begin_render_pass(color_attachments=[dict(view=target.create_view(),
                            resolve_target=tex.create_view() if samples>1 else None,
                            load_op='clear',store_op='store',clear_value=(.125,.375,.625,.875))])
                        p.set_pipeline(pipe);p.set_blend_constant((.25,.25,.25,.25));p.draw(3);p.end()
                        device.queue.submit([encoder.finish()])
                        results.append(bytes(device.queue.read_texture(dict(texture=tex),dict(bytes_per_row=w*4),(w,h,1))))
                    self.assertEqual(results[0],results[1],(fmt,samples,mask))


if __name__=='__main__':unittest.main(verbosity=2)
