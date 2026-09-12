"""Compare current production WGSL to an explicitly selected older header.

Resident, alternating desktop GPU timings; never an Android FPS prediction.
Pass an archived 0.4.10 header with --baseline; output stays in ignored build/.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
from test_ao_gpu import Renderer, ROOT, plane, feet_scene, np, load_shader_source

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--prepared-depth', action='store_true', help='Include production fused R32/decal preparation')
parser.add_argument('--compatibility', action='store_true', help='Use conservative Mali kernel on both sides')
parser.add_argument('--desktop-budget', action='store_true', help='1920-wide AO budget instead of Android 960')
parser.add_argument('--repeat', type=int, default=2048)
args = parser.parse_args()
output = args.output.resolve()
if not output.is_relative_to((ROOT/'build').resolve()) or output.exists():
    parser.error('--output must be a new file inside build/')
if args.repeat < 32: parser.error('--repeat must be at least 32 for resident timing')
baseline = load_shader_source(args.baseline,compatibility=args.compatibility)
current = load_shader_source(compatibility=args.compatibility,prepared_depth=args.prepared_depth)
renderers = [Renderer(mobile=not args.desktop_budget,timing=True,shader_source=baseline),
             Renderer(mobile=not args.desktop_budget,timing=True,shader_source=current,prepared_depth=args.prepared_depth)]
report = dict(
    adapter=dict(renderers[0].adapter.info),
    scope='Synthetic resident desktop GPU passes. Excludes game rendering, depth snapshots, foliage replay, UI and presentation. Not Android FPS.',
    baseline_header=str(args.baseline.resolve()),
    baseline_wgsl_sha256=hashlib.sha256(baseline.encode()).hexdigest(),
    current_wgsl_sha256=hashlib.sha256(current.encode()).hexdigest(),
    compatibility=args.compatibility, prepared_depth=args.prepared_depth, mobile_budget=not args.desktop_budget,
    resident_repetitions=args.repeat, timed_repetitions=32,
    pass_order=['GTAO', 'blur X', 'blur Y', 'composite', 'decal depth'],
    scenes=[])
for w,h in ((1920,1080), (2960,1848)):
    for kind in ('feet', 'dense_edges'):
        ground,_ = plane(w,h)
        depth,_ = feet_scene(w,h)
        y,x = np.indices((h,w))
        if kind == 'dense_edges':
            depth = ground - 35*((x//32+y//32)%2)
        render_args={}
        if args.prepared_depth:
            raised,mask=plane(w,h,lift=18.)
            mask &= (x%7!=0)&(y%11!=0)
            decal=np.where(mask,raised,ground)
            depth=np.where(depth<ground-.001,depth,decal)
            render_args=dict(decal_before=ground,decal_after=decal)
        samples = [[],[]]
        expected = None
        for iteration in range(8):
            for i in ([0,1] if iteration%2 == 0 else [1,0]):
                result = renderers[i].render(depth,repeat=args.repeat,timing_samples=32,**render_args)
                if i == 0: expected = result
                elif expected is not None:
                    for a,b in zip(result,expected): np.testing.assert_array_equal(a,b)
                if iteration > 1: samples[i].append(renderers[i].last_timings_ms)
        medians = [[statistics.median(row[j] for row in variant) for j in range(5 if args.prepared_depth else 4)] for variant in samples]
        row = dict(size=[w,h],scene=kind,before_ms=medians[0],after_ms=medians[1],
                   gtao_speedup=medians[0][0]/medians[1][0],
                   measured_pass_sum_speedup=sum(medians[0])/sum(medians[1]),
                   max_pixel_difference=0)
        report['scenes'].append(row)
        print(json.dumps(row),flush=True)
output.write_text(json.dumps(report,indent=2)+'\n')
print(output)
