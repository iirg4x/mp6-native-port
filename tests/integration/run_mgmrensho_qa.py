"""Bounded Endurance Alley visual/lifecycle checks with virtual PAD only."""
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
from tests.integration.run_mgmfree_qa import log_errors, event_count

ROUND = 'pressuntil:a/qa.endurance.choice/round;timeout:5000;wait:240;'
PREFIX = ('period:20;timeout:100000;pressuntil:start/ovl.start/mgmrenshodll;' + ROUND)
QUIT = ('pressuntil:b/qa.endurance.choice/quit;wait:120;' +
        ''.join('stick:left;wait:1;' for _ in range(35)) + 'wait:10;'
        'pressuntil:a/qa.endurance.back/quit;')
COMPLETE = 'pressuntil:a/qa.endurance.complete/returned;wait:120'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=('lifecycle', 'reload', 'round'), default='lifecycle')
    parser.add_argument('--widescreen', action='store_true')
    parser.add_argument('--ao', action='store_true')
    parser.add_argument('--unlocked-fps', action='store_true')
    parser.add_argument('--texture-diagnostics', action='store_true')
    parser.add_argument('--window-size', default='960x540')
    parser.add_argument('--capture-trigger', default='qa.endurance.choice')
    parser.add_argument('--capture-delay', type=int, default=40)
    parser.add_argument('--capture-stride', type=int, default=20)
    parser.add_argument('--capture-count', type=int, default=8)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    if not 0 < args.timeout <= 120 or args.capture_delay < 0 or min(args.capture_count, args.capture_stride) < 1:
        parser.error('invalid bounded timeout/capture parameters')
    directory = ROOT / 'build/mgmrensho-qa/runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    directory.mkdir(parents=True)
    print('Evidence:', directory, flush=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
    env.update(
        MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel', MP6_TEST_LOAD_DLL='mgmrenshodll',
        MP6_TEST_LOAD_DLL_HOLD='1', MP6_QA_ENDURANCE_UNLOCK='1',
        MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'),
        MP6_TICK_HZ='0', MP6_ENH_HEAP_SCALE='4', MP6_WINDOW_SIZE=args.window_size,
        MP6_VSYNC='0', MP6_UNLOCKED_FPS=str(int(args.unlocked_fps)),
        MP6_ENH_WIDESCREEN=str(int(args.widescreen)), MP6_WIDESCREEN=str(int(args.widescreen)),
        MP6_FREE_ASPECT='1', MP6_ENH_AMBIENT_OCCLUSION=str(int(args.ao)), MP6_ENH_AA='0',
        MP6_FRAME_DUMP=str(directory / 'frames'), MP6_FRAME_DUMP_COUNT=str(args.capture_count),
        MP6_FRAME_DUMP_STRIDE=str(args.capture_stride), MP6_FRAME_DUMP_DELAY=str(args.capture_delay),
        MP6_FRAME_DUMP_TRIGGER=args.capture_trigger, SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
    exe = ROOT / 'build/mgmrensho-qa/release/mp6native.exe'
    if args.texture_diagnostics:
        env['MP6_QA_ENDURANCE_TEXTURES'] = '1'
    script = PREFIX + QUIT + COMPLETE
    if args.scenario == 'reload':
        env['MP6_QA_ENDURANCE_RELOAD'] = '1'
        script = PREFIX + QUIT + ROUND + QUIT + COMPLETE
    elif args.scenario == 'round':
        # Choose one implemented game, then let its actual gameplay/result
        # code return. No score, outcome or Endurance state is fabricated.
        env['MP6_QA_ENDURANCE_ROUND'] = '1'
        script = PREFIX + ('pressuntil:a/ovl.start/m602dll;timeout:18000;'
                           'waitev:qa.endurance.result/returned;wait:800')
    isolate_background(env, exe)
    timeout = False
    with (directory / 'game.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(exe), '1000000', '--input-script', script],
            cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
            creationflags=background_process_flags())
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timeout = True
            process.terminate()
            code = process.wait(timeout=10)
    text = (directory / 'game.log').read_text(errors='replace')
    expected = [('ovl.start', 'mgmrenshodll'), ('qa.endurance.enter', 'prolog'),
        ('qa.endurance.choice', 'round'), ('qa.endurance.choice', 'quit'),
        ('qa.endurance.back', 'quit'), ('qa.endurance.epilog', 'done'),
        ('qa.endurance.complete', 'returned')]
    if args.scenario == 'round':
        expected = expected[:3] + [('qa.endurance.fixture', 'm602dll'),
            ('ovl.start', 'instdll'), ('ovl.start', 'm602dll'),
            ('qa.endurance.epilog', 'done'), ('qa.endurance.result', 'returned')]
    missing = [f'{k}={v}' for k, v in expected if not event_count(text, k, v)]
    if args.scenario == 'reload':
        for key, value in expected[:-1]:
            if event_count(text, key, value) != 2:
                missing.append(f'{key}={value}: expected two visits')
    captures = len(list((directory / 'frames').glob('*.mfd')))
    if captures != args.capture_count:
        missing.append(f'captures {captures}/{args.capture_count}')
    errors = log_errors(text)
    result = dict(exit_code=code, timed_out=timeout, missing=missing, errors=errors,
        captures=captures, scenario=args.scenario, widescreen=args.widescreen, window_size=args.window_size,
        ao=args.ao, unlocked_fps=args.unlocked_fps,
        passed=code == 0 and not timeout and not missing and not errors)
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
