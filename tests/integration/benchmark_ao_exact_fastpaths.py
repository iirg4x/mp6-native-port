"""Alternating resident timing of exact blur optimization; not Android FPS."""
import json
import statistics
from test_ao_exact_fastpaths import reference_shader
from test_ao_gpu import Renderer, ROOT, plane, feet_scene, np, load_shader_source

renderers=[Renderer(mobile=True,timing=True,shader_source=reference_shader()),Renderer(mobile=True,timing=True,shader_source=load_shader_source())]
report={'adapter':dict(renderers[0].adapter.info),'scope':'Resident desktop GPU pass times only, not Android FPS; excludes depth snapshots and game rendering.','resolutions':[]}
for w,h in [(1280,720),(1920,1080),(2401,1081)]:
    ground,_=plane(w,h)
    raised,mask=plane(w,h,lift=18.)
    after=np.where(mask,raised,ground)
    feet,_=feet_scene(w,h)
    final=np.where(feet<ground-.001,feet,after)
    samples=[[],[]]
    expected=None
    for iteration in range(8):
        for index in ([0,1] if iteration%2==0 else [1,0]):
            _,_,output=renderers[index].render(final,decal_before=ground,decal_after=after,repeat=256)
            if iteration>1: samples[index].append(renderers[index].last_timings_ms)
            if index==0: expected=output
            elif expected is not None: np.testing.assert_array_equal(output,expected)
    medians=[[statistics.median(sample[i] for sample in variant) for i in range(5)] for variant in samples]
    row={'framebuffer':[w,h],'before_ms':medians[0],'after_ms':medians[1],
         'pass_order':['GTAO','blur X','blur Y','composite','decal depth'],
         'blur_reduction':1-sum(medians[1][1:3])/sum(medians[0][1:3]),'max_pixel_difference':0}
    report['resolutions'].append(row)
    print(json.dumps(row),flush=True)
path=ROOT/'build/ao-exact-fastpath-benchmark.json'
path.write_text(json.dumps(report,indent=2)+'\n')
print(path)
