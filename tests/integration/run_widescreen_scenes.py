"""Muted native scene captures. A capture pass is not a gameplay-completion claim."""
import argparse
import csv
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXE = ROOT/'build/widescreen-qa/release/mp6native.exe'
sys.path.insert(0, str(ROOT))
from tools import audit_widescreen
from tests.integration.background_qa import isolate_background, background_process_flags
from tests.integration.run_filesel_qa import card_hashes

PARTY_MENU_SCRIPT = (
    'pressuntil:a/qa.mdsel.choice/party;wait:120;'
    'pressuntil:a/qa.party.stage/intro;wait:120;stick:down;wait:30;'
    'pressuntil:a/qa.party.stage/board;wait:240;stick:right;wait:120;stick:left;wait:120;'
    'pressuntil:a/qa.party.stage/rules;wait:240;press:b;waitev:qa.party.stage/board;wait:180;'
    'pressuntil:a/qa.party.stage/rules;wait:180;'
    'stick:right;wait:90;stick:right;wait:90;stick:right;wait:90;'
    'pressuntil:a/qa.party.stage/players;wait:240;stick:right;wait:120;'
    'pressuntil:a/qa.party.stage/characters;wait:240;'
    'pressuntil:a/qa.party.stage/handicap;wait:240;'
    'pressuntil:a/qa.party.stage/confirm;wait:240;'
    'pressuntil:a/ovl.start/w01dll;wait:100000;')


def capture_ticks(frames):
    """Read deterministic capture ticks; wall-clock columns are ignored."""
    index = Path(frames)/'index.csv'
    if not index.is_file():
        return []
    ticks = []
    with index.open(newline='', encoding='utf-8') as stream:
        for row in csv.DictReader(stream):
            try:
                ticks.append(int(row['tick']))
            except (KeyError, TypeError, ValueError):
                raise ValueError('frame index has no valid deterministic tick')
    return ticks


def capture_timeline(frames):
    """Read frame-dump metadata, including FI replay flags."""
    index = Path(frames)/'index.csv'
    if not index.is_file():
        return []
    with index.open(newline='', encoding='utf-8') as stream:
        rows = list(csv.DictReader(stream))
    for row in rows:
        try:
            int(row['tick'])
            int(row['replay'])
        except (KeyError, TypeError, ValueError):
            raise ValueError('frame index has no valid tick/replay metadata')
    return rows


def deterministic_capture_errors(ticks, expected, stride):
    if len(ticks) != expected:
        return ['deterministic capture index has %d rows, expected %d' %
                (len(ticks), expected)]
    if any(right - left != stride for left, right in zip(ticks, ticks[1:])):
        return ['capture ticks are not uniformly spaced by stride=%d: %r' %
                (stride, ticks)]
    return []


def fi_capture_errors(ticks, replay_flags, expected, require_replay=False):
    """Validate FI captures without requiring fixed tick spacing."""
    errors = []
    if len(ticks) != expected:
        errors.append('FI capture index has %d rows, expected %d' %
                      (len(ticks), expected))
    if any(right < left for left, right in zip(ticks, ticks[1:])):
        errors.append('FI capture ticks are not nondecreasing: %r' % ticks)
    if len(replay_flags) != len(ticks):
        errors.append('FI replay metadata count does not match capture rows')
    replay_count = sum(replay_flags)
    if require_replay and replay_count <= 0:
        errors.append('No interpolated frames captured; reduce GPU-side crop/readback cost')
    return errors, replay_count


def parse_capture_rect(value):
    if not value:
        return None
    try:
        values = tuple(int(part) for part in value.split(','))
        if len(values) != 4:
            raise ValueError()
        x, y, width, height = values
        if min(x, y) < 0 or min(width, height) <= 0:
            raise ValueError()
    except (TypeError, ValueError):
        raise ValueError('capture-rect must be nonnegative x,y and positive w,h')
    return values


