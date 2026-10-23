"""Play every recovered minigame to its exit with the heap checked.

Loads each recovered minigame directly (no board), presses A every half
second until the game leaves it, and walks the five HuMem heaps after every
tick (MP6_HEAP_CHECK=1). Recovered code can write past an allocation where
the GameCube had harmless neighbours; natively that breaks the next heap
block's header, which usually only crashes later -- on the web, where the
heap walk reads out of bounds at the overlay's exit. This catches it at the
tick it happens, and also reports crashes (MP6_CRASH_VEH=1) and minigames
that never leave.

    python tools/minigame_exit_sweep.py [-j N] [m613dll ...]

Needs a release build (tools/build.py --configuration release) and the
disc (MP6_DISC_ROOT). Exits 1 when any minigame fails.
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
TICKS = 60000  # boot to the minigame takes ~20k uncapped ticks; the longest games ~25k more


def minigames():
    names = set()
    for rel, _flags in build.native_rel_sources():
        parts = rel.split('/')
        if len(parts) > 3 and re.fullmatch(r'm6\d\ddll', parts[2].lower()):
            names.add(parts[2].lower())
    return sorted(names)


def run(module):
    env = dict(os.environ, MP6_LAUNCHER='0', MP6_BOOT_TO='selmenu', MP6_AUTO_START_TICKS='60,150',
               MP6_TEST_LOAD_DLL=module, MP6_TEST_LOAD_DLL_HOLD='1', MP6_TICK_HZ='0', MP6_BACKGROUND_QA='1',
               MP6_HEAP_CHECK='1', MP6_CRASH_VEH='1', SDL_AUDIODRIVER='dummy')
    script = f'timeout:{TICKS};waitev:ovl.start/{module};' + 'press:a;wait:30;' * 1500
    out = subprocess.run([str(EXE), str(TICKS), '--input-script', script], cwd=EXE.parent, env=env,
                         capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=3600)
    log = out.stdout + out.stderr
    starts = re.findall(r'\[EVENT\] ovl\.start=(\S+)', log)
    problems = []
    if module not in [s.lower() for s in starts]:
        problems.append('never started')
    else:
        after = starts[[s.lower() for s in starts].index(module) + 1:]
        if not after:
            problems.append('never left')
    problems += re.findall(r'\[MP6-HEAP\] [^\n]*', log)
    crash = re.search(r'\[MP6-CRASH\][^\n]*(?:\n[^\n]*){0,3}', log)
    if crash:
        problems.append(crash.group(0))
    elif out.returncode not in (0,):
        problems.append(f'exit code {out.returncode}')
    return module, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('modules', nargs='*')
    parser.add_argument('-j', type=int, default=3)
    args = parser.parse_args()
    if not EXE.exists():
        raise SystemExit(f'no release build at {EXE}')
    modules = [m.lower() for m in args.modules] or minigames()
    failed = 0
    with ThreadPoolExecutor(args.j) as pool:
        for module, problems in pool.map(run, modules):
            if problems:
                failed += 1
                print(f'FAIL {module}: ' + ' | '.join(problems), flush=True)
            else:
                print(f'ok   {module}', flush=True)
    print(f'{len(modules)} minigame(s), {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
