"""Isolated, bounded native board QA; never opens the user's save directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--exe', default='build/board-qa/release/mp6native.exe')
parser.add_argument('--name', default='baseline')
parser.add_argument('--seconds', type=int, default=240)
parser.add_argument('--capsule', type=int, default=-1)
parser.add_argument('--input-script', default='period:30;timeout:149000;pressuntil:a/qa.complete/done;wait:60')
parser.add_argument('--gpu-timings', action='store_true', help='Log real asynchronous GPU pass timestamps')
parser.add_argument('--route', default='')
parser.add_argument('--window-size', default='1024x768')
parser.add_argument('--widescreen', action='store_true')
parser.add_argument('--results-geometry', action='store_true', help='QA-only results theater mesh census')
parser.add_argument('--ao', type=int, choices=(0, 1, 2), default=0)
parser.add_argument('--mobile-ao', action='store_true', help='QA-only Android shading budget on the desktop renderer')
parser.add_argument('--ao-cycle', type=int, help='QA-only live AO sweep starting at this tick')
parser.add_argument('--unshaded-ao', action='store_true', help='QA-only zero AO strength with identical geometry and render passes')
parser.add_argument('--raw-ao-depth', action='store_true', help='QA-only pre-optimization depth conversion with identical scene and passes')
parser.add_argument('--uncached-settings', action='store_true', help='QA-only original repeated environment lookups')
parser.add_argument('--frame-upload-mode', type=int, choices=(0, 1, 2),
                    help='Private upload scheduling diagnostic: control, batched, or ABBA')
parser.add_argument('--foreground-ao', action='store_true', help='QA-only display of AO-protected premultiplied foliage')
parser.add_argument('--water-ao', action='store_true', help='QA-only display of the exact water receiver mask')
parser.add_argument('--legacy-foliage-viewport', action='store_true', help='QA-only reproduction of unscaled private foliage viewport')
parser.add_argument('--foliage-no-depth', action='store_true', help='QA-only foliage alignment diagnostic without depth rejection')
parser.add_argument('--geometry-at', type=int, help='QA-only world-space mesh capture tick')
parser.add_argument('--no-grounding', action='store_true', help='QA-only AO comparison without visual grounding')
parser.add_argument('--no-lawn-lift', action='store_true', help='QA-only comparison against the original lawn height')
parser.add_argument('--no-connector-lift', action='store_true', help='QA-only reproduction of buried white path connectors')
parser.add_argument('--no-log-grounding', action='store_true', help='QA-only comparison without log base extensions')
parser.add_argument('--no-tree-grounding', action='store_true', help='QA-only comparison without tree base extensions')
parser.add_argument('--no-foliage-mask', action='store_true', help='QA-only comparison without AO foliage coverage')
parser.add_argument('--legacy-foliage-blend', action='store_true', help='QA-only same-state reproduction of the old transmission blend')
parser.add_argument('--aa', type=int, choices=range(5), default=0)
parser.add_argument('--shadow-quality', type=int, choices=(1, 2, 4, 8, 16), default=1)
parser.add_argument('--unlocked-fps', action='store_true')
parser.add_argument('--capture-delay', type=int, default=0)
parser.add_argument('--cache-seed', help='copy an existing build-area GPU cache for a recovery/warm-start test')
parser.add_argument('--audio-trace', action='store_true')
parser.add_argument('--capture-frames', type=int, default=0)
parser.add_argument('--capture-stride', type=int, default=4)
parser.add_argument('--capture-trigger', default='capsule.begin',
                    help='substring of a game log line that starts the capture')
parser.add_argument('--one-turn', action='store_true')
parser.add_argument('--tick-hz', type=int, default=0)
parser.add_argument('--hold-bad-camera', action='store_true')
parser.add_argument('--hold-event-end', action='store_true')
parser.add_argument('--save-at', type=int)
parser.add_argument('--load-at', type=int)
parser.add_argument('--state-path')
parser.add_argument('--ticks', type=int, default=150000)
parser.add_argument('--stop-round', type=int, default=7,
                    help='QA heartbeat stop round; 0 disables the round stop')
args = parser.parse_args()
if not re.fullmatch(r'[1-9][0-9]{2,3}x[1-9][0-9]{2,3}', args.window_size):
    parser.error('--window-size must be WIDTHxHEIGHT, each 100..9999')
if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
    parser.error('--name must be a single simple directory name')
if args.seconds <= 0 or args.ticks <= 0 or not 0 <= args.tick_hz <= 1000 or args.stop_round < 0:
    parser.error('positive seconds/ticks, nonnegative stop-round, and tick-hz in 0..1000 required')
if not 0 <= args.capture_frames <= 1000 or not 1 <= args.capture_stride <= 1000:
    parser.error('capture-frames must be 0..1000 and capture-stride 1..1000')
directory = root / 'build/board-qa-runs' / args.name
directory.mkdir(parents=True, exist_ok=False)
cache_directory = directory / 'gpu-cache'
cache_directory.mkdir()
if args.cache_seed:
    seed = (root / args.cache_seed).resolve(strict=True)
    if not seed.is_dir() or not seed.is_relative_to((root / 'build').resolve()):
        parser.error('--cache-seed must be an existing directory inside build')
    for generation in ('', 'gpu-cache-v2', 'gpu-cache-v3'):
        if not (seed / generation).is_dir():
            continue
        target = cache_directory / generation
        target.mkdir(exist_ok=True)
        for name in ('pipeline_cache.db', 'pipeline_cache.db-wal', 'pipeline_cache.db-shm',
                     'dawn_cache.db', 'dawn_cache.db-wal', 'dawn_cache.db-shm'):
            source = seed / generation / name
            if source.is_file():
                shutil.copy2(source, target / name)
executable = (root / args.exe).resolve(strict=True)
with executable.open('rb') as binary:
    binary_hash = hashlib.file_digest(binary, 'sha256').hexdigest()
if executable == (root / 'build/board-qa/release/mp6native.exe').resolve():
    ready_path = root / 'build/board-qa/build-ready.json'
    if not ready_path.exists():
        parser.error('QA build did not finish successfully; rebuild with build_board_qa.py')
    ready = json.loads(ready_path.read_text())
    builder_hash = hashlib.sha256((root / 'tests/integration/build_board_qa.py').read_bytes()).hexdigest()
    fixture_hash=hashlib.sha256((root/'tests/integration/capsule_audit_fixture.c').read_bytes()).hexdigest()
    if (ready['sha256'] != binary_hash or ready['builder_sha256'] != builder_hash or
            ready.get('capsule_fixture_sha256') != fixture_hash):
        parser.error('QA executable or fixtures are stale; rebuild with build_board_qa.py')
env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
if args.frame_upload_mode is not None:
    env['MP6_UPLOAD_MODE'] = str(args.frame_upload_mode)
if args.gpu_timings:
    env['MP6_GPU_TIMINGS'] = '1'
env.update(MP6_LAUNCHER='0', MP6_BOOT_TO='mdparty', MP6_AUTO_START_TICKS='60,150',
           MP6_TICK_HZ=str(args.tick_hz), MP6_WINDOW_SIZE=args.window_size,
           MP6_DISC_ROOT=str(root / 'build/disc-cache/orig/GP6E01'),
           MP6_QA_CAPSULE=str(args.capsule), MP6_QA_ROUTE=args.route,
           MP6_GPU_CACHE_PATH=str(directory / 'gpu-cache'),
           MP6_QA_STOP_ROUND=str(args.stop_round))
env.update(MP6_ENH_AMBIENT_OCCLUSION=str(args.ao), MP6_ENH_AA=str(args.aa),
           MP6_ENH_SHADOW_QUALITY=str(args.shadow_quality),
           MP6_UNLOCKED_FPS='1' if args.unlocked_fps else '0', MP6_VSYNC='0',
           MP6_TICK_RATE_LOG='1', MP6_PRESENT_RATE_LOG='1', MP6_AO_DIAG='1')
if args.ao_cycle is not None:
    env.pop('MP6_ENH_AMBIENT_OCCLUSION')
    env['MP6_QA_AO_CYCLE'] = str(args.ao_cycle)
if args.mobile_ao:
    env['MP6_QA_MOBILE_AO'] = '1'
if args.unshaded_ao:
    env['MP6_QA_UNSHADED_AO'] = '1'
if args.raw_ao_depth:
    env['MP6_QA_RAW_AO_DEPTH'] = '1'
if args.uncached_settings:
    env['MP6_QA_UNCACHED_SETTINGS'] = '1'
if args.foreground_ao:
    env['MP6_QA_FOREGROUND_AO'] = '1'
if args.water_ao:
    env['MP6_QA_WATER_AO'] = '1'
if args.legacy_foliage_viewport:
    env['MP6_QA_LEGACY_FOLIAGE_VIEWPORT'] = '1'
if args.foliage_no_depth:
    env['MP6_QA_FOLIAGE_NO_DEPTH'] = '1'
if args.geometry_at is not None:
    env['MP6_QA_GEOMETRY_AT'] = str(args.geometry_at)
if args.no_grounding:
    env['MP6_QA_NO_GROUNDING'] = '1'
if args.no_lawn_lift:
    env['MP6_QA_NO_LAWN_LIFT'] = '1'
if args.no_connector_lift:
    env['MP6_QA_NO_CONNECTOR_LIFT'] = '1'
if args.no_log_grounding:
    env['MP6_QA_NO_LOG_GROUNDING'] = '1'
if args.no_tree_grounding:
    env['MP6_QA_NO_TREE_GROUNDING'] = '1'
if args.no_foliage_mask:
    env['MP6_QA_NO_FOLIAGE_MASK'] = '1'
if args.legacy_foliage_blend:
    env['MP6_QA_LEGACY_FOLIAGE_BLEND'] = '1'
if args.widescreen:
    # Exercise the same resolved preference used by interactive play, not only
    # the legacy getenv fallback (which distorts engine hot-path measurements).
    env.update(MP6_ENH_WIDESCREEN='1', MP6_WIDESCREEN='1', MP6_FREE_ASPECT='1')
if args.results_geometry:
    env['MP6_QA_RESULTS_GEOMETRY'] = '1'
    env['MP6_WS_HSF_DIAG'] = '1'
if args.one_turn:
    env['MP6_QA_ONE_TURN'] = '1'
if args.audio_trace:
    env['MP6_AUDIO_TIMELINE'] = '1'
    env['MP6_AUDIO_SE_MACRO_DUMP'] = '1095,1096,1097,1122'
if args.capture_frames:
    env.update(MP6_FRAME_DUMP=str(directory / 'frames'),
               MP6_FRAME_DUMP_COUNT=str(args.capture_frames),
               MP6_FRAME_DUMP_STRIDE=str(args.capture_stride),
               MP6_FRAME_DUMP_DELAY=str(args.capture_delay),
               MP6_FRAME_DUMP_TRIGGER='' if args.capture_trigger == '-' else args.capture_trigger)
if args.hold_bad_camera:
    env['MP6_QA_HOLD_BAD_CAMERA'] = '1'
if args.hold_event_end:
    env['MP6_QA_HOLD_EVENT_END'] = '1'
if args.save_at is not None or args.load_at is not None:
    if not args.state_path:
        parser.error('--state-path is required for a quick-state test')
    state_path = (root / args.state_path).resolve()
    # Test states never overwrite user states or leave the ignored test area.
    if not state_path.is_relative_to((root / 'build/board-qa-runs').resolve()):
        parser.error('test state must be inside build/board-qa-runs')
    if args.save_at is not None and state_path.exists():
        parser.error('refusing to overwrite an existing test state')
    if args.load_at is not None and not state_path.is_file():
        parser.error('load test state does not exist')
    env['MP6_SAVESTATE_PATH'] = str(state_path)
    if args.save_at is not None: env['MP6_SAVESTATE_SAVE_AT_TICK'] = str(args.save_at)
    if args.load_at is not None: env['MP6_SAVESTATE_LOAD_AT_TICK'] = str(args.load_at)
with (directory / 'game.log').open('w', encoding='utf-8') as output:
    process = subprocess.Popen([str(executable), str(args.ticks), '--input-script',
                                args.input_script],
                               cwd=directory, env=env, stdout=output, stderr=subprocess.STDOUT)
    print(f'Board QA {args.name}: PID={process.pid}; log={directory / "game.log"}', flush=True)
    start = time.monotonic()
    try:
        result = process.wait(timeout=args.seconds)
        reason = 'exited'
    except subprocess.TimeoutExpired:
        process.terminate()
        result = process.wait(timeout=10)
        reason = 'wall-clock limit'
    print(f'Board QA {args.name}: {reason}, exit={result}, elapsed={time.monotonic()-start:.1f}s', flush=True)
log = (directory / 'game.log').read_text(errors='replace')
report = dict(request=vars(args), executable=str(executable), sha256=binary_hash,
              exit_code=result, reason=reason, elapsed_seconds=time.monotonic()-start,
              completion_marker='[EVENT] qa.complete=done' in log,
              turn_end_count=log.count('[BOARD-QA] turn.end'),
              crash_marker=any(marker in log for marker in
                               ('[MP6-CRASH]', '[FATAL]', '[AURORA FATAL:')))
(directory / 'result.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print('Diagnostic only: inspect effect/state and camera output; exit 0 is not a gameplay pass.', flush=True)
