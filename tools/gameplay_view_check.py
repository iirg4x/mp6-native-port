"""Check that widescreen only changes the view, never how a minigame plays.

Plays each recovered minigame twice from the same start -- every player a
computer (MP6_TEST_ALL_COM), the random streams pinned (MP6_TEST_FRAND_SEED),
the same scripted input -- once in a 4:3 window and once in a 21:9 window
with widescreen on, and compares the per-tick gameplay hashes the game
prints (MP6_GAMEPLAY_TRACE, include/mp6_gameplay_trace.h): random stream,
collision world, objects, model transforms and player records. Any
difference is gameplay that depends on the screen shape.

    python tools/gameplay_view_check.py [-j N] [--repeat] [m613dll ...]

--repeat runs the 4:3 case twice instead, to confirm a minigame is
repeatable at all. Model transforms (m=) are reported separately: the
widescreen layer legitimately moves and scales backdrop models, so an m-only
difference names its models (MP6_GAMEPLAY_TRACE_MODELS) for review.

Needs a release build (tools/build.py --configuration release) and the disc
(MP6_DISC_ROOT). Exits 1 when any minigame's gameplay differs.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402

EXE = ROOT / 'build' / 'release' / 'mp6native.exe'
TICKS = 60000
SEED = '0x1234567'
FIELDS = ('r', 'c', 'o', 'm', 'p')
LINE = re.compile(r'^\[GPLAY\] t=(\d+) r=(\S+) c=(\S+) o=(\S+) m=(\S+) p=(\S+)', re.M)
MODEL = re.compile(r'^\[GPLAY-(MODEL|OBJECT)\] t=\d+ id=(\d+) (.*)$', re.M)
BELOW_NORMAL = 0x00004000


def minigames():
    names = set()
    for rel, _flags in build.native_rel_sources():
        parts = rel.split('/')
        if len(parts) > 3 and re.fullmatch(r'm6\d\ddll', parts[2].lower()):
            names.add(parts[2].lower())
    return sorted(names)


def play(module, size, wide, extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_') or k in ('MP6_DISC_ROOT',)}
    env.update(MP6_LAUNCHER='0', MP6_BOOT_TO='selmenu', MP6_AUTO_START_TICKS='60,150',
               MP6_TEST_LOAD_DLL=module, MP6_TEST_LOAD_DLL_HOLD='1', MP6_TICK_HZ='0',
               MP6_BACKGROUND_QA='1', MP6_ENH_HEAP_SCALE='4', MP6_VSYNC='0',
               MP6_TEST_ALL_COM='1', MP6_TEST_FRAND_SEED=SEED, MP6_GAMEPLAY_TRACE='2',
               MP6_WINDOW_SIZE=size, MP6_FREE_ASPECT='1',
               MP6_ENH_WIDESCREEN=str(int(wide)), MP6_WIDESCREEN=str(int(wide)),
               SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy', **(extra or {}))
    script = f'timeout:{TICKS};waitev:ovl.start/{module};' + 'press:a;wait:30;' * 1500
    flags = BELOW_NORMAL if os.name == 'nt' else 0
    out = subprocess.run([str(EXE), str(TICKS), '--input-script', script], cwd=EXE.parent, env=env,
                         capture_output=True, text=True, encoding='utf-8', errors='replace',
                         timeout=3600, creationflags=flags)
    return out.stdout + out.stderr


def trace(log):
    return [m.groups() for m in LINE.finditer(log)]


def models(log):
    return {f'{kind.lower()} {mid}': rest for kind, mid, rest in MODEL.findall(log)}


def compare(a, b):
    """First tick where each field differs, and whether the runs differ in length."""
    first = {}
    for x, y in zip(a, b):
        for i, field in enumerate(FIELDS):
            if field not in first and x[i + 1] != y[i + 1]:
                first[field] = int(x[0])
    return first


def check(module, repeat):
    runs = [('640x480', False), ('640x480', False)] if repeat else [('640x480', False), ('1120x480', True)]
    with ThreadPoolExecutor(2) as pool:
        logs = list(pool.map(lambda run: play(module, *run), runs))
    traces = [trace(log) for log in logs]
    if not traces[0] or not traces[1]:
        return module, 'NO TRACE', []
    ended = ['[GPLAY-END]' in log for log in logs]
    first = compare(*traces)
    notes = []
    if len(traces[0]) != len(traces[1]):
        notes.append(f'length {len(traces[0])} vs {len(traces[1])} ticks')
    if not all(ended):
        notes.append('did not finish: ' + ', '.join(run[0] for run, e in zip(runs, ended) if not e))
    gameplay = {f: t for f, t in first.items() if f != 'm'}
    if gameplay:
        verdict = 'DIFFERS'
        notes.append(' '.join(f'{f}@{t}' for f, t in sorted(gameplay.items(), key=lambda kv: kv[1])))
    elif len(traces[0]) != len(traces[1]):
        verdict = 'DIFFERS'
    else:
        verdict = 'same'
    if 'm' in first or 'o' in first:
        tick = min(first.get('m', 1 << 30), first.get('o', 1 << 30))
        extra = {'MP6_GAMEPLAY_TRACE_MODELS': str(tick)}
        with ThreadPoolExecutor(2) as pool:
            pair = [models(log) for log in pool.map(lambda run: play(module, *run, extra), runs)]
        moved = [f'{key}: {pair[0].get(key)} -> {pair[1].get(key)}'
                 for key in sorted(set(pair[0]) | set(pair[1])) if pair[0].get(key) != pair[1].get(key)]
        notes.append(f'differ at t={tick}: ' + '; '.join(moved))
    return module, verdict, notes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('modules', nargs='*')
    parser.add_argument('-j', type=int, default=1, help='minigames in parallel (two game processes each)')
    parser.add_argument('--repeat', action='store_true', help='compare two 4:3 runs (repeatability)')
    args = parser.parse_args()
    if not EXE.exists():
        raise SystemExit(f'no release build at {EXE}')
    modules = [m.lower() for m in args.modules] or minigames()
    failed = 0
    with ThreadPoolExecutor(args.j) as pool:
        for module, verdict, notes in pool.map(lambda m: check(m, args.repeat), modules):
            failed += verdict != 'same'
            print(f'{verdict:8} {module} ' + ' | '.join(notes), flush=True)
    print(f'{len(modules) - failed}/{len(modules)} minigames play the same', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
