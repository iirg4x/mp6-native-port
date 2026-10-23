"""Isolated graphical smoke tests for the recovered Mini-game Tour overlay.

The executable is driven only by the native PAD-script seam.  The run uses a
hidden QA window, dummy audio, a fresh working directory, and a finite timeout;
it never reads or writes the user's saves and never sends desktop/device input.
Run ``build_mgmfree_qa.py`` first.
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
    'pressuntil:start/ovl.start/mgmfreedll;'
    # The recovered intro's HuWinMesWait consumes A while revealing the
    # message.  Stop the repeated confirm on the actual menu-ready event.
    'pressuntil:a/qa.mgmfree.ready/menu;timeout:5000;'
)

# L/R are injected trigger tokens in the QA build only.  The menu's item
# control is the analog D-pad path, so keep it as a stick step to exercise the
# same branch as a controller.  Category changes deliberately sleep ten
# frames; hold the stick long enough to span that transition and the normal
# D-pad repeat delay before waiting for the resulting item event.
MENU_STEPS = (
    'press:r;waitev:qa.mgmfree.category/right;' +
    ''.join('stick:right;wait:1;' for _ in range(35)) +
    'waitev:qa.mgmfree.item/right;'
)

# The captured confirmation lays out Yes/No horizontally and defaults to No.
# Wait for the window-open/reveal animation, then select Yes explicitly.
CONFIRM_QUIT = (
    'wait:120;' + ''.join('stick:left;wait:1;' for _ in range(35)) +
    'wait:10;pressuntil:a/qa.mgmfree.back;'
)

SCRIPTS = {
    'grouping': PREFIX + 'press:r;wait:120;pressuntil:a/qa.mgmfree.group;'
        'wait:120;' + ''.join('stick:right;wait:90;' for _ in range(4)) +
        'pressuntil:b/qa.mgmfree.ready;wait:120;'
        'pressuntil:b/qa.mgmfree.back_prompt;' + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120',
    'instruction': PREFIX + 'press:r;wait:120;pressuntil:a/qa.mgmfree.group;'
        'wait:120;pressuntil:a/qa.inst.ready;wait:180;'
        'pressuntil:b/qa.mgmfree.ready;wait:120;'
        'pressuntil:b/qa.mgmfree.back_prompt;' + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120',
    'parallax': PREFIX + 'wait:11200;' +
        'pressuntil:b/qa.mgmfree.back_prompt;' + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120',
    'preview': PREFIX + ''.join('stick:right;wait:1;' for _ in range(35)) +
        'waitev:qa.mgmfree.item/right;wait:480;'
        'pressuntil:b/qa.mgmfree.back_prompt;' + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120',
    'preview_instruction': PREFIX + ''.join('stick:right;wait:1;' for _ in range(35)) +
        'waitev:qa.mgmfree.item/right;wait:120;'
        'pressuntil:a/qa.inst.ready;wait:360;'
        'pressuntil:b/qa.mgmfree.ready;wait:120;'
        'pressuntil:b/qa.mgmfree.back_prompt;' + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120',
    'audit': PREFIX + 'wait:120;' + ''.join(
        'press:r;wait:180;stick:right;wait:120;stick:down;wait:120;'
        for _ in range(8)) +
        'pressuntil:b/qa.mgmfree.back_prompt;' + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120',
    'lifecycle': PREFIX + MENU_STEPS + (
        'pressuntil:b/qa.mgmfree.back_prompt;'
        'pressuntil:b/qa.mgmfree.back/cancel;'
        'pressuntil:b/qa.mgmfree.back_prompt;'
        + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120'
    ),
    'reload': PREFIX + MENU_STEPS + (
        'pressuntil:b/qa.mgmfree.back_prompt;'
        + CONFIRM_QUIT +
        'pressuntil:a/qa.mgmfree.ready;timeout:5000;'
        'pressuntil:b/qa.mgmfree.back_prompt;'
        + CONFIRM_QUIT +
        'waitev:ovl.start/mdseldll;wait:120'
    ),
}


def log_errors(text):
    """Return fatal/runtime errors, excluding no-controller UX notices."""
    markers = ('[FATAL]', '[CRASH]', 'script.timeout=', '[AURORA ERROR',
               'Validation Error', 'memory free error',
               'GXEnd: vertex data not evenly divisible', 'unmapped vtx attr')
    return [line for line in text.splitlines() if any(marker in line for marker in markers)]


def movie_evidence(text):
    import re
    frames = [int(frame) for frame in re.findall(r'qa\.thp\.frame=decoded num=(\d+)', text)]
    movies = re.findall(r'qa\.thp\.open=(\S+)', text)
    return dict(decoded_frames=len(frames), unique_movies=len(set(movies)),
                movie_starts=len(movies), loop_starts=frames.count(0))


def event_count(text, key, value):
    return text.count(f'[EVENT] {key}={value}')


def run(scenario, output, disc, args):
    directory = output / scenario
    directory.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('MP6_')}
    env.update(
        MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel',
        MP6_TEST_LOAD_DLL='mgmfreedll', MP6_TEST_LOAD_DLL_HOLD='1',
        MP6_QA_DISC_ROOT=str(disc.resolve()), MP6_TICK_HZ='60' if args.unlocked_fps else '0', MP6_ENH_HEAP_SCALE='4',
        MP6_WINDOW_SIZE=args.window_size, MP6_VSYNC='0',
        MP6_UNLOCKED_FPS=str(int(args.unlocked_fps)),
        MP6_ENH_WIDESCREEN=str(int(args.widescreen)),
        MP6_WIDESCREEN=str(int(args.widescreen)), MP6_FREE_ASPECT='1',
        MP6_ENH_AMBIENT_OCCLUSION=str(int(args.ao)), MP6_ENH_AA='0',
        MP6_FRAME_DUMP=str(directory / 'frames'),
        MP6_FRAME_DUMP_COUNT=str(args.capture_count),
        MP6_FRAME_DUMP_STRIDE=str(args.capture_stride),
        MP6_FRAME_DUMP_DELAY=str(args.capture_delay),
        MP6_FRAME_DUMP_TRIGGER=args.capture_trigger,
        SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
    if scenario == 'reload':
        env['MP6_QA_MGMFREE_RELOAD'] = '1'
    if args.unlocked:
        env['MP6_QA_MGMFREE_UNLOCK'] = '1'
    if args.geometry:
        env['MP6_WS_HSF_DIAG'] = '1'

    executable = ROOT / 'build/mgmfree-qa/release/mp6native.exe'
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
        ('ovl.start', 'mgmfreedll'),
        ('qa.mgmfree.enter', 'prolog'),
        ('qa.mgmfree.ready', 'menu'),
        ('qa.mgmfree.category', 'right'),
        ('qa.mgmfree.item', 'right'),
        ('qa.mgmfree.back_prompt', 'open'),
        ('qa.mgmfree.back', 'quit'),
        ('qa.mgmfree.complete', 'returned'),
        ('ovl.start', 'mdseldll'),
    ]
    if scenario == 'lifecycle':
        expected.insert(6, ('qa.mgmfree.back', 'cancel'))
    if scenario == 'audit':
        expected.remove(('qa.mgmfree.item', 'right'))
        if event_count(text, 'qa.mgmfree.category', 'right') < 8:
            expected.append(('qa.mgmfree.all_categories', 'missing'))
    if scenario == 'parallax':
        expected.remove(('qa.mgmfree.category', 'right'))
        expected.remove(('qa.mgmfree.item', 'right'))
    if scenario in ('preview', 'preview_instruction'):
        expected.remove(('qa.mgmfree.category', 'right'))
    if scenario in ('grouping', 'instruction'):
        expected.remove(('qa.mgmfree.item', 'right'))
        expected.append(('qa.mgmfree.group', 'ready'))
    if scenario == 'instruction':
        expected.extend([('qa.inst.enter', 'selected'), ('qa.inst.ready', 'menu')])
    if scenario == 'preview_instruction':
        expected.extend([('qa.inst.enter', 'selected'), ('qa.inst.ready', 'menu')])
    missing = [f'{key}={value}' for key, value in expected
               if event_count(text, key, value) < 1]
    if scenario == 'reload':
        for key, value in (('qa.mgmfree.enter', 'prolog'),
                           ('qa.mgmfree.ready', 'menu')):
            if event_count(text, key, value) < 2:
                missing.append(f'{key}={value} (twice)')
    errors = log_errors(text)
    if f'[QA-CONTENT] root={disc.resolve()}' not in text:
        errors.append('Requested content root was not selected by the runtime')
    video = movie_evidence(text)
    if args.unlocked and video['decoded_frames'] < 2:
        errors.append('Unlocked preview did not decode moving video')
    if args.unlocked and scenario == 'parallax' and video['loop_starts'] < 2:
        errors.append('Preview movie loop was not observed')
    captures = len(list((directory / 'frames').glob('*.mfd')))
    if captures != args.capture_count:
        missing.append(f'frame captures ({captures}/{args.capture_count})')
    result = dict(
        scenario=scenario, exit_code=code, timed_out=timed_out,
        widescreen=args.widescreen, window_size=args.window_size,
        unlocked=args.unlocked, ao=args.ao, unlocked_fps=args.unlocked_fps,
        capture_trigger=args.capture_trigger, capture_count=captures,
        video=video,
        missing=missing, errors=errors,
        passed=code == 0 and not timed_out and not missing and not errors)
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n',
                                           encoding='utf-8')
    print(json.dumps(result), flush=True)
    return result['passed']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=('both', *SCRIPTS), default='both')
    parser.add_argument('--disc-root', type=Path,
                        default=ROOT / 'build/disc-cache/orig/GP6E01')
    parser.add_argument('--widescreen', action='store_true')
    parser.add_argument('--unlocked', action='store_true', help='QA-only in-memory unlock fixture')
    parser.add_argument('--ao', action='store_true')
    parser.add_argument('--geometry', action='store_true', help='dump scene-owned HSF bounds once')
    parser.add_argument('--unlocked-fps', action='store_true')
    parser.add_argument('--window-size', default='1280x960')
    parser.add_argument('--capture-trigger', default='qa.mgmfree.ready')
    parser.add_argument('--capture-delay', type=int, default=100)
    parser.add_argument('--capture-stride', type=int, default=15)
    parser.add_argument('--capture-count', type=int, default=6)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    if args.capture_count < 1 or args.capture_stride < 1 or args.capture_delay < 0:
        parser.error('capture count/stride must be positive and delay nonnegative')
    if args.timeout <= 0 or args.timeout > 120:
        parser.error('--timeout must be in (0,120] seconds')
    output = ROOT / 'build/mgmfree-qa/runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    print('Evidence:', output, flush=True)
    scenarios = ('lifecycle', 'reload') if args.scenario == 'both' else (args.scenario,)
    results = [run(scenario, output, args.disc_root.resolve(), args)
               for scenario in scenarios]
    return 0 if all(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
