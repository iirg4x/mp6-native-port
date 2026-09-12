"""Alternating fixed-seed Release board runs; never touches user saves."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--name',required=True)
parser.add_argument('--ao',type=int,choices=(0,1,2),default=0)
parser.add_argument('--aa',type=int,choices=(0,1,2),default=2,
    help='Off (0), MSAA 4x (1), or FXAA (2); identical for both executables')
parser.add_argument('--captures',action='store_true')
parser.add_argument('--control',choices=('baseline','current'),
    help='Capture-only same-executable control for measuring render nondeterminism')
parser.add_argument('--gpu-profile',action='store_true',
    help='Use isolated GPU-profile executables; timings include query instrumentation')
parser.add_argument('--startup-ticks',type=int,default=20000,
    help='Tick budget for real-time startup before the fixed board measurement')
args=parser.parse_args()
if args.control and not args.captures: parser.error('--control requires --captures')
if args.startup_ticks<20000: parser.error('startup tick budget must be at least 20000')
if not re.fullmatch('[a-zA-Z0-9_-]+',args.name): parser.error('simple run name required')
order=('baseline','current') if args.captures else ('baseline','current','current','baseline','baseline','current')
if args.control: order=(args.control,args.control)
rows=[]
seed='build/board-qa-runs/engine-shipping-ao-off-baseline/gpu-cache'
for i,variant in enumerate(order):
    name=f'{args.name}-{i}-{variant}'
    command=[sys.executable,'tests/integration/run_board_qa.py','--exe',
        f'build/engine-benchmark/{variant+("-gpu" if args.gpu_profile else "")}/release/mp6native.exe','--name',name,
        '--seconds','90','--widescreen','--window-size','1920x1080','--ao',str(args.ao),
        '--aa',str(args.aa),'--ticks',str(args.startup_ticks),'--stop-round','0','--cache-seed',seed,
        '--input-script',f'period:30;timeout:{args.startup_ticks-1000};pressuntil:a/w01.live/120;wait:18000']
    if args.captures:
        command+=['--capture-trigger','w01.live','--capture-delay','1800','--capture-frames','12','--capture-stride','150']
    if args.gpu_profile:
        command+=['--gpu-timings']
    subprocess.run(command,cwd=ROOT,check=True)
    log=(ROOT/'build/board-qa-runs'/name/'game.log').read_text(errors='replace')
    match=re.search(r'\[ENGINE-BENCH\] ticks=4000 game=([\d.]+) submit=([\d.]+) seal=([\d.]+) post=([\d.]+) frame=([\d.]+)',log)
    if not match: raise RuntimeError(f'incomplete measurement: {name}')
    if '[FATAL]' in log or '[AURORA FATAL]' in log: raise RuntimeError(f'failure in {name}')
    row=dict(zip(('game','submit','seal','post','frame'),map(float,match.groups())))
    row.update(variant=variant,run=name)
    upload=re.search(r'\[ENGINE-UPLOAD\] vertex=(\d+) index=(\d+) uniform=(\d+) storage=(\d+) bytes/frame',log)
    if upload:
        row['upload_bytes']=dict(zip(('vertex','index','uniform','storage'),map(int,upload.groups())))
    from engine_benchmark_metrics import texture_upload_window
    texture_uploads=texture_upload_window(log)
    if texture_uploads is not None:
        row['texture_upload_samples']=texture_uploads
    draws=re.search(r'\[ENGINE-DRAWS\] submitted=([\d.]+) merged=([\d.]+)/frame',log)
    if draws:
        row['draws']=dict(zip(('submitted','merged'),map(float,draws.groups())))
    if args.gpu_profile:
        from engine_benchmark_metrics import gpu_window
        row['gpu_final_window']=gpu_window(log)
    rows.append(row)
    seed=f'build/board-qa-runs/{name}/gpu-cache'
    print(json.dumps(row),flush=True)
result={'ao':args.ao,'aa':args.aa,'captures':args.captures,'gpu_profile':args.gpu_profile,
        'control':args.control,
        'startup_tick_budget':args.startup_ticks,'runs':rows,'device':'Windows PC, not an Android measurement'}
if args.captures:
    from compare_ao_captures import frame
    import numpy as np
    captures=[]
    for row in rows:
        directory=ROOT/'build/board-qa-runs'/row['run']
        log=(directory/'game.log').read_text(errors='replace')
        event=re.search(r'\[EVENT\] w01\.live=\S+ num=\d+ tick=(\d+)',log)
        if not event: raise RuntimeError(f'missing board-ready event: {directory}')
        captures.append((int(event[1]),sorted((directory/'frames').glob('*.mfd'))))
    (start_a,files_a),(start_b,files_b)=captures
    if len(files_a)!=12 or len(files_b)!=12: raise RuntimeError('incomplete capture pair')
    comparison=[]
    for a,b in zip(files_a,files_b):
        ta,pa=frame(a)
        tb,pb=frame(b)
        if ta-start_a!=tb-start_b or pa.shape!=pb.shape:
            raise RuntimeError(f'unaligned board age or dimensions: {a}, {b}')
        delta=np.abs(pa.astype(np.int16)-pb.astype(np.int16))
        comparison.append(dict(board_age=ta-start_a,baseline_tick=ta,current_tick=tb,
            changed_pixels=int(np.any(delta!=0,axis=2).sum()),
            max_rgb_delta=int(delta.max()),mean_absolute_rgb=float(delta.mean())))
    result['scene_comparison']=comparison
if not args.captures:
    medians={v:{k:statistics.median(r[k] for r in rows if r['variant']==v)
                for k in ('game','submit','seal','post','frame')} for v in ('baseline','current')}
    result['median_ms']=medians
    result['frame_reduction_percent']=100*(1-medians['current']['frame']/medians['baseline']['frame'])
path=ROOT/'build/engine-benchmark'/f'{args.name}.json'
path.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2),flush=True)
