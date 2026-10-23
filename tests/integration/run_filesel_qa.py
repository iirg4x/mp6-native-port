"""Exercise real file creation/copy/erase using only isolated test memory cards."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags, MARKER
from tests.integration.run_title_qa import errors_in_log


def action_script(stage):
    prefix = ('period:30;timeout:9000000;pressuntil:start/title.opening/1;'
              'waitev:title.ready/1;press:start;'
              'pressuntil:a/qa.filesel.ready/1;timeout:3500;wait:60;')
    actions = {
        'create': ('pressuntil:a/qa.filesel.name/ready;wait:90;press:a;wait:20;'
                   'press:start;wait:30;pressuntil:a/qa.filesel.done/create;wait:240'),
        'copy': ('pressuntil:a/qa.filesel.options/ready;wait:60;stick:right;wait:20;'
                 'press:a;waitev:qa.filesel.copy/ready;wait:90;'
                 'pressuntil:a/qa.filesel.done/copy;'
                 'pressuntil:a/qa.filesel.ready/2;wait:240'),
        'erase': ('stick:right;wait:30;pressuntil:a/qa.filesel.options/ready;'
                  'wait:60;stick:right;wait:20;stick:right;wait:20;press:a;'
                  'waitev:qa.filesel.erase/ready;wait:90;stick:left;wait:20;'
                  'pressuntil:a/qa.filesel.done/erase;'
                  'pressuntil:a/qa.filesel.ready/2;wait:240'),
        'verify': 'wait:500',
    }
    return prefix + actions[stage]


def card_hashes(directory):
    return {str(p.relative_to(directory)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.rglob('*')) if p.is_file()}


def validate_log(log, stage, code, timed_out):
    slots = [tuple(map(int, values)) for values in re.findall(
        r'\[FILESEL-QA\] slots=(\d+),(\d+),(\d+) selected=\d+', log)]
    expected = {'create': (0, 0, 0), 'copy': (1, 0, 0),
                'erase': (1, 1, 0), 'verify': (1, 0, 0)}[stage]
    errors = errors_in_log(log)
    if code != 0 or timed_out:
        errors.append('process failed or exceeded wall-clock bound')
    if MARKER not in log or 'verified hidden=1 input-subsystems=0' not in log:
        errors.append('background/controller isolation was not verified')
    if not slots or slots[0] != expected:
        errors.append(f'initial saved slots differ from {expected}: {slots}')
    if f'[EVENT] qa.filesel.done={stage} ' not in log:
        errors.append('native action did not complete')
    if stage in ('copy', 'erase'):
        final = (1, 1, 0) if stage == 'copy' else (1, 0, 0)
        if len(slots) < 2 or slots[-1] != final:
            errors.append(f'native menu did not return with slots {final}')
        choice = 1 if stage == 'copy' else 2
        if f'[EVENT] qa.filesel.choice=selected num={choice} ' not in log:
            errors.append('expected native menu choice was not observed')
    return slots, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--stage', required=True, choices=('create', 'copy', 'erase', 'verify'))
    parser.add_argument('--seed', help='successful prior filesel QA run, inside build/filesel-qa-runs')
    parser.add_argument('--window-size', default='640x480', choices=('640x480', '848x480', '1120x480'))
    parser.add_argument('--seconds', type=int, default=300)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name) or args.seconds < 1:
        parser.error('simple run name and positive seconds required')
    if bool(args.seed) != (args.stage != 'create'):
        parser.error('only create starts fresh; other stages require an isolated QA seed')
    exe = ROOT / 'build/title-qa/release/mp6native.exe'
    binary_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    ready = json.loads((exe.parent / 'title-qa-ready.json').read_text())
    builder_hash = hashlib.sha256((ROOT / 'tests/integration/build_title_qa.py').read_bytes()).hexdigest()
    if ready != {'sha256': binary_hash, 'builder_sha256': builder_hash}:
        parser.error('QA executable/probes changed; rebuild first')
    run_root = ROOT / 'build/filesel-qa-runs'
    seed = None
    if args.seed:
        seed = (ROOT / args.seed).resolve(strict=True)
        if not seed.is_relative_to(run_root.resolve()) or not seed.is_dir():
            parser.error('seed must be inside build/filesel-qa-runs')
        previous = json.loads((seed / 'result.json').read_text())
        needed = {'copy': 'create', 'erase': 'copy', 'verify': 'erase'}[args.stage]
        if not previous['passed'] or previous['stage'] != needed:
            parser.error('seed must be a successful ' + needed + ' test')
        if previous['cards_after'] != card_hashes(seed / 'saves') or not previous['cards_after']:
            parser.error('seed card contents changed or are missing')
    directory = run_root / args.name
    directory.mkdir(parents=True, exist_ok=False)
    if seed:
        shutil.copytree(seed / 'saves', directory / 'saves')
    before = card_hashes(directory / 'saves')
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
    wide = str(int(args.window_size != '640x480'))
    event = {'create': 'qa.filesel.name', 'copy': 'qa.filesel.options',
             'erase': 'qa.filesel.options', 'verify': 'qa.filesel.ready'}[args.stage]
    env.update(MP6_LAUNCHER='0', MP6_TICK_HZ='0',
               MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'),
               MP6_GPU_CACHE_PATH=str(directory / 'gpu-cache'),
               MP6_WINDOW_SIZE=args.window_size, MP6_VSYNC='0', MP6_UNLOCKED_FPS='0',
               MP6_ENH_WIDESCREEN=wide, MP6_WIDESCREEN=wide, MP6_FREE_ASPECT='1',
               MP6_ENH_AMBIENT_OCCLUSION='0', MP6_ENH_AA='0',
               MP6_FILESEL_QA_STAGE=args.stage,
               MP6_FRAME_DUMP=str(directory / 'frames'), MP6_FRAME_DUMP_COUNT='120',
               MP6_FRAME_DUMP_STRIDE='5', MP6_FRAME_DUMP_TRIGGER=event)
    isolate_background(env, exe)
    start = time.monotonic()
    timed_out = False
    with (directory / 'game.log').open('w', encoding='utf-8') as log:
        proc = subprocess.Popen([str(exe), '10000000', '--input-script', action_script(args.stage)],
                                cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=background_process_flags())
        print(f'{args.stage} {args.window_size}: hidden PID {proc.pid}', flush=True)
        try:
            code = proc.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.terminate()
            code = proc.wait(timeout=10)
    log = (directory / 'game.log').read_text(encoding='utf-8', errors='replace')
    slots, errors = validate_log(log, args.stage, code, timed_out)
    captures = len(list((directory / 'frames').glob('*.mfd')))
    after = card_hashes(directory / 'saves')
    if not after or (args.stage != 'verify' and after == before):
        errors.append('native card write was not persisted')
    if args.stage == 'verify' and after != before:
        errors.append('read-only verification changed card contents')
    if captures < 16:
        errors.append('insufficient rendered action coverage')
    result = dict(passed=not errors, stage=args.stage, size=args.window_size,
                  sha256=binary_hash, seed=str(seed) if seed else None,
                  elapsed_seconds=round(time.monotonic() - start, 2),
                  exit_code=code, timed_out=timed_out, slots=slots, captures=captures,
                  cards_before=before, cards_after=after, errors=errors)
    (directory / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
    print('Log:', directory / 'game.log')
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