def validate_capture_trigger(value):
    if not value:
        return ''
    if not re.fullmatch(r'[A-Za-z0-9_.-]+=[^;\\s]+', value):
        raise ValueError('capture-trigger must be an exact event key=value')
    return value


def ao_trace_errors(log, require_layers=False):
    rows = []
    pattern = re.compile(
        r'\[AO-QA-TRACE\] tick=(?P<tick>-?\d+) camera=(?P<camera>-?\d+) '
        r'fov=(?P<fov>-?\d+(?:\.\d+)?) vp=(?P<vp>[^ ]+) '
        r'reason=(?P<reason>[^ ]+) count=(?P<count>\d+) epoch=(?P<epoch>\d+)')
    for line in log.splitlines():
        match = pattern.search(line)
        if match:
            rows.append(match.groupdict())
    errors = []
    applied = {}
    for row in rows:
        key = (int(row['camera']), int(row['epoch']))
        if row['reason'] == 'apply':
            applied[key] = applied.get(key, 0) + 1
    duplicates = {key: count for key, count in applied.items() if count != 1}
    if duplicates:
        errors.append('AO apply was not once per admitted epoch: '+repr(duplicates))
    if require_layers:
        for layer in (2, 7):
            marker = '[EVENT] qa.ao.layer.clear=depth-clear num=%d ' % layer
            if marker not in log:
                errors.append('AO trace missing native clear layer %d' % layer)
    return rows, errors


def ao_execution_evidence(log, level, implementation, require_work=False):
    matches = re.findall(r'\[MP6-AO\] encoded=(\d+) composited=(\d+) foliage=(\d+) xegtao=(\d+)', log)
    if not matches:
        return {}, ['Missing renderer AO execution counters']
    counts = dict(zip(('encoded', 'composited', 'foliage', 'xegtao'), map(int, matches[-1])))
    errors = []
    if not level and (counts['encoded'] or counts['composited'] or counts['xegtao']):
        errors.append('AO-off run submitted AO work')
    if level and require_work and (not counts['encoded'] or not counts['composited']):
        errors.append('AO was requested for the 3D route but no AO pass completed')
    if level and require_work and implementation == 'xegtao' and not counts['xegtao']:
        errors.append('XeGTAO requested but no upstream dispatch executed')
    if implementation == 'legacy' and counts['xegtao']:
        errors.append('Legacy run unexpectedly executed XeGTAO')
    return counts, errors


def verified_executable(value, provenance=None):
    """Verify a requested binary against ready/provenance attestation."""
    exe = Path(value)
    if not exe.is_absolute():
        exe = ROOT/exe
    exe = exe.resolve()
    if not exe.is_file():
        raise ValueError('executable not found: '+str(exe))
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    default = DEFAULT_EXE.resolve()
    provenance_path = None
    provenance_data = None
    if provenance:
        provenance_path = Path(provenance)
        if not provenance_path.is_absolute():
            provenance_path = ROOT/provenance_path
        provenance_path = provenance_path.resolve()
        if not provenance_path.is_file():
            raise ValueError('provenance not found: '+str(provenance_path))
        raw = provenance_path.read_text(encoding='utf-8').strip()
        try:
            provenance_data = json.loads(raw)
        except json.JSONDecodeError:
            provenance_data = {'exe': raw}
        expected = provenance_data.get('exe') if isinstance(provenance_data, dict) else None
        if not isinstance(expected, str) or expected.lower() != digest.lower():
            raise ValueError('executable hash does not match provenance: '+str(provenance_path))
    elif exe != default:
        raise ValueError('--exe outside the current QA output requires --provenance')
    if exe == default and provenance_path is None:
        ready_path = exe.parents[1]/'ready.json'
        if not ready_path.is_file():
            raise ValueError('QA executable is missing ready.json')
        ready = json.loads(ready_path.read_text(encoding='utf-8'))
        expected = dict(exe=digest,
                        builder=hashlib.sha256(
                            (Path(__file__).with_name('build_widescreen_qa.py')).read_bytes()
                        ).hexdigest())
        if (not isinstance(ready, dict) or
                not isinstance(ready.get('exe'), str) or
                not isinstance(ready.get('builder'), str) or
                ready['exe'].lower() != expected['exe'] or
                ready['builder'].lower() != expected['builder']):
            raise ValueError('QA executable is stale; rebuild')
        provenance_path = ready_path
        provenance_data = ready
    return dict(path=exe, sha256=digest,
                provenance=str(provenance_path) if provenance_path else None,
                provenance_sha256=hashlib.sha256(
                    provenance_path.read_bytes()).hexdigest() if provenance_path else None,
                provenance_data=provenance_data)


