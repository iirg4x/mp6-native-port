"""Muted graphical entrance tests driven by PAD scripts, without desktop input.

Run build_mdmini_qa.py first. Outputs, cards and settings are isolated from the
normal executable's working directory. The handoff scenario enters the
recovered MGMfree menu and proves its input-ready state.
"""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags
# The native player-count menu starts at the number of connected controllers.
# One right input wraps that maximum to one player, independent of this PC's
# attached pads. The script controls P1 only; accepting the default can stall
# character selection waiting for an actual P2 rather than expose a game hang.
PREFIX = ('period:20;timeout:100000;pressuntil:start/ovl.start/mdminidll;'
          'pressuntil:a/qa.mdmini.stage/players;wait:120;stick:right;wait:30;')
SCRIPTS = {
    'lifecycle': PREFIX + (
        'pressuntil:a/qa.mdmini.stage/modes;wait:180;'
        'press:b;waitev:qa.mdmini.stage/characters;'
        'pressuntil:a/qa.mdmini.stage/modes;wait:180;'
        'pressuntil:b/qa.mdmini.confirm/open;'
        'wait:90;stick:left;wait:30;pressuntil:a/qa.mdmini.exit/done;'
        'pressuntil:a/qa.mdmini.complete/reentered;wait:180'),
    'handoff': PREFIX + (
        'pressuntil:a/ovl.start/mgmfreedll;'
        'pressuntil:a/qa.mgmfree.ready/menu;wait:120'),
}


def run(scenario, output, disc, args):
    directory = output / scenario
    directory.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
    env.update(
        MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel', MP6_TEST_LOAD_DLL='mdminidll',
        MP6_TEST_LOAD_DLL_HOLD='1', MP6_DISC_ROOT=str(disc), MP6_TICK_HZ='0',
        MP6_ENH_HEAP_SCALE='4', SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy',
        MP6_FRAME_DUMP=str(directory / 'frames'), MP6_FRAME_DUMP_COUNT=str(args.capture_count),
        MP6_FRAME_DUMP_STRIDE=str(args.capture_stride), MP6_FRAME_DUMP_DELAY=str(args.capture_delay),
        MP6_FRAME_DUMP_TRIGGER=args.capture_trigger,
        MP6_WINDOW_SIZE=args.window_size,
        MP6_ENH_WIDESCREEN=str(int(args.widescreen)), MP6_WIDESCREEN=str(int(args.widescreen)),
        MP6_FREE_ASPECT='1')
    env.update(MP6_ENH_AMBIENT_OCCLUSION=str(args.ao), MP6_ENH_AA='0', MP6_VSYNC='0',
               MP6_UNLOCKED_FPS=str(int(args.unlocked)))
    if args.geometry:
        env['MP6_WS_HSF_DIAG'] = '1'
    if scenario == 'handoff':
        env['MP6_QA_MDMINI_HANDOFF'] = '1'
    executable = ROOT / 'build/mdmini-qa/release/mp6native.exe'
    timed_out = False
    isolate_background(env, executable)
    with (directory / 'game.log').open('w') as log:
        process = subprocess.Popen(
            [str(executable), '1000000', '--input-script', SCRIPTS[scenario]],
            cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
            creationflags=background_process_flags())
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.terminate()
            code = process.wait(timeout=10)
    text = (directory / 'game.log').read_text(errors='replace')
    expected = ['qa.mdmini.stage=' + stage for stage in ('intro', 'players', 'characters', 'modes')]
    if scenario == 'lifecycle':
        expected += ['qa.mdmini.exit=done', 'qa.mdmini.complete=reentered']
    else:
        expected += ['qa.mdmini.stage=launch', 'ovl.start=mgmfreedll',
                     'qa.mgmfree.enter=prolog', 'qa.mgmfree.ready=menu',
                     'qa.mdmini.complete=mgmfree-entered']
    missing = [marker for marker in expected if '[EVENT] ' + marker not in text]
    if args.geometry and args.widescreen and '[MDMINI-QA] tile=1' not in text.splitlines():
        missing.append('native-density Minigame floor fill')
    errors = [line for line in text.splitlines()
              if any(marker in line for marker in ('[FATAL]', '[CRASH]', 'script.timeout=',
                                                  '[AURORA ERROR', 'Validation Error'))]
    if len(list((directory / 'frames').glob('*.mfd'))) != args.capture_count:
        missing.append('requested frame captures')
    result = dict(scenario=scenario, exit_code=code, timed_out=timed_out,
                  widescreen=args.widescreen, window_size=args.window_size,
                  ao=args.ao, unlocked=args.unlocked,
                  capture_trigger=args.capture_trigger,
                  missing=missing, errors=errors,
                  passed=code == 0 and not timed_out and not missing and not errors)
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return result['passed']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=('both', *SCRIPTS), default='both')
    parser.add_argument('--disc-root', type=Path,
                        default=ROOT / 'build/disc-cache/orig/GP6E01')
    parser.add_argument('--widescreen', action='store_true')
    parser.add_argument('--window-size', default='1280x960')
    parser.add_argument('--capture-trigger', default='qa.mdmini.mode.ready')
    parser.add_argument('--capture-delay', type=int, default=100)
    parser.add_argument('--capture-stride', type=int, default=15)
    parser.add_argument('--capture-count', type=int, default=6)
    parser.add_argument('--geometry', action='store_true')
    parser.add_argument('--ao', type=int, choices=(0, 1, 2), default=0)
    parser.add_argument('--unlocked', action='store_true')
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    if args.capture_count < 1 or args.capture_stride < 1 or args.capture_delay < 0:
        parser.error('capture count/stride must be positive and delay nonnegative')
    output = ROOT / 'build/mdmini-qa/runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    print('Evidence:', output, flush=True)
    results = [run(s, output, args.disc_root.resolve(), args) for s in SCRIPTS
               if args.scenario in ('both', s)]
    return 0 if all(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
