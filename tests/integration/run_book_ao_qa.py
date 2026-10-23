"""Hidden, muted book-scene AO captures using an isolated save/config directory."""
from pathlib import Path
import argparse
import csv
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags


def xegtao_environment(implementation, quality, denoise, resolution='full'):
    env = {'MP6_AO_XEGTAO': str(int(implementation == 'xegtao'))}
    if implementation == 'xegtao':
        env.update(MP6_AO_XEGTAO_QUALITY=quality, MP6_AO_XEGTAO_DENOISE=str(denoise),
                   MP6_AO_XEGTAO_RESOLUTION=resolution)
    return env


def xegtao_execution_evidence(log, ao, implementation, quality, denoise, resolution='full'):
    counts = re.findall(r'^\[MP6-AO\] encoded=\d+ composited=\d+ foliage=\d+ xegtao=(\d+)\s*$',
                        log, re.MULTILINE)
    evidence = dict(effective_quality=None, effective_denoise=None, effective_resolution=None, mode=None,
                    dispatches=int(counts[-1]) if counts else None)
    if not ao or implementation != 'xegtao':
        return evidence
    rows = re.findall(r'^\[MP6-AO\] XeGTAO .*$', log, re.MULTILINE)
    assert rows, 'XeGTAO requested but no effective native policy was reported'
    policies = [re.fullmatch(
        r'\[MP6-AO\] XeGTAO (reference|experimental): (full-resolution direct|half-resolution bilateral) uint output, '
        r'(Low|Medium|High), ([12]) spatial denoise (pass|passes); opt-in only', row.rstrip('\r'))
        for row in rows]
    assert all(policies), 'Malformed effective native XeGTAO policy was reported'
    policies = [policy.groups() for policy in policies]
    expected = ('reference' if resolution == 'full' and quality == 'high' and denoise == 2 else 'experimental',
                'full-resolution direct' if resolution == 'full' else 'half-resolution bilateral',
                quality.capitalize(), str(denoise), 'pass' if denoise == 1 else 'passes')
    assert all(policy == expected for policy in policies), (
        f'XeGTAO effective native policy mismatch: requested {expected}, reported {policies}')
    assert evidence['dispatches'] and evidence['dispatches'] > 0, (
        'XeGTAO requested but no upstream dispatch executed')
    evidence.update(effective_quality=policies[-1][2].lower(),
                    effective_denoise=int(policies[-1][3]), mode=policies[-1][0],
                    effective_resolution=policies[-1][1].split('-')[0])
    return evidence


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name')
    parser.add_argument('--ao', type=int, choices=[0, 1, 2], default=2)
    parser.add_argument('--exe', type=Path, default=ROOT/'build/release/mp6native.exe')
    parser.add_argument('--size', default='1280x960')
    parser.add_argument('--aa', type=int, choices=range(5), default=0)
    parser.add_argument('--unlocked', action='store_true')
    parser.add_argument('--implementation', choices=['legacy', 'xegtao'], default='legacy',
                        help='Opt-in upstream reference path; not an Android acceptance run')
    parser.add_argument('--xegtao-quality', choices=['low', 'medium', 'high'], default='high')
    parser.add_argument('--xegtao-denoise', type=int, choices=[1, 2], default=2)
    parser.add_argument('--xegtao-resolution', choices=['full', 'half'], default='full',
                        help='Opt-in half-resolution bilateral reconstruction; reference remains full')
    parser.add_argument('--gpu-validation', action='store_true')
    parser.add_argument('--gpu-timings', action='store_true')
    parser.add_argument('--capture-trigger', default='',
                        help='Observed game event key=value substring to arm capture (not arbitrary log text)')
    parser.add_argument('--capture-delay', type=int, default=700)
    parser.add_argument('--capture-stride', type=int, default=200)
    parser.add_argument('--capture-count', type=int, default=6)
    parser.add_argument('--capture-rect', default='',
                        help='GPU-side x,y,w,h crop in source EFB pixels; use for FI captures')
    parser.add_argument('--require-replay', action='store_true',
                        help='Fail unless index.csv contains an actual interpolated present')
    parser.add_argument('--foreground', choices=['off','only'], help='isolated diagnostic build only')
    args = parser.parse_args(argv)
    if args.implementation == 'legacy' and (args.xegtao_quality != 'high' or args.xegtao_denoise != 2
                                             or args.xegtao_resolution != 'full'):
        parser.error('nondefault XeGTAO quality, denoise or resolution requires --implementation xegtao')
    if not 0 <= args.capture_delay <= 1000000 or not 1 <= args.capture_stride <= 1000:
        parser.error('capture-delay must be 0..1000000 and capture-stride 1..1000')
    if not 1 <= args.capture_count <= 1000:
        parser.error('capture-count must be 1..1000')
    if args.capture_rect:
        try:
            x, y, width, height = map(int, args.capture_rect.split(','))
            if min(x, y) < 0 or min(width, height) <= 0:
                raise ValueError()
        except ValueError:
            parser.error('capture-rect must be nonnegative x,y and positive w,h')
    if args.require_replay and not args.unlocked:
        parser.error('require-replay needs --unlocked')
    assert args.name.replace('-', '').replace('_', '').isalnum()
    return args


