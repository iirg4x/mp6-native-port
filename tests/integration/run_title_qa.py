"""Let the original intro replay twice, then press Start at the third title.

Runs with a fresh, isolated save/config/cache directory. Captures stdout/stderr
and records the exact binary hash. Does not interact with another running game.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags


def errors_in_log(text):
    # Every run uses a newly created empty save directory. CARDOpen returning
    # NOFILE for MARIPA6 is the expected first-run state, not a renderer error.
    expected = '[AURORA ERROR: aurora::card] Failed to open file: MARIPA6'
    return [line for line in text.splitlines() if any(marker in line for marker in (
        'script.timeout', '[FATAL]', '[CRASH]', 'memory free error',
        'Validation Error', '[AURORA ERROR')) and line.strip() != expected]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--windowed', action='store_true')
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--visits', type=int, default=3)
    parser.add_argument('--capture', action='store_true', help='capture file select in a rendered run')
    parser.add_argument('--window-size', default='960x720')
    parser.add_argument('--widescreen', action='store_true')
    parser.add_argument('--capture-phase', choices=('opening', 'title', 'filesel'), default='filesel')
    parser.add_argument('--capture-count', type=int, default=30)
    parser.add_argument('--capture-stride', type=int, default=15)
    parser.add_argument('--capture-delay', type=int, default=0)
    parser.add_argument('--ao', type=int, choices=(0, 1, 2), default=0,
                        help='AO level: 0 off, 1 subtle, 2 strong')
    parser.add_argument('--implementation', choices=('legacy', 'xegtao'), default='legacy',
                        help='AO implementation used for the run')
    parser.add_argument('--gpu-validation', action='store_true',
                        help='Enable optional graphics validation in the isolated process')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('name must be a single simple directory name')
    if args.seconds < 1 or not 1 <= args.visits <= 20:
        parser.error('positive seconds and 1..20 visits required')
    if not re.fullmatch(r'[1-9][0-9]{2,3}x[1-9][0-9]{2,3}', args.window_size):
        parser.error('window size must be WIDTHxHEIGHT')
    if args.capture_count < 1 or not 1 <= args.capture_stride <= 1000 or args.capture_delay < 0:
        parser.error('capture count > 0, stride 1..1000 and delay >= 0 required')
    exe = ROOT / 'build/title-qa' / ('release/mp6native.exe' if args.windowed
                                    else 'release-headless/mp6native_headless.exe')
    binary_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    ready = json.loads((exe.parent / 'title-qa-ready.json').read_text())
    builder_hash = hashlib.sha256((ROOT / 'tests/integration/build_title_qa.py').read_bytes()).hexdigest()
    if ready != {'sha256': binary_hash, 'builder_sha256': builder_hash}:
        parser.error('QA executable/probes changed; rebuild first')
    directory = ROOT / 'build/title-qa-runs' / args.name
    directory.mkdir(parents=True, exist_ok=False)
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
    env.update(MP6_LAUNCHER='0', MP6_TICK_HZ='0',
               MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'),
               MP6_GPU_CACHE_PATH=str(directory / 'gpu-cache'),
               MP6_WINDOW_SIZE=args.window_size, MP6_VSYNC='0', MP6_UNLOCKED_FPS='0',
               MP6_ENH_WIDESCREEN=str(int(args.widescreen)), MP6_WIDESCREEN=str(int(args.widescreen)),
               MP6_FREE_ASPECT='1', MP6_ENH_AMBIENT_OCCLUSION=str(args.ao), MP6_ENH_AA='0',
               MP6_AO_DIAG='1', MP6_AO_XEGTAO=str(int(args.implementation == 'xegtao')),
               MP6_AO_IMPLEMENTATION=args.implementation,
               MP6_GPU_VALIDATION=str(int(args.gpu_validation)),
               SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
    if args.capture:
        if not args.windowed:
            parser.error('--capture requires --windowed')
        event = {'opening': 'title.opening', 'title': 'title.ready', 'filesel': 'title.filesel'}[args.capture_phase]
        env.update(MP6_FRAME_DUMP=str(directory / 'frames'), MP6_FRAME_DUMP_COUNT=str(args.capture_count),
                   MP6_FRAME_DUMP_STRIDE=str(args.capture_stride), MP6_FRAME_DUMP_TRIGGER=event,
                   MP6_FRAME_DUMP_DELAY=str(args.capture_delay))
    script = ('period:30;timeout:9000000;pressuntil:start/title.opening/1;'
              f'waitev:title.ready/{args.visits};press:start;'
              'waitev:title.filesel/entered;wait:600')
    start = time.monotonic()
    timed_out = False
    if args.windowed:
        isolate_background(env, exe)
    with (directory / 'game.log').open('w', encoding='utf-8') as log:
        proc = subprocess.Popen([str(exe), '10000000', '--input-script', script],
                                cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=background_process_flags())
        try:
            code = proc.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.terminate()
            code = proc.wait(timeout=10)
    log = (directory / 'game.log').read_text(encoding='utf-8', errors='replace')
    visits = re.findall(r'\[EVENT\] title.ready=ready num=(\d+)', log)
    passed = (code == 0 and not timed_out and visits == [str(i) for i in range(1, args.visits + 1)]
              and '[EVENT] title.start=accepted' in log
              and '[EVENT] title.filesel=entered' in log
              and not errors_in_log(log))
    memory = [dict(zip(('visit', 'heap', 'dvd', 'model', 'anim'), map(int, row)))
              for row in re.findall(r'\[TITLE-QA\] memory visit=(\d+) heap=(\d+) dvd=(\d+) model=(\d+) anim=(\d+)', log)]
    memory_stable = (len(memory) == args.visits and all(
        all(row[key] == memory[1][key] for key in ('heap', 'dvd', 'model', 'anim'))
        for row in memory[2:]))
    filesel_tick = re.search(r'\[EVENT\] title.filesel=entered num=1 tick=(\d+)', log)
    completed_ticks = re.search(r'reached (\d+) VIWaitForRetrace ticks \(limit (\d+)\) -- (?:shutting down Aurora, )?exiting 0', log)
    completed = (filesel_tick is not None and completed_ticks is not None
                 and int(completed_ticks[1]) == int(completed_ticks[2]) == int(filesel_tick[1]) + 600)
    captures = len(list((directory/'frames').glob('*.mfd')))
    passed = passed and memory_stable and completed and (not args.capture or captures == args.capture_count)
    report = dict(passed=passed, exit_code=code, timed_out=timed_out,
                  elapsed_seconds=round(time.monotonic() - start, 2),
                  title_visits=visits, memory=memory, memory_stable=memory_stable,
                  post_filesel_ticks=600 if completed else None, executable=str(exe), sha256=binary_hash)
    report.update(size=args.window_size, widescreen=args.widescreen, capture_phase=args.capture_phase,
                  captures=captures, errors=errors_in_log(log), ao=args.ao,
                  implementation=args.implementation, gpu_validation=args.gpu_validation)
    (directory / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    print('Log:', directory / 'game.log')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
