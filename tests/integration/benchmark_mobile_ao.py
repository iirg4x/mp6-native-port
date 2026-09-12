"""Resident GPU pass timing, not Android-device or whole-game FPS evidence."""
import json
from pathlib import Path
import statistics
from test_ao_gpu import Renderer, feet_scene, plane, np, ROOT

baseline=ROOT/'build/checkpoints/before-background-terrain-mobile-ao/source/src/gx/ambient_occlusion_shader.hpp'
renderers=[('previous_desktop_budget',Renderer(timing=True,shader_path=baseline)),
           ('previous_shader_mobile_budget',Renderer(timing=True,mobile=True,shader_path=baseline)),
           ('optimized_mobile',Renderer(timing=True,mobile=True))]
report={'adapter':dict(renderers[0][1].adapter.info),
        'scope':'Five resident AO passes only; excludes game, depth copies, foliage replay and present. Desktop GPU, not Android FPS.',
        'resolutions':[]}
for width,height in ((1280,720),(1920,1080)):
    ground,_=plane(width,height)
    raised,mask=plane(width,height,lift=3.)
    after=np.where(mask,raised,ground)
    feet,_=feet_scene(width,height)
    final=np.where(feet<ground-.001,feet,after)
    row={'framebuffer':[width,height],'variants':{}}
    reference=None
    for name,renderer in renderers:
        samples=[]
        for iteration in range(5):
            _,_,output=renderer.render(final,decal_before=ground,decal_after=after,repeat=256)
            if iteration: samples.append(renderer.last_timings_ms)
        medians=[statistics.median(sample[i] for sample in samples) for i in range(5)]
        row['variants'][name]={'passes_ms':dict(zip(('gtao','blur_x','blur_y','composite','decal_depth'),medians)),
                               'total_ms':sum(medians)}
        if name=='previous_shader_mobile_budget': reference=output
        if name=='optimized_mobile':
            np.testing.assert_array_equal(output,reference)
            row['optimized_shader_max_pixel_difference']=float(np.abs(output-reference).max())
    row['pass_time_reduction']=1-row['variants']['optimized_mobile']['total_ms']/row['variants']['previous_desktop_budget']['total_ms']
    report['resolutions'].append(row)
    print(json.dumps(row),flush=True)
path=ROOT/'build/ao-mobile-benchmark.json'
path.write_text(json.dumps(report,indent=2)+'\n')
print(path)
