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
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--windowed', action='store_true')
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--visits', type=int, default=3)
    parser.add_argument('--capture', action='store_true', help='capture file select in a rendered run')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('name must be a single simple directory name')
    if args.seconds < 1 or not 1 <= args.visits <= 20:
        parser.error('positive seconds and 1..20 visits required')
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
               MP6_WINDOW_SIZE='960x720')
    if args.capture:
        if not args.windowed:
            parser.error('--capture requires --windowed')
        env.update(MP6_FRAME_DUMP=str(directory / 'frames'), MP6_FRAME_DUMP_COUNT='30',
                   MP6_FRAME_DUMP_STRIDE='15', MP6_FRAME_DUMP_TRIGGER='title.filesel')
    script = ('period:30;timeout:9000000;pressuntil:start/title.opening/1;'
              f'waitev:title.ready/{args.visits};press:start;'
              'waitev:title.filesel/entered;wait:600')
    start = time.monotonic()
    timed_out = False
    with (directory / 'game.log').open('w', encoding='utf-8') as log:
        proc = subprocess.Popen([str(exe), '10000000', '--input-script', script],
                                cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT)
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
              and not any(marker in log for marker in (
                  'script.timeout', '[FATAL]', '[CRASH]', 'memory free error')))
    memory = [dict(zip(('visit', 'heap', 'dvd', 'model', 'anim'), map(int, row)))
              for row in re.findall(r'\[TITLE-QA\] memory visit=(\d+) heap=(\d+) dvd=(\d+) model=(\d+) anim=(\d+)', log)]
    memory_stable = (len(memory) == args.visits and all(
        all(row[key] == memory[1][key] for key in ('heap', 'dvd', 'model', 'anim'))
        for row in memory[2:]))
    filesel_tick = re.search(r'\[EVENT\] title.filesel=entered num=1 tick=(\d+)', log)
    completed_ticks = re.search(r'reached (\d+) VIWaitForRetrace ticks \(limit (\d+)\) -- (?:shutting down Aurora, )?exiting 0', log)
    completed = (filesel_tick is not None and completed_ticks is not None
                 and int(completed_ticks[1]) == int(completed_ticks[2]) == int(filesel_tick[1]) + 600)
    passed = passed and memory_stable and completed
    report = dict(passed=passed, exit_code=code, timed_out=timed_out,
                  elapsed_seconds=round(time.monotonic() - start, 2),
                  title_visits=visits, memory=memory, memory_stable=memory_stable,
                  post_filesel_ticks=600 if completed else None, executable=str(exe), sha256=binary_hash)
    (directory / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    print('Log:', directory / 'game.log')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
