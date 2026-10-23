"""Bounded Tournament menu/duel checks using a hidden window and virtual PAD."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags
from tests.integration.run_mgmfree_qa import event_count, log_errors


PREFIX = (
    'period:20;timeout:100000;'
    'pressuntil:start/ovl.start/mgmtournamentdll;'
    # The overlay's opening HuWinMesWait consumes A before state 6 is
    # settled; repeat the real A input until the state assertion holds.
    'pressuntil:a/qa.mgmtournament.state/6;timeout:12000;'
)
QUIT = (
    # B backs out of the round prompt and opens message 5.  Stop driving
    # B once that confirmation prompt is observed; another B there would
    # cancel the confirmation instead of selecting Quit.
    'pressuntil:a/qa.mgmtournament.prompt/round;'
    'wait:220;pressuntil:b/qa.mgmtournament.prompt/quit;'
    'wait:120;'
    + ''.join('stick:left;wait:1;' for _ in range(35))
    + 'wait:10;pressuntil:a/qa.mgmtournament.choice/quit;'
    + 'waitev:qa.mgmtournament.back/quit;'
)
COMPLETE = 'pressuntil:a/qa.mgmtournament.complete/returned;wait:120'

# m662 has one human in the first pair (configured by the QA-only scene
# fixture).  The initial stick selects a tree; repeated A pulses cover the
# short sideways/settle window without inventing a result.  The final wait
# lets the native CPU and player state machines finish before observing the
# overlay return, with no further input that could leak into Tournament.
M662_INPUT = (
    'wait:180;stick:right;' +
    ''.join('wait:30;press:a;' for _ in range(10)) +
    'wait:900;'
)


SCRIPTS = {
    # Real Tournament entry, menu quit, and a second entry in the same
    # process.  The bridge's reload seam only runs after the first return.
    'lifecycle': PREFIX + QUIT + COMPLETE,
    'reload': PREFIX + QUIT + (
        'waitev:ovl.start/mgmtournamentdll;wait:120;'
        'pressuntil:a/qa.mgmtournament.state/6;timeout:12000;'
        + QUIT + COMPLETE
    ),
    # Select the recovered m662 Trick or Tree Duel through the real prompt,
    # finish its native round naturally, then leave Tournament normally.
    'round': PREFIX + (
        'pressuntil:a/qa.mgmtournament.prompt/round;'
        'stick:right;wait:20;stick:right;wait:20;'
        'pressuntil:a/qa.mgmtournament.choice/inst;'
        'waitev:ovl.start/instdll;'
        'waitev:qa.mgmtournament.inst/ready;'
        'pressuntil:start/qa.mgmtournament.inst_start/game;'
        'waitev:ovl.start/m662dll;' +
        M662_INPUT +
        'waitev:ovl.return;'
        'waitev:qa.mgmtournament.state/10;'
        'wait:240;' + QUIT + COMPLETE
    ),
}


def expected_events(scenario):
    common = [
        ('ovl.start', 'mgmtournamentdll'),
        ('qa.mgmtournament.enter', 'menu'),
        ('qa.mgmtournament.state', '6'),
    ]
    if scenario == 'round':
        return common + [
            ('qa.mgmtournament.fixture', 'm662dll'),
            ('qa.mgmtournament.prompt', 'round'),
            ('qa.mgmtournament.choice', 'inst'),
            ('qa.mgmtournament.route', 'inst'),
            ('ovl.start', 'instdll'),
            ('qa.mgmtournament.inst', 'entered'),
            ('qa.mgmtournament.inst', 'ready'),
            ('qa.mgmtournament.inst_start', 'game'),
            ('ovl.start', 'm662dll'),
            ('ovl.return', '-'),
            ('qa.mgmtournament.state', '10'),
            ('qa.mgmtournament.coin', 'mgCoinBonus'),
            ('qa.mgmtournament.choice', 'quit'),
            ('qa.mgmtournament.back', 'quit'),
            ('qa.mgmtournament.epilog', 'done'),
            ('qa.mgmtournament.complete', 'returned'),
        ]
    return common + [
        ('qa.mgmtournament.choice', 'quit'),
        ('qa.mgmtournament.back', 'quit'),
        ('qa.mgmtournament.epilog', 'done'),
        ('qa.mgmtournament.complete', 'returned'),
    ]


def validate_log(text, scenario, captures, capture_count):
    """Return a deterministic validation result without launching a process."""
    missing = [f'{key}={value}' for key, value in expected_events(scenario)
               if event_count(text, key, value) < 1]
    if scenario == 'reload':
        for key, value in (('ovl.start', 'mgmtournamentdll'),
                           ('qa.mgmtournament.enter', 'menu'),
                           ('qa.mgmtournament.state', '6'),
                           ('qa.mgmtournament.choice', 'quit'),
                           ('qa.mgmtournament.epilog', 'done')):
            if event_count(text, key, value) < 2:
                missing.append(f'{key}={value} (twice)')
    if scenario == 'round' and event_count(text, 'qa.mgmtournament.coin', 'mgCoinBonus') < 4:
        missing.append('qa.mgmtournament.coin=mgCoinBonus (four players)')
    if scenario == 'round':
        coin_lines = [
            line for line in text.splitlines()
            if '[EVENT] qa.mgmtournament.coin=mgCoinBonus' in line
        ]
        rewards = []
        for line in coin_lines:
            match = re.search(r'\bnum=(-?\d+)\b', line)
            if match:
                rewards.append(int(match.group(1)))
        if not any(reward > 0 for reward in rewards):
            missing.append('qa.mgmtournament.coin=mgCoinBonus (nonzero winner reward)')
    if captures != capture_count:
        missing.append(f'frame captures ({captures}/{capture_count})')
    errors = log_errors(text)
    return dict(
        scenario=scenario, capture_count=captures, missing=missing,
        errors=errors, passed=not missing and not errors)


def run(scenario, output, disc, args):
    directory = output / scenario
    directory.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('MP6_')}
    env.update(
        MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel',
        MP6_TEST_LOAD_DLL='mgmtournamentdll', MP6_TEST_LOAD_DLL_HOLD='1',
        MP6_QA_MGMTOURNAMENT_UNLOCK='1',
        MP6_DISC_ROOT=str(disc),
        MP6_TICK_HZ='60' if args.unlocked_fps else '0', MP6_ENH_HEAP_SCALE='4',
        MP6_WINDOW_SIZE=args.window_size, MP6_VSYNC='0',
        MP6_UNLOCKED_FPS=str(int(args.unlocked_fps)),
        MP6_ENH_WIDESCREEN=str(int(args.widescreen)),
        MP6_WIDESCREEN=str(int(args.widescreen)), MP6_FREE_ASPECT='1',
        MP6_ENH_AMBIENT_OCCLUSION=str(int(args.ao)), MP6_AO_DIAG='1', MP6_ENH_AA='0',
        MP6_FRAME_DUMP=str(directory / 'frames'),
        MP6_FRAME_DUMP_COUNT=str(args.capture_count),
        MP6_FRAME_DUMP_STRIDE=str(args.capture_stride),
        MP6_FRAME_DUMP_DELAY=str(args.capture_delay),
        MP6_FRAME_DUMP_TRIGGER=args.capture_trigger,
        SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
    if scenario == 'reload':
        env['MP6_QA_MGMTOURNAMENT_RELOAD'] = '1'
    if scenario == 'round':
        env['MP6_QA_MGMTOURNAMENT_GAME'] = '1'
    if args.geometry_diagnostics:
        env['MP6_WS_HSF_DIAG'] = '1'

    executable = ROOT / 'build/mgmtournament-qa/release/mp6native.exe'
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
    captures = len(list((directory / 'frames').glob('*.mfd')))
    result = validate_log(text, scenario, captures, args.capture_count)
    result.update(exit_code=code, timed_out=timed_out,
                  widescreen=args.widescreen, window_size=args.window_size,
                  ao=args.ao, unlocked_fps=args.unlocked_fps,
                  passed=(code == 0 and not timed_out and result['passed']))
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n',
                                           encoding='utf-8')
    print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=('all', *SCRIPTS), default='all')
    parser.add_argument('--disc-root', type=Path,
                        default=ROOT / 'build/disc-cache/orig/GP6E01')
    parser.add_argument('--widescreen', action='store_true')
    parser.add_argument('--ao', action='store_true')
    parser.add_argument('--unlocked-fps', action='store_true')
    parser.add_argument('--geometry-diagnostics', action='store_true')
    parser.add_argument('--window-size', default='1280x720')
    parser.add_argument('--capture-trigger', default='qa.mgmtournament.state=6')
    parser.add_argument('--capture-delay', type=int, default=240)
    parser.add_argument('--capture-stride', type=int, default=15)
    parser.add_argument('--capture-count', type=int, default=6)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    if args.capture_count < 1 or args.capture_stride < 1 or args.capture_delay < 0:
        parser.error('capture count/stride must be positive and delay nonnegative')
    if args.timeout <= 0 or args.timeout > 120:
        parser.error('--timeout must be in (0,120] seconds')
    output = ROOT / 'build/mgmtournament-qa/runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    print('Evidence:', output, flush=True)
    scenarios = tuple(SCRIPTS) if args.scenario == 'all' else (args.scenario,)
    results = [run(scenario, output, args.disc_root.resolve(), args)
               for scenario in scenarios]
    return 0 if all(result == 0 for result in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
