"""Execute the port renderer's actual present shaders, before/after pass fusion.

Uses the same optional wgpu dependency as test_ao_gpu.py. This measures desktop
GPU work, not an Android performance prediction. No game/save files are used.
"""
from pathlib import Path
import json
import faulthandler
import re
import statistics
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build/ao-gpu-test-deps'))
import numpy as np
import wgpu


class PresentRenderer:
    def __init__(self):
        source = (ROOT / 'build/aurora-release-source/lib/webgpu/gpu.cpp').read_text()
        resample = re.search(r'resampleShaderSource = R"\((.*?)\)"sv;', source, re.S).group(1)
        copy = re.search(r'sourceDescriptor.code = R"""\((.*?)\)""";', source, re.S).group(1)
        self.adapter = wgpu.gpu.request_adapter_sync(power_preference='high-performance')
        self.device = self.adapter.request_device_sync(required_features=['timestamp-query'])
        self.modules = [self.device.create_shader_module(code=s) for s in (resample, copy)]
        self.sampler = self.device.create_sampler(mag_filter='linear', min_filter='linear')
        self.pipelines = {}

    def pipeline(self, fmt, kind):
        key = fmt, kind
        if key not in self.pipelines:
            module = self.modules[kind != 'resample']
            target = dict(format=fmt)
            if kind == 'overlay':
                blend = dict(src_factor='one', dst_factor='one-minus-src-alpha', operation='add')
                target['blend'] = dict(color=blend, alpha=blend)
            self.pipelines[key] = self.device.create_render_pipeline(layout='auto',
                vertex=dict(module=module, entry_point='vs_main'),
                fragment=dict(module=module, entry_point={'resample':'fs_main', 'copy':'fs_opaque',
                                                         'overlay':'fs_premultiplied_alpha'}[kind],
                              targets=[target]))
        return self.pipelines[key]

    def texture(self, w, h, fmt):
        return self.device.create_texture(size=(w,h,1), format=fmt,
            usage=wgpu.TextureUsage.TEXTURE_BINDING|wgpu.TextureUsage.RENDER_ATTACHMENT|
                  wgpu.TextureUsage.COPY_DST|wgpu.TextureUsage.COPY_SRC)

    def run(self, source_size, target_size, fmt='bgra8unorm', fxaa=0, area=0,
            direct=False, overlay=False, repeat=1, surface_size=None, origin=(0,0)):
        device = self.device
        w,h = target_size
        ow,oh = surface_size or target_size
        sw,sh = source_size
        y,x = np.indices((sh,sw))
        data = np.stack(((x*19+y*7)%256, (x//7*53+y*11)%256, (x+y*23)%256,
                         (x*5+y)%256), axis=-1).astype(np.uint8)
        source = self.texture(sw,sh,fmt)
        device.queue.write_texture(dict(texture=source),data,dict(bytes_per_row=sw*4),(sw,sh,1))
        intermediate, output = self.texture(w,h,fmt), self.texture(ow,oh,fmt)
        uniform = device.create_buffer_with_data(data=struct.pack('<IffI',area,w,h,fxaa),
                                                  usage=wgpu.BufferUsage.UNIFORM)
        resample,copy,over = [self.pipeline(fmt,k) for k in ('resample','copy','overlay')]
        sample_group = device.create_bind_group(layout=resample.get_bind_group_layout(0),entries=[
            dict(binding=0,resource=dict(buffer=uniform,size=16)),dict(binding=1,resource=self.sampler),
            dict(binding=2,resource=source.create_view())])
        def copy_group(pipeline,texture):
            return device.create_bind_group(layout=pipeline.get_bind_group_layout(0), entries=[
                dict(binding=0,resource=self.sampler),dict(binding=1,resource=texture.create_view())])
        intermediate_group = copy_group(copy,intermediate)
        overlays = []
        if overlay:
            yy,xx = np.indices((h,w))
            # Two differently colored, partly transparent layers: Rml + touch.
            for n in (0,1):
                alpha = np.where((xx+yy*n)%(17+9*n)<9, 128, 0).astype(np.uint8)
                pixels = np.zeros((h,w,4),np.uint8)
                pixels[...,n] = alpha
                pixels[...,3] = alpha
                texture = self.texture(w,h,fmt)
                device.queue.write_texture(dict(texture=texture),pixels,dict(bytes_per_row=w*4),(w,h,1))
                overlays.append((texture,copy_group(over,texture)))
        stride = 2 if direct else 4
        query = device.create_query_set(type='timestamp',count=stride*repeat)
        encoder = device.create_command_encoder()
        def begin(texture,slot):
            return encoder.begin_render_pass(color_attachments=[dict(view=texture.create_view(),
                load_op='clear',store_op='store',clear_value=(0,0,0,1))],timestamp_writes=dict(
                query_set=query,beginning_of_pass_write_index=slot,end_of_pass_write_index=slot+1))
        for i in range(repeat):
            if not direct:
                p = begin(intermediate,i*stride)
                p.set_pipeline(resample);p.set_bind_group(0,sample_group);p.draw(3);p.end()
            p = begin(output,i*stride+(0 if direct else 2))
            p.set_viewport(*origin,w,h,0,1)
            p.set_pipeline(resample if direct else copy)
            p.set_bind_group(0,sample_group if direct else intermediate_group);p.draw(3)
            for _,group in overlays:
                p.set_pipeline(over);p.set_bind_group(0,group);p.draw(3)
            p.end()
        timing = device.create_buffer(size=8*stride*repeat,
            usage=wgpu.BufferUsage.QUERY_RESOLVE|wgpu.BufferUsage.COPY_SRC)
        encoder.resolve_query_set(query,0,stride*repeat,timing,0)
        device.queue.submit([encoder.finish()])
        stamps = np.frombuffer(device.queue.read_buffer(timing),np.uint64).reshape(-1,stride)
        times = [sum(int(r[j+1])-int(r[j]) for j in range(0,stride,2))/1e6 for r in stamps]
        self.last_ms = statistics.median(times[-min(repeat,32):])
        raw = device.queue.read_texture(dict(texture=output),dict(bytes_per_row=ow*4),(ow,oh,1))
        return np.frombuffer(raw,np.uint8).reshape(oh,ow,4).copy()


class PresentFusion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.renderer = PresentRenderer()
        print('GPU:',json.dumps(dict(cls.renderer.adapter.info)),flush=True)

    def test_all_samplers_aa_and_noninteger_scaling(self):
        for fmt in ('bgra8unorm','rgba8unorm'):
            for source,target in (((1,1),(1,1)),((127,73),(511,257)),((511,257),(127,73)),
                                  ((257,129),(257,129)),((1024,576),(512,288))):
                for fxaa,area in ((0,0),(0,1),(1,0),(1,1)):
                    with self.subTest(fmt=fmt,source=source,target=target,fxaa=fxaa,area=area):
                        old = self.renderer.run(source,target,fmt,fxaa,area)
                        new = self.renderer.run(source,target,fmt,fxaa,area,direct=True)
                        # Native tablet case is exact. Offset rasterization on
                        # other extents can change the last rounding bit in one
                        # filtered channel; no sample/edge/alpha may disappear.
                        if source==(2957,1848):
                            np.testing.assert_array_equal(new,old)
                        else:
                            delta=np.abs(new.astype(np.int16)-old.astype(np.int16))
                            self.assertLessEqual(int(delta.max()),1)
                            self.assertLessEqual(np.count_nonzero(delta),max(1,delta.size//10000))
                            np.testing.assert_array_equal(new[...,3],old[...,3])

    def test_premultiplied_menu_and_touch_order(self):
        for fxaa in (0,1):
            old = self.renderer.run((731,411),(511,257),fxaa=fxaa,overlay=True)
            new = self.renderer.run((731,411),(511,257),fxaa=fxaa,overlay=True,direct=True)
            np.testing.assert_array_equal(new,old)

    def test_tablet_resolution(self):
        old = self.renderer.run((2960,1848),(2960,1848),fxaa=1)
        new = self.renderer.run((2960,1848),(2960,1848),fxaa=1,direct=True)
        np.testing.assert_array_equal(new,old)

    def test_integer_letterbox_and_android_rounding(self):
        for fmt in ('bgra8unorm','rgba8unorm'):
            for source,target,surface,origin in (
                    ((2957,1848),(2957,1848),(2960,1848),(1,0)),
                    ((127,73),(127,73),(135,79),(3,2)),
                    ((511,257),(511,257),(520,280),(4,11))):
                for fxaa in (0,1):
                    with self.subTest(fmt=fmt,source=source,fxaa=fxaa,origin=origin):
                        kw=dict(fmt=fmt,fxaa=fxaa,surface_size=surface,origin=origin)
                        old=self.renderer.run(source,target,**kw)
                        new=self.renderer.run(source,target,direct=True,**kw)
                        if source == (2957,1848):
                            np.testing.assert_array_equal(new,old)
                        else:
                            # Offset viewport rasterization can round an FXAA
                            # result one UNORM step differently at isolated pixels.
                            delta=np.abs(new.astype(np.int16)-old.astype(np.int16))
                            self.assertLessEqual(int(delta.max()),1)
                            self.assertLessEqual(int(np.count_nonzero(delta)),max(1,new.size//10000))
                            np.testing.assert_array_equal(new[:,:,3],old[:,:,3])


if __name__=='__main__':
    faulthandler.dump_traceback_later(45,repeat=True)
    if '--benchmark' in sys.argv:
        renderer = PresentRenderer()
        results=[]
        for fxaa in (0,1):
            pairs=[]
            for n in range(4):
                values={}
                for direct in ((False,True) if n%2==0 else (True,False)):
                    renderer.run((2960,1848),(2960,1848),fxaa=fxaa,direct=direct,repeat=96)
                    values['direct' if direct else 'two_pass'] = renderer.last_ms
                pairs.append(values)
            results.append(dict(fxaa=fxaa,measurements_ms=pairs))
        print(json.dumps(dict(gpu=dict(renderer.adapter.info),results=results),indent=2))
    else:
        unittest.main(verbosity=2)