def exited_to_scene(log, scene, target):
    """Boards call their results overlay on system exit instead of returning."""
    entered = requested = killed = False
    for line in log.splitlines():
        if 'minigame.unavailable' in line or 'dll.bound=minigame-stub' in line:
            return False
        if f'[EVENT] ovl.start={scene} ' in line:
            entered, requested, killed = True, False, False
        elif entered and '[EVENT] qa.scene.exit=requested ' in line:
            requested = True
        elif requested and '[EVENT] ovl.kill=' in line:
            killed = True
        elif entered and '[EVENT] ovl.start=' in line:
            return requested and killed and f'ovl.start={target} ' in line
    return False


def returned_from_scene(log, scene):
    """Require an ordered real scene entry, return request and next overlay.

    A bounded exit, a pre-launch return, or an unavailable stub is not proof
    that the minigame reached its native ending.
    """
    entered = returning = False
    for line in log.splitlines():
        if any(marker in line for marker in ('minigame.unavailable', 'dll.bound=minigame-stub')):
            return False
        start = re.match(r'\[EVENT\] ovl.start=([^ ]+) ', line)
        if start:
            if start[1] == scene:
                entered, returning = True, False
            elif entered and returning:
                return True
        if entered and line.startswith('[EVENT] ovl.return='):
            returning = True
    return False


def capture_budget(delay, stride, count, census=False, startup_slack=0):
    # Match frame_dump.c; an out-of-range stride silently falls back to 1 in
    # the engine, which would make a long-playthrough capture misleading.
    if delay < 0 or not 1 <= stride <= 1000 or count < 1 or startup_slack < 0:
        raise ValueError('Use delay >= 0, stride 1..1000, count >= 1, and startup_slack >= 0')
    budget = startup_slack + delay + (count - 1) * stride + 60
    if census:
        budget = max(budget, 900)
    if budget > 100000:
        raise ValueError('Capture schedule must fit within 100000 scene ticks')
    return budget


def resize_errors(log):
    observed = [tuple(map(int, match)) for match in re.findall(
        r'\[WIDE-RESIZE\] settled step=(\d+) tick=\d+ window=(\d+)x(\d+) '
        r'wide=(\d+) logical=(\d+) hidden=(\d+)', log)]
    widths = (640,848,1120,1120,1120,848,640,640,848)
    enabled = (0,1,1,0,1,1,1,0,1)
    expected = [(i,width,480,wide,width if wide else 640,1)
                for i,(width,wide) in enumerate(zip(widths,enabled))]
    return [] if observed == expected else ['Live resize/toggle states did not settle: '+repr(observed)]