def main(argv=None):
    args = parse_args(argv)
    output = ROOT/'build/book-ao-qa'/args.name
    output.mkdir(parents=True, exist_ok=False)
    env = {k:v for k,v in os.environ.items() if not k.startswith('MP6_')}
    env.update(MP6_LAUNCHER='0', MP6_DISC_ROOT=str(ROOT/'build/disc-cache/orig/GP6E01'),
               MP6_GPU_CACHE_PATH=str(output/'gpu-cache'), MP6_WINDOW_SIZE=args.size,
               MP6_AUTO_START_TICKS='60,150',
               MP6_TICK_HZ='60', MP6_VSYNC='0', MP6_UNLOCKED_FPS=str(int(args.unlocked)),
               MP6_ENH_AMBIENT_OCCLUSION=str(args.ao), MP6_ENH_AA=str(args.aa),
               MP6_AO_DIAG='1', MP6_ENH_WIDESCREEN='1', MP6_WIDESCREEN='1', MP6_FREE_ASPECT='1',
               MP6_FRAME_DUMP=str(output/'frames'), MP6_FRAME_DUMP_COUNT=str(args.capture_count),
               MP6_FRAME_DUMP_DELAY=str(args.capture_delay), MP6_FRAME_DUMP_STRIDE=str(args.capture_stride))
    if args.capture_trigger:
        env['MP6_FRAME_DUMP_TRIGGER'] = args.capture_trigger
    if args.capture_rect:
        env['MP6_FRAME_DUMP_RECT'] = args.capture_rect
    env.update(xegtao_environment(args.implementation, args.xegtao_quality, args.xegtao_denoise,
                                 args.xegtao_resolution))
    env['MP6_GPU_VALIDATION'] = str(int(args.gpu_validation))
    env['MP6_GPU_TIMINGS'] = str(int(args.gpu_timings))
    isolate_background(env, args.exe)
    if args.foreground:
        env['MP6_AO_QA_FOREGROUND'] = args.foreground
    with (output/'run.log').open('w') as log:
        result = subprocess.run([str(args.exe), '1900'],
                                cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=180, creationflags=background_process_flags())
    log = (output/'run.log').read_text(errors='replace')
    assert result.returncode == 0, log[-5000:]
    assert 'Validation Error' not in log and '[CRASH]' not in log, log[-5000:]
    xegtao_evidence = xegtao_execution_evidence(log, args.ao, args.implementation,
                                              args.xegtao_quality, args.xegtao_denoise,
                                              args.xegtao_resolution)
    captures = []
    for path in sorted((output/'frames').glob('*.mfd')):
        data = path.read_bytes()
        w,h = struct.unpack_from('<II', data, 12)
        fmt, = struct.unpack_from('<I', data, 24)
        Image.frombytes('RGBA',(w,h),data[96:],'raw','BGRA' if fmt in (3,4) else 'RGBA').save(path.with_suffix('.png'))
        captures.append(path.with_suffix('.png').name)
    assert len(captures) == args.capture_count, captures
    with (output/'frames/index.csv').open(newline='') as index:
        timeline = list(csv.DictReader(index))
    replay_captures = sum(row['replay'] == '1' for row in timeline)
    assert len(timeline) == args.capture_count, 'capture timeline is incomplete'
    if args.require_replay:
        assert replay_captures > 0, 'No interpolated frames captured; reduce GPU-side crop/readback cost'
    result = dict(executable=str(args.exe), sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                  ao=args.ao, aa=args.aa, unlocked=args.unlocked, foreground=args.foreground,
                  implementation=args.implementation,
                  xegtao_quality=args.xegtao_quality, xegtao_denoise=args.xegtao_denoise,
                  xegtao_resolution=args.xegtao_resolution,
                  xegtao_execution=xegtao_evidence,
                  gpu_validation=args.gpu_validation, gpu_timings=args.gpu_timings,
                  capture_trigger=args.capture_trigger, capture_delay=args.capture_delay,
                  capture_stride=args.capture_stride,
                  capture_rect=args.capture_rect, replay_captures=replay_captures,
                  size=args.size, captures=captures)
    (output/'result.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
    print(output)


if __name__ == '__main__':
    main()
