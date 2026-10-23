"""Alternating fixed-seed Release board runs; never touches user saves."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

from ao_preparation_benchmark import (
    DEFAULT_MATRIX_REPETITIONS,
    run_prepared_canonical_matrix,
)

ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--name',required=True)
parser.add_argument('--ao',type=int,choices=(0,1,2),default=0)
parser.add_argument('--implementation', choices=('legacy', 'xegtao'), default='legacy',
    help='AO implementation forwarded to each board run')
parser.add_argument('--xegtao-quality', choices=('low','medium','high','ultra'), default='high')
parser.add_argument('--xegtao-denoise', type=int, choices=(1,2,3), default=2)
parser.add_argument('--xegtao-resolution', choices=('full','half'), default='full')
parser.add_argument('--xegtao-depth-storage', choices=('r32float','r16float'), default='r32float')
parser.add_argument('--ao-matrix', action='store_true',
    help='Run cold/warm symmetric off, legacy, XeGTAO, XeGTAO, legacy, off on current')
parser.add_argument('--depth-storage-matrix', action='store_true',
    help='Run current XeGTAO symmetrically with R32, R16, R16, R32 working-depth storage')
parser.add_argument('--prepared-canonical-matrix', action='store_true',
    help='Run the same current-frame executable through off/view-Z/prepared-canonical paths')
parser.add_argument('--matrix-repetitions', type=int, default=DEFAULT_MATRIX_REPETITIONS,
    help='Repeated measured passes for --prepared-canonical-matrix (default: 2)')
parser.add_argument('--aa',type=int,choices=(0,1,2),default=2,
    help='Off (0), MSAA 4x (1), or FXAA (2); identical for both executables')
parser.add_argument('--captures',action='store_true')
parser.add_argument('--control',choices=('baseline','current'),
    help='Capture-only same-executable control for measuring render nondeterminism')
profile_group=parser.add_mutually_exclusive_group()
profile_group.add_argument('--gpu-profile',action='store_true',
    help='Use isolated GPU-profile executables; timings include query instrumentation')
profile_group.add_argument('--frame-profile',action='store_true',
    help='Use current-frame CPU instrumentation and include GPU timing windows')
parser.add_argument('--startup-ticks',type=int,default=20000,
    help='Tick budget for real-time startup before the fixed board measurement')
MATRIX_SCHEDULE=(
    ('off', 0, 'legacy'),
    ('legacy', 2, 'legacy'),
    ('xegtao', 2, 'xegtao'),
    ('xegtao', 2, 'xegtao'),
    ('legacy', 2, 'legacy'),
    ('off', 0, 'legacy'),
)
DEPTH_STORAGE_MATRIX=('r32float','r16float','r16float','r32float')
PREPARED_CANONICAL_REPETITIONS = DEFAULT_MATRIX_REPETITIONS
PREPARED_CANONICAL_MATRIX_SCHEDULE = ('off', 'viewz', 'canonical', 'canonical', 'viewz', 'off')
_DEPTH_STORAGE_LOG_RE = re.compile(
    r'\[MP6-AO\] XeGTAO depth storage: requested=(r32float|r16float) '
    r'actual=(r32float|r16float)'
)


def validate_xegtao_depth_storage(log, requested):
    """Require the native XeGTAO run to report the requested storage format."""

    records = _DEPTH_STORAGE_LOG_RE.findall(log)
    if not records:
        raise RuntimeError(
            'XeGTAO run did not report depth storage; expected '
            "'[MP6-AO] XeGTAO depth storage: requested=... actual=...'"
        )
    for logged_requested, actual in records:
        if logged_requested != requested or actual != requested:
            raise RuntimeError(
                f'XeGTAO depth storage mismatch: requested={requested} '
                f'log_requested={logged_requested} actual={actual}'
            )
    return dict(requested=requested, actual=records[-1][1], records=len(records))


def validation_error(args):
    """Return a parser error without touching the filesystem or starting a run."""
    depth_storage_matrix = getattr(args, 'depth_storage_matrix', False)
    prepared_canonical_matrix = getattr(args, 'prepared_canonical_matrix', False)
    matrix_repetitions = getattr(args, 'matrix_repetitions', 2)
    if prepared_canonical_matrix and (args.ao_matrix or depth_storage_matrix or args.captures or args.control):
        return '--prepared-canonical-matrix is incompatible with --ao-matrix, --depth-storage-matrix, --captures, and --control'
    if args.control and not args.captures:
        return '--control requires --captures'
    if prepared_canonical_matrix and not args.frame_profile:
        return '--prepared-canonical-matrix requires --frame-profile'
    if prepared_canonical_matrix and args.aa != 0:
        return '--prepared-canonical-matrix requires --aa 0 to isolate AO cost'
    if prepared_canonical_matrix and (args.ao != 0 or args.implementation != 'legacy'):
        return '--prepared-canonical-matrix selects its own AO and implementation conditions'
    if prepared_canonical_matrix and (
            args.xegtao_quality != 'high' or args.xegtao_denoise != 2 or
            args.xegtao_resolution != 'full' or args.xegtao_depth_storage != 'r32float'):
        return '--prepared-canonical-matrix requires Full/R32/High/2 XeGTAO settings'
    if prepared_canonical_matrix and (type(matrix_repetitions) is not int or matrix_repetitions < 1):
        return '--matrix-repetitions must be a positive integer'
    if not prepared_canonical_matrix and matrix_repetitions != 2:
        return '--matrix-repetitions is only valid with --prepared-canonical-matrix'
    if args.ao_matrix and (args.captures or args.control):
        return '--ao-matrix is incompatible with --captures and --control'
    if depth_storage_matrix and (args.ao_matrix or args.captures or args.control):
        return '--depth-storage-matrix is incompatible with --ao-matrix, --captures, and --control'
    if args.ao_matrix and args.aa != 0:
        return '--ao-matrix requires --aa 0 to isolate AO cost'
    if depth_storage_matrix and args.aa != 0:
        return '--depth-storage-matrix requires --aa 0 to isolate AO cost'
    if args.ao_matrix and (args.ao != 0 or args.implementation != 'legacy'):
        return '--ao-matrix selects its own AO and implementation conditions'
    if depth_storage_matrix and (args.ao != 0 or args.implementation != 'legacy'):
        return '--depth-storage-matrix selects its own AO and implementation conditions'
    if args.frame_profile and args.control == 'baseline':
        return '--frame-profile only provides a current-frame executable'
    return None


args=parser.parse_args()
if (error := validation_error(args)):
    parser.error(error)
if args.startup_ticks<20000: parser.error('startup tick budget must be at least 20000')
if not re.fullmatch('[a-zA-Z0-9_-]+',args.name): parser.error('simple run name required')

if args.prepared_canonical_matrix:
    run_prepared_canonical_matrix(args, ROOT)
    raise SystemExit(0)


def schedule_for(args):
    """Return (condition, variant, ao, implementation) without launching a run."""
    if getattr(args, 'prepared_canonical_matrix', False):
        return tuple((condition, 'current',
                      0 if condition == 'off' else 2,
                      'legacy' if condition == 'off' else 'xegtao')
                     for condition in PREPARED_CANONICAL_MATRIX_SCHEDULE * getattr(
                         args, 'matrix_repetitions', 2))
    if args.ao_matrix:
        return tuple((condition, 'current', ao, implementation)
                     for condition, ao, implementation in MATRIX_SCHEDULE)
    if getattr(args, 'depth_storage_matrix', False):
        return tuple((storage, 'current', 2, 'xegtao') for storage in DEPTH_STORAGE_MATRIX)
    if args.frame_profile:
        variants = ('current', 'current') if args.captures else ('current',) * 6
    else:
        variants = ('baseline', 'current') if args.captures else (
            'baseline', 'current', 'current', 'baseline', 'baseline', 'current')
        if args.control:
            variants = (args.control, args.control)
    return tuple((variant, variant, args.ao, args.implementation) for variant in variants)


schedule=schedule_for(args)
rows=[]
seed='build/board-qa-runs/engine-shipping-ao-off-baseline/gpu-cache'
condition_seeds={}
for i,(condition,variant,ao,implementation) in enumerate(schedule):
    name=f'{args.name}-{i}-{condition}'
    depth_storage = condition if args.depth_storage_matrix else args.xegtao_depth_storage
    suffix='-frame' if args.frame_profile else '-gpu' if args.gpu_profile else ''
    cache_args=[]
    if args.ao_matrix or args.depth_storage_matrix:
        if condition in condition_seeds:
            cache_args=['--cache-seed', condition_seeds[condition]]
    else:
        cache_args=['--cache-seed', seed]
    command=[sys.executable,'tests/integration/run_board_qa.py','--exe',
        f'build/engine-benchmark/{variant+suffix}/release/mp6native.exe','--name',name,
        '--seconds','90','--widescreen','--window-size','1920x1080','--ao',str(ao),
        '--implementation',implementation,
        '--xegtao-quality',args.xegtao_quality,'--xegtao-denoise',str(args.xegtao_denoise),
        '--xegtao-resolution',args.xegtao_resolution,
        '--xegtao-depth-storage',depth_storage,
        '--aa',str(args.aa),'--ticks',str(args.startup_ticks),'--stop-round','0',*cache_args,
        '--input-script',f'period:30;timeout:{args.startup_ticks-1000};pressuntil:a/w01.live/120;wait:18000']
    if not args.ao_matrix and args.captures:
        command+=['--capture-trigger','w01.live','--capture-delay','1800','--capture-frames','12','--capture-stride','150']
    if args.gpu_profile or args.frame_profile:
        command+=['--gpu-timings']
    subprocess.run(command,cwd=ROOT,check=True)
    log=(ROOT/'build/board-qa-runs'/name/'game.log').read_text(errors='replace')
    xegtao_depth_storage = None
    if ao and implementation == 'xegtao':
        xegtao_depth_storage = validate_xegtao_depth_storage(log, depth_storage)
    match=re.search(r'\[ENGINE-BENCH\] ticks=4000 game=([\d.]+) submit=([\d.]+) seal=([\d.]+) post=([\d.]+) frame=([\d.]+)',log)
    if not match: raise RuntimeError(f'incomplete measurement: {name}')
    if '[FATAL]' in log or '[AURORA FATAL]' in log: raise RuntimeError(f'failure in {name}')
    row=dict(zip(('game','submit','seal','post','frame'),map(float,match.groups())))
    row.update(condition=condition, variant=variant, ao=ao,
               implementation=implementation, run=name)
    if ao and implementation == 'xegtao':
        row['xegtao_policy'] = dict(quality=args.xegtao_quality,
                                   denoise_passes=args.xegtao_denoise,
                                   resolution=args.xegtao_resolution,
                                   depth_storage=depth_storage)
        row['requested_xegtao_depth_storage'] = depth_storage
        row['xegtao_depth_storage'] = xegtao_depth_storage
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
    if args.gpu_profile or args.frame_profile:
        from engine_benchmark_metrics import gpu_window
        row['gpu_final_window']=gpu_window(log)
        if (args.ao_matrix or args.depth_storage_matrix) and row['gpu_final_window']['dropped_frames'] != 0:
            raise RuntimeError(f'incomplete GPU window for {name}: dropped frames present')
    if args.frame_profile:
        from engine_benchmark_metrics import frame_profile
        row['frame_profile']=frame_profile(log)
    rows.append(row)
    if args.ao_matrix or args.depth_storage_matrix:
        condition_seeds[condition]=f'build/board-qa-runs/{name}/gpu-cache'
    else:
        seed=f'build/board-qa-runs/{name}/gpu-cache'
    # Keep the full stage inventory in the result artifact. Matrix console
    # output should identify progress without repeating hundreds of counters.
    progress = {k:row[k] for k in ('condition','run','frame')} if (args.ao_matrix or args.depth_storage_matrix) else row
    if (args.ao_matrix or args.depth_storage_matrix) and 'gpu_final_window' in row:
        progress['gpu_span_ms'] = row['gpu_final_window']['span_average_ms']
    print(json.dumps(progress),flush=True)
result={'ao':args.ao,'implementation':args.implementation,'ao_matrix':args.ao_matrix,
        'depth_storage_matrix':args.depth_storage_matrix,'aa':args.aa,
        'requested_xegtao_policy':dict(quality=args.xegtao_quality,
            denoise_passes=args.xegtao_denoise,resolution=args.xegtao_resolution,
            depth_storage=args.xegtao_depth_storage),
        'requested_xegtao_depth_storage':args.xegtao_depth_storage,
        'depth_storage_matrix_order':DEPTH_STORAGE_MATRIX if args.depth_storage_matrix else None,
        'captures':args.captures,'gpu_profile':args.gpu_profile,'frame_profile':args.frame_profile,
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
if args.depth_storage_matrix:
    result['depth_storage_measurement_window']={
        'warmup_board_age': '<1800 ticks',
        'measured_board_age': '1800..5799 ticks inclusive',
        'frames': 4000,
        'cache_policy': 'cold first run per storage; warm repeat only within that storage',
    }
    result['depth_storage_medians_ms']={
        storage:{k:statistics.median(r[k] for r in rows if r['condition']==storage)
                 for k in ('game','submit','seal','post','frame')}
        for storage in dict.fromkeys(DEPTH_STORAGE_MATRIX)
    }
elif args.ao_matrix:
    result['matrix_measurement_window']={
        'warmup_board_age': '<1800 ticks',
        'measured_board_age': '1800..5799 ticks inclusive',
        'frames': 4000,
        'cache_policy': 'cold first run per condition; warm repeat only within that condition',
    }
    result['condition_medians_ms']={
        condition:{k:statistics.median(r[k] for r in rows if r['condition']==condition)
                   for k in ('game','submit','seal','post','frame')}
        for condition,_,_ in (('off',0,'legacy'),('legacy',2,'legacy'),('xegtao',2,'xegtao'))
    }
elif not args.captures:
    variants=tuple(dict.fromkeys(r['variant'] for r in rows))
    medians={v:{k:statistics.median(r[k] for r in rows if r['variant']==v)
                for k in ('game','submit','seal','post','frame')} for v in variants}
    result['median_ms']=medians
    if set(('baseline','current')).issubset(medians):
        result['frame_reduction_percent']=100*(1-medians['current']['frame']/medians['baseline']['frame'])
path=ROOT/'build/engine-benchmark'/f'{args.name}.json'
path.write_text(json.dumps(result,indent=2)+'\n')
summary = dict(result_path=str(path), condition_medians_ms=result['condition_medians_ms'],
               device=result['device']) if args.ao_matrix else result
print(json.dumps(summary,indent=2),flush=True)
