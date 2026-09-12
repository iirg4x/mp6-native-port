"""Exact-output, resident GPU water/land comparisons; not Android FPS."""
import argparse
import hashlib
import json
import statistics
from pathlib import Path
from test_ao_gpu import Renderer, ROOT, np, plane, feet_scene, load_shader_source


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,help='Optional archived experiment; defaults to production')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repeat',type=int,default=1024)
    parser.add_argument('--compatibility',action='store_true')
    args=parser.parse_args()
    target=args.output.resolve()
    if target.exists() or not target.is_relative_to((ROOT/'build').resolve()):
        parser.error('output must be a new path inside build/')
    if args.repeat<32: parser.error('at least 32 resident repetitions required')
    sources=[load_shader_source(args.baseline,compatibility=args.compatibility,prepared_depth=True),
             load_shader_source(args.candidate,compatibility=args.compatibility,prepared_depth=True)]
    renderers=[Renderer(mobile=True,timing=True,prepared_depth=True,shader_source=s) for s in sources]
    report=dict(scope='Resident desktop GPU diagnostic, Android 960px AO budget; not mobile hardware or whole-game FPS.',
                adapter=dict(renderers[0].adapter.info),compatibility=args.compatibility,
                shader_hashes=[hashlib.sha256(s.encode()).hexdigest() for s in sources],
                resident_repeats=args.repeat,timed_repeats=32,warmup_rounds=2,rounds=6,
                pass_order=['GTAO','blur X','blur Y','composite','depth preparation'],scenes=[])
    for w,h in ((1920,1080),(2960,1848)):
        ground,_=plane(w,h)
        y,x=np.indices((h,w))
        for kind in ('feet_land','dense_land','dense_water'):
            z,_=feet_scene(w,h)
            if kind!='feet_land': z=ground-35*((x//32+y//32)%2)
            coverage=np.zeros((h,w,4),np.float32)
            if kind=='dense_water': coverage[...,3]=(x>w//4)
            samples=[[],[]]
            expected=None
            for iteration in range(8):
                for i in ([0,1] if iteration%2==0 else [1,0]):
                    result=renderers[i].render(z,decal_before=z,decal_after=z,foliage=coverage,
                                               repeat=args.repeat,timing_samples=32)
                    if expected is None: expected=result
                    for a,b in zip(result,expected): np.testing.assert_array_equal(a,b)
                    if iteration>=2: samples[i].append(renderers[i].last_timings_ms)
            medians=[[statistics.median(row[j] for row in variant) for j in range(5)] for variant in samples]
            row=dict(size=[w,h],scene=kind,ms=medians,samples_ms=samples,max_pixel_difference=0,
                     composite_reduction_percent=100*(1-medians[1][3]/medians[0][3]),
                     pass_sum_reduction_percent=100*(1-sum(medians[1])/sum(medians[0])))
            report['scenes'].append(row)
            print(json.dumps({k:v for k,v in row.items() if k!='samples_ms'}),flush=True)
    target.write_text(json.dumps(report,indent=2)+'\n')
    print(target,flush=True)


if __name__=='__main__': main()