def verified_card_seed(path):
    """Only copy attested test cards; never open the player's live saves."""
    seed = (ROOT/path).resolve(strict=True)
    if not seed.is_relative_to((ROOT/'build/filesel-qa-runs').resolve()):
        raise ValueError('card seed must be an isolated filesel QA run')
    result = json.loads((seed/'result.json').read_text())
    hashes = card_hashes(seed/'saves')
    if not result.get('passed') or not hashes or result.get('cards_after') != hashes:
        raise ValueError('card seed must have passed with unchanged saved-card hashes')
    return seed/'saves'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenes', nargs='+', required=True, help='Native overlay names, or minigames')
    parser.add_argument('--sizes', nargs='+', default=['640x480','848x480','1120x480'])
    parser.add_argument('--delay', type=int, default=300)
    parser.add_argument('--stride', type=int, default=450)
    parser.add_argument('--count', type=int, default=8)
    parser.add_argument('--startup-slack', type=int, default=600,
                        help='Deterministic scene-entry/render slack before the first capture')
    parser.add_argument('--geometry', action='store_true')
    parser.add_argument('--export-scene', action='store_true',
                        help='QA-only: export the live loaded HSF scene to scene.json')
    parser.add_argument('--night', action='store_true', help='Select the native night minigame variant')
    parser.add_argument('--mesh', help='Dump faces of comma-separated exact mesh names (QA only)')
    parser.add_argument('--hide-mesh', help='Render isolation only: hide exact named meshes after tick 500')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--reenter', action='store_true', help='Force mdsel to reload itself for lifetime QA')
    parser.add_argument('--all-cpu', action='store_true', help="Use the minigame's native AI for all players")
    parser.add_argument('--require-return', action='store_true', help='Also require the native overlay return and a subsequent scene entry')
    parser.add_argument('--request-exit', action='store_true',
                        help='QA-only system exit after captures; requires return, not round completion')
    parser.add_argument('--exit-target', help='Require a requested exit to this overlay instead of a return (boards)')
    parser.add_argument('--cpu-difficulty', type=int, choices=range(4), default=0)
    parser.add_argument('--cpu-difficulties', type=int, choices=range(4), nargs=4,
                        help='Override native CPU difficulty separately for P1..P4')
    parser.add_argument('--winner-split', action='store_true', help='M618: capture from the real winner split-view expansion')
    parser.add_argument('--env', action='append', default=[], metavar='MP6_NAME=VALUE',
                        help='Extra game environment, e.g. MP6_ENH_SHADOW_QUALITY=4 (repeatable)')
    parser.add_argument('--no-offstage', action='store_true',
                        help='Disable the widescreen off-stage actor offset (before/after QA)')
    parser.add_argument('--winner-camera', action='store_true', help='M640/M650/M659: capture the real winner camera transition (M650: record scene)')
    parser.add_argument('--party-menu', action='store_true', help='Navigate mdsel -> native Party setup, including backtracking, then enter Treetop')
    parser.add_argument('--resize-live', action='store_true', help='QA-only hidden-window resize/toggle cycle, starting at native640')
    parser.add_argument('--card-seed', type=Path, help='Copy an attested filesel QA card into each isolated run')
    parser.add_argument('--solo-results', action='store_true',
                        help='Enter the real Solo completed-board results path with an attested QA card')
    parser.add_argument('--original-gradient', action='store_true',
                        help='M659-only negative control: render the original short sky gradient')
    parser.add_argument('--bus-motion', action='store_true',
                        help='QA-only actual signpost vertex/phase measurements')
    parser.add_argument('--evaluated-shapes', action='store_true',
                        help='Export evaluated unskinned shape vertices for a single-pose Blender study')
    parser.add_argument('--bus-old-timer', action='store_true',
                        help='QA-only negative control: restart bus props on the old clock')
    parser.add_argument('--exe', type=Path, default=DEFAULT_EXE,
                        help='QA executable; non-default binaries require --provenance')
    parser.add_argument('--provenance', type=Path,
                        help='JSON/text attestation containing the executable SHA-256')
    parser.add_argument('--ao', type=int, choices=(0, 1, 2), default=0,
                        help='AO level: 0 off, 1 subtle, 2 strong')
    parser.add_argument('--implementation', choices=('legacy', 'xegtao'), default='legacy',
                        help='AO implementation used for the run')
    parser.add_argument('--ao-trace', action='store_true',
                        help='Enable QA-only AO epoch/layer trace output')
    parser.add_argument('--gpu-validation', action='store_true',
                        help='Enable optional graphics validation in the isolated process')
    parser.add_argument('--unlocked', action='store_true',
                        help='Run the bounded scene with 60 Hz unlocked-FPS interpolation')
    parser.add_argument('--capture-trigger', default='',
                        help='Exact observed event key=value to arm frame capture')
    parser.add_argument('--capture-rect', default='',
                        help='GPU-side x,y,w,h crop in source EFB pixels')
    parser.add_argument('--require-replay', action='store_true',
                        help='Require at least one replay=1 frame in an unlocked run')
    parser.add_argument('--input', default='period:30;timeout:1000000;pressuntil:a/qa.never/done;')
    args = parser.parse_args()
    if args.exit_target and not args.request_exit:
        parser.error('--exit-target requires --request-exit')
    if args.request_exit:
        args.require_return = args.require_return or not args.exit_target
        if args.startup_slack < 200:
            parser.error('--request-exit requires at least 200 startup-slack ticks')
    try:
        card_seed = verified_card_seed(args.card_seed) if args.card_seed else None
    except (ValueError, OSError) as error:
        parser.error(str(error))
    try:
        budget = capture_budget(args.delay, args.stride, args.count,
                                args.geometry or args.mesh or args.hide_mesh or args.export_scene,
                                args.startup_slack)
    except ValueError as error:
        parser.error(str(error))
    try:
        capture_rect = parse_capture_rect(args.capture_rect)
        capture_trigger = validate_capture_trigger(args.capture_trigger)
    except ValueError as error:
        parser.error(str(error))
    if args.require_replay and not args.unlocked:
        parser.error('--require-replay needs --unlocked')
    if args.resize_live and (args.sizes != ['640x480'] or budget < 1700):
        parser.error('--resize-live requires --sizes 640x480 and at least1700 scene ticks')
    owners = {r['owner'].split('/')[-1].lower() for r in audit_widescreen.inventory() if r['owner'].startswith('REL/')}
    scenes = sorted(o for o in owners if o.startswith('m6')) if args.scenes == ['minigames'] else args.scenes
    for scene in scenes:
        if scene.lower() not in owners: parser.error('Not a compiled native scene: '+scene)
        if args.all_cpu and not scene.lower().startswith('m6'): parser.error('--all-cpu requires a minigame')
        if args.winner_split and scene.lower() != 'm618dll': parser.error('--winner-split supports M618 only')
        if args.winner_camera and scene.lower() not in {'m640dll', 'm650dll', 'm659dll'}:
            parser.error('--winner-camera supports M640/M650/M659 only')
        if args.party_menu and scene.lower() != 'mdpartydll': parser.error('--party-menu supports mdpartydll only')
    if args.solo_results and (scenes != ['mdsingdll'] or not card_seed):
        parser.error('--solo-results requires --scenes mdsingdll and an attested --card-seed')
    if args.original_gradient and scenes != ['m659dll']:
        parser.error('--original-gradient supports m659dll only')
    if args.cpu_difficulties and not args.all_cpu:
        parser.error('--cpu-difficulties requires --all-cpu')
    try:
        executable = verified_executable(args.exe, args.provenance)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    exe = executable['path']
    output = ROOT/'build/widescreen-qa/runs'/datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    output.mkdir(parents=True)
    print('Evidence:', output, flush=True)
    results = []
    for scene in scenes:
        scene = scene.lower()
        for size in args.sizes:
            directory = output/(scene+'-'+size)
            directory.mkdir()
            if card_seed:
                shutil.copytree(card_seed, directory/'saves')
            wide = int(size.split('x')[0])/int(size.split('x')[1]) > 1.34
            env = {k:v for k,v in os.environ.items() if not k.startswith('MP6_')}
            default_trigger = ('qa.mdsing.result-ready=ready' if args.solo_results
                               else 'qa.party.stage=intro' if args.party_menu
                               else 'ovl.start='+scene)
            frame_trigger = capture_trigger or default_trigger
            env.update(MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel', MP6_TEST_LOAD_DLL=scene,
                MP6_TICK_HZ='60' if args.unlocked else '0',
                MP6_VSYNC='0', MP6_UNLOCKED_FPS=str(int(args.unlocked)),
                MP6_ENH_HEAP_SCALE='4',
                MP6_DISC_ROOT=str(ROOT/'build/disc-cache/orig/GP6E01'),
                MP6_ENH_WIDESCREEN=str(int(wide)), MP6_WIDESCREEN=str(int(wide)), MP6_FREE_ASPECT='1',
                MP6_WINDOW_SIZE=size, MP6_ENH_AMBIENT_OCCLUSION=str(args.ao),
                MP6_AO_XEGTAO=str(int(args.implementation == 'xegtao')),
                MP6_AO_DIAG='1',
                MP6_AO_IMPLEMENTATION=args.implementation, MP6_ENH_AA='0',
                MP6_WS_QA_SCENE=scene, MP6_WS_QA_BUDGET=str(budget),
                SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy',
                MP6_FRAME_DUMP=str(directory/'frames'), MP6_FRAME_DUMP_COUNT=str(args.count),
                MP6_FRAME_DUMP_STRIDE=str(args.stride), MP6_FRAME_DUMP_DELAY=str(args.delay),
                MP6_FRAME_DUMP_TRIGGER=frame_trigger)
            if args.solo_results:
                env['MP6_WS_QA_SOLO_RESULTS'] = '1'
            if args.original_gradient:
                env['MP6_WS_QA_ORIGINAL_GRADIENT'] = '1'
            if args.bus_motion:
                env['MP6_WS_QA_BUS'] = '1'
            if args.bus_old_timer:
                env['MP6_WS_QA_BUS_OLD_TIMER'] = '1'
            if args.export_scene:
                env['MP6_WS_QA_EXPORT_SCENE'] = str(directory/'scene.json')
            if args.evaluated_shapes:
                env['MP6_WS_QA_EVALUATED_SHAPES'] = '1'
                if scene == 'm675dll':
                    env['MP6_WS_QA_WATER_BITMAP'] = str(directory/'hama_umi.bin')
            if capture_rect:
                env['MP6_FRAME_DUMP_RECT'] = ','.join(map(str, capture_rect))
            if args.request_exit:
                env['MP6_WS_QA_REQUEST_EXIT'] = '1'
            if args.party_menu:
                # Allow the game to enter mdsel naturally.  Directly loading
                # mdpartydll would bypass the transition under test.
                env.pop('MP6_TEST_LOAD_DLL')
            elif scene == 'mdseldll' and not args.reenter:
                env.pop('MP6_TEST_LOAD_DLL')
            elif scene == 'bootdll':
                env.pop('MP6_BOOT_TO')
                env.pop('MP6_TEST_LOAD_DLL')
            script = 'period:20;timeout:1000000;pressuntil:start/ovl.start/'+scene+';'+args.input
            if args.solo_results and args.input == parser.get_default('input'):
                # Stop confirming once the requested screen is actually visible.
                stage = 'bank-ready' if frame_trigger == 'qa.mdsing.bank-ready=ready' else 'result-ready'
                script = ('period:30;timeout:1000000;pressuntil:start/ovl.start/'+scene+';'
                          'pressuntil:a/qa.mdsing.'+stage+'/ready;wait:1000000;')
            if args.party_menu:
                script = 'period:30;timeout:1000000;pressuntil:start/ovl.start/mdseldll;' + PARTY_MENU_SCRIPT
            if args.geometry or args.mesh or args.hide_mesh or args.export_scene:
                env['MP6_WS_HSF_DIAG'] = '1'
            if args.hide_mesh:
                env['MP6_WS_QA_HIDE_MESH'] = args.hide_mesh
            if args.night:
                env['MP6_WS_QA_NIGHT'] = '1'
            if args.mesh:
                env['MP6_WS_QA_MESH'] = args.mesh
            if args.all_cpu:
                env['MP6_WS_QA_ALL_CPU'] = '1'
                env['MP6_WS_QA_CPU_DIFFICULTY'] = str(args.cpu_difficulty)
                if args.cpu_difficulties:
                    env['MP6_WS_QA_CPU_LEVELS'] = ''.join(map(str,args.cpu_difficulties))
            if args.resize_live:
                # Do not force the switch through an environment override: the
                # probe publishes the same live preference used by Settings.
                for key in ('MP6_ENH_WIDESCREEN', 'MP6_WIDESCREEN', 'MP6_FREE_ASPECT'):
                    env.pop(key, None)
                env['MP6_WS_QA_RESIZE'] = '1'
            if args.winner_split or args.winner_camera:
                env['MP6_WS_QA_WINNER_CAPTURE'] = str(budget)
                if scene == 'm650dll':
                    env['MP6_WS_QA_M650_RECORD'] = '1'
                if not capture_trigger:
                    frame_trigger = f'qa.{scene[:4].lower()}.winner=ready'
                    env['MP6_FRAME_DUMP_TRIGGER'] = frame_trigger
            if args.no_offstage:
                env['MP6_WS_QA_NO_OFFSTAGE'] = '1'
            for item in args.env:
                key, _, value = item.partition('=')
                env[key] = value
            if args.ao_trace:
                env['MP6_AO_TRACE'] = '1'
            if args.gpu_validation:
                env['MP6_GPU_VALIDATION'] = '1'
            timed_out = False
            isolate_background(env, exe)
            with (directory/'game.log').open('w') as log:
                proc = subprocess.Popen([str(exe),'1000000','--input-script',script],cwd=directory,env=env,
                    stdout=log,stderr=subprocess.STDOUT,creationflags=background_process_flags())
                try: code = proc.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    proc.terminate()
                    code = proc.wait(timeout=10)
            text = (directory/'game.log').read_text(errors='replace')
            captures = list((directory/'frames').glob('*.mfd'))
            errors = [line for line in text.splitlines() if any(s in line for s in (
                '[FATAL]','[MP6-CRASH]','[CRASH]','script.timeout=','Validation Error','[AURORA ERROR'))
                or ('[FRAMEDUMP]' in line and 'is invalid' in line)]
            bus_evidence = None
            if args.bus_motion:
                from tests.integration.analyze_bus_motion import analyze
                bus_evidence = analyze(text)
                if not args.bus_old_timer:
                    errors.extend(bus_evidence['errors'])
            if args.export_scene:
                from tools.blender_scene import validate_snapshot
                try:
                    validate_snapshot(json.loads((directory/'scene.json').read_text(encoding='utf-8')))
                except (OSError, ValueError) as error:
                    errors.append('live scene export is missing or invalid: ' + str(error))
            try:
                timeline = capture_timeline(directory/'frames')
                ticks = [int(row['tick']) for row in timeline]
                replay_flags = [int(row['replay']) for row in timeline]
                if any(flag not in (0, 1) for flag in replay_flags):
                    errors.append('capture replay flags must be 0 or 1')
                replay_count = sum(replay_flags)
                if args.unlocked:
                    fi_errors, replay_count = fi_capture_errors(
                        ticks, replay_flags, args.count, args.require_replay)
                    errors.extend(fi_errors)
                else:
                    errors.extend(deterministic_capture_errors(
                        ticks, args.count, args.stride))
            except (OSError, ValueError) as error:
                ticks = []
                replay_flags = []
                replay_count = 0
                timeline = []
                errors.append('capture index invalid: '+str(error))
            ao_trace = []
            ao_execution, execution_errors = ao_execution_evidence(text, args.ao, args.implementation, args.party_menu)
            errors.extend(execution_errors)
            if args.ao_trace:
                ao_trace, trace_errors = ao_trace_errors(text, require_layers=args.party_menu)
                errors.extend(trace_errors)
            returned = returned_from_scene(text, scene)
            exit_requested = '[EVENT] qa.scene.exit=requested ' in text
            exit_transition = exited_to_scene(text, scene, args.exit_target) if args.exit_target else None
            if args.exit_target and not exit_transition:
                errors.append('Requested exit did not transition to ' + args.exit_target)
            if args.request_exit and not exit_requested:
                errors.append('Requested QA system exit was not observed')
            if args.request_exit and exit_requested:
                exit_tick = re.search(r'\[EVENT\] qa\.scene\.exit=requested .*?tick=(\d+)', text)
                if not exit_tick or any(tick >= int(exit_tick[1]) for tick in ticks):
                    errors.append('Captures overlap the requested exit; increase startup-slack')
            if args.resize_live:
                errors.extend(resize_errors(text))
            if args.require_return and not returned:
                errors.append('Native overlay return and subsequent overlay entry not observed')
            if args.party_menu:
                if '[EVENT] qa.mdsel.choice=party num=0 ' not in text:
                    errors.append('Native mdsel did not choose Party')
                starts = [line for line in text.splitlines() if line.startswith('[EVENT] ovl.start=')]
                mdsel_index = next((i for i, line in enumerate(starts)
                                    if 'ovl.start=mdseldll ' in line), None)
                mdparty_index = next((i for i, line in enumerate(starts)
                                      if 'ovl.start=mdpartydll ' in line), None)
                if mdsel_index is None or mdparty_index is None or mdsel_index >= mdparty_index:
                    errors.append('Party route did not prove ordered mdsel -> mdparty transition')
                for stage in ('intro', 'board', 'rules', 'players', 'characters', 'handicap', 'confirm', 'launch'):
                    if '[EVENT] qa.party.stage='+stage not in text:
                        errors.append('Missing Party menu stage: ' + stage)
                if text.count('[EVENT] qa.party.stage=board') < 2:
                    errors.append('Party rules-to-board backtracking not observed')
                if '[EVENT] ovl.start=w01dll ' not in text:
                    errors.append('Party setup did not enter the selected board')
            winner_event = f'qa.{scene[:4].lower()}.winner=ready'
            if (args.winner_split or args.winner_camera) and '[EVENT] '+winner_event not in text:
                errors.append('Requested real winner event not observed')
            result = dict(scene=scene,size=size,exit_code=code,timed_out=timed_out,captures=len(captures),
                capture_ticks=ticks, capture_stride=args.stride,
                deterministic_ticks=not args.unlocked,
                startup_slack=args.startup_slack,
                unlocked=args.unlocked, tick_hz=60 if args.unlocked else 0,
                capture_trigger=frame_trigger, capture_rect=(
                    ','.join(map(str, capture_rect)) if capture_rect else ''),
                require_replay=args.require_replay,
                replay_count=replay_count, replay_flags=replay_flags,
                ao=args.ao, implementation=args.implementation,
                ao_execution=ao_execution,
                ao_trace_rows=len(ao_trace),
                ao_trace_apply=sum(row['reason'] == 'apply' for row in ao_trace),
                executable=executable['sha256'], provenance=executable['provenance'],
                provenance_sha256=executable['provenance_sha256'],
                hidden_meshes=args.hide_mesh,
                export_scene=args.export_scene,
                exported_scene=(str(directory/'scene.json') if (args.export_scene and
                                (directory/'scene.json').is_file()) else None),
                party_menu=args.party_menu,
                solo_results=args.solo_results,
                original_gradient=args.original_gradient,
                bus_motion=args.bus_motion, bus_old_timer=args.bus_old_timer,
                evaluated_shapes=args.evaluated_shapes,
                bus_motion_evidence=bus_evidence,
                card_seed=str(args.card_seed) if args.card_seed else None,
                input_script=script,
                resize_live=args.resize_live,
                all_cpu=args.all_cpu, cpu_difficulty=args.cpu_difficulty,
                cpu_difficulties=args.cpu_difficulties, returned=returned,
                system_exit_requested=exit_requested,
                exit_target=args.exit_target, exit_transition=exit_transition,
                errors=errors,passed=code==0 and not timed_out and len(captures)==args.count and not errors)
            (directory/'result.json').write_text(json.dumps(result,indent=2))
            results.append(result)
            (output/'results.json').write_text(json.dumps(results,indent=2))
            print(json.dumps(result),flush=True)
    return 0 if all(r['passed'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
