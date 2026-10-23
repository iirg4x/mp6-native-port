"""Isolated graphical lifecycle checks for the recovered instruction overlay.

The executable is driven only by the native PAD-script seam.  Each scenario
uses a hidden QA window, dummy audio, a fresh working directory, no player
input devices, and a finite timeout.  Run `build_inst_qa.py` first.
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


PREFIX = (
    'period:20;timeout:100000;'
    'pressuntil:start/ovl.start/instdll;'
    'pressuntil:a/qa.inst.ready/menu;timeout:5000;'
)

SCRIPTS = {
    'back': PREFIX + (
        'wait:220;'
        'pressuntil:b/qa.inst.back/return;'
        'waitev:ovl.start/mdseldll;wait:220'
    ),
    'start': PREFIX + (
        'wait:220;'
        'pressuntil:start/qa.inst.start/game;'
        'waitev:ovl.start/m616dll;'
        'pressuntil:a/qa.inst.game/m616;'
        'pressuntil:a/qa.inst.game_return/return;'
        # m616 returns through instdll's prior-game path to mode select.  The
        # QA bridge then performs one fresh instdll load for a second menu.
        'waitev:qa.inst.enter;'
        'waitev:ovl.start/mdseldll;'
        'waitev:ovl.start/instdll;'
        'pressuntil:a/qa.inst.ready;'
        'pressuntil:b/qa.inst.back/return;'
        'waitev:ovl.start/mdseldll;wait:220'
    ),
}


def log_errors(text):
    """Return fatal/runtime errors, including a codec retry-loop symptom."""
    markers = (
        '[FATAL]', '[CRASH]', 'script.timeout=', '[AURORA ERROR',
        'Validation Error', 'memory free error', 'THPSimpleOpen fail',
    )
    return [line for line in text.splitlines()
            if any(marker in line for marker in markers)]


def event_count(text, key, value):
    return text.count(f'[EVENT] {key}={value}')


def run(scenario, output, disc, args):
    directory = output / scenario
    directory.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('MP6_')}
    env.update(
        MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel',
        MP6_TEST_LOAD_DLL='instdll', MP6_TEST_LOAD_DLL_HOLD='1',
        MP6_QA_INST_END='1',
        MP6_DISC_ROOT=str(disc), MP6_TICK_HZ='0', MP6_ENH_HEAP_SCALE='4',
        MP6_WINDOW_SIZE=args.window_size, MP6_VSYNC='0', MP6_UNLOCKED_FPS='0',
        MP6_ENH_WIDESCREEN=str(int(args.widescreen)),
        MP6_WIDESCREEN=str(int(args.widescreen)), MP6_FREE_ASPECT='1',
        MP6_ENH_AMBIENT_OCCLUSION='0', MP6_ENH_AA='0',
        MP6_FRAME_DUMP=str(directory / 'frames'),
        MP6_FRAME_DUMP_COUNT=str(args.capture_count),
        MP6_FRAME_DUMP_STRIDE=str(args.capture_stride),
        MP6_FRAME_DUMP_DELAY=str(args.capture_delay),
        MP6_FRAME_DUMP_TRIGGER=args.capture_trigger,
        SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
    if scenario == 'start':
        env['MP6_QA_INST_RETURN'] = '1'

    executable = ROOT / 'build/inst-qa/release/mp6native.exe'
    if not executable.is_file():
        raise FileNotFoundError(f'build first: {executable}')
    isolate_background(env, executable)
    timed_out = False
    with (directory / 'game.log').open('w', encoding='utf-8') as log:
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

    text = (directory / 'game.log').read_text(encoding='utf-8', errors='replace')
    expected = [
        ('ovl.start', 'instdll'),
        ('qa.inst.enter', f'm{args.game}'),
        ('qa.inst.ready', 'menu'),
    ]
    missing = []
    if scenario == 'back':
        expected.extend((
            ('qa.inst.back', 'return'),
            ('ovl.start', 'mdseldll'),
        ))
    else:
        expected.extend((
            ('qa.inst.start', 'game'),
            ('ovl.start', 'm616dll'),
            ('qa.inst.game', 'm616'),
            ('qa.inst.game_return', 'return'),
            ('qa.inst.back', 'return'),
            ('ovl.start', 'mdseldll'),
        ))
        if event_count(text, 'ovl.start', 'instdll') < 2:
            missing.append('ovl.start=instdll (initial/reload)')
        if event_count(text, 'qa.inst.enter', 'm616') < 2:
            missing.append('qa.inst.enter=m616 (reentry)')
        if event_count(text, 'qa.inst.ready', 'menu') < 2:
            missing.append('qa.inst.ready=menu (fresh reload)')
    for key, value in expected:
        if value.endswith(' (reentry)'):
            actual = value[:-len(' (reentry)')]
            if event_count(text, key, actual) < 2:
                missing.append(f'{key}={actual} (twice)')
        elif event_count(text, key, value) < 1:
            missing.append(f'{key}={value}')
    errors = log_errors(text)
    captures = len(list((directory / 'frames').glob('*.mfd')))
    if captures != args.capture_count:
        missing.append(f'frame captures ({captures}/{args.capture_count})')
    result = dict(
        scenario=scenario, exit_code=code, timed_out=timed_out,
        game=args.game,
        widescreen=args.widescreen, window_size=args.window_size,
        capture_trigger=args.capture_trigger, capture_count=captures,
        missing=missing, errors=errors,
        passed=code == 0 and not timed_out and not missing and not errors)
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n',
                                           encoding='utf-8')
    print(json.dumps(result), flush=True)
    return result['passed']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=('both', *SCRIPTS), default='both')
    parser.add_argument('--game', type=int, default=616,
                        help='must match MP6_QA_INST_GAME used for the QA build')
    parser.add_argument('--disc-root', type=Path,
                        default=ROOT / 'build/disc-cache/orig/GP6E01')
    parser.add_argument('--widescreen', action='store_true')
    parser.add_argument('--window-size', default='1280x960')
    parser.add_argument('--capture-trigger', default='qa.inst.ready')
    parser.add_argument('--capture-delay', type=int, default=100)
    parser.add_argument('--capture-stride', type=int, default=15)
    parser.add_argument('--capture-count', type=int, default=6)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    if args.game != 616 and args.scenario != 'back':
        parser.error('only --scenario back supports a non-default instruction route')
    if args.capture_count < 1 or args.capture_stride < 1 or args.capture_delay < 0:
        parser.error('capture count/stride must be positive and delay nonnegative')
    if args.timeout <= 0 or args.timeout > 120:
        parser.error('--timeout must be in (0,120] seconds')
    output = ROOT / 'build/inst-qa/runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    print('Evidence:', output, flush=True)
    results = [run(scenario, output, args.disc_root.resolve(), args)
               for scenario in SCRIPTS if args.scenario in ('both', scenario)]
    return 0 if all(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
