"""Isolated, bounded native board QA; never opens the user's save directory."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root))
from tests.integration.background_qa import isolate_background, background_process_flags
from tests.integration.ao_resource_probe import parse_resource_log
from tests.integration.ao_resize_probe import resize_ao_error, validate_resize_ao_evidence
from tests.integration.ao_default_probe import default_ao_error, validate_default_ao
from tests.integration.ao_deferred_capture import deferred_capture_error, validate_deferred_capture
from tests.integration.ao_batch_compute_probe import batch_compute_error, validate_batch_compute
from tests.integration.ao_buffer_capture import buffer_capture_error, validate_buffer_capture


def ao_fi_matrix_range_error(range_spec, limit, unlocked_fps, tick_hz):
    """Return an admission error for the opt-in FI matrix capture."""

    if range_spec is None:
        if limit != 256:
            return '--ao-fi-matrix-limit requires --ao-fi-matrix-range'
        return None
    if not unlocked_fps:
        return '--ao-fi-matrix-range requires --unlocked-fps'
    if tick_hz <= 0:
        return '--ao-fi-matrix-range requires paced --tick-hz > 0'
    match = re.fullmatch(r'([0-9]+):([0-9]+)', range_spec)
    if not match:
        return '--ao-fi-matrix-range must be FROM:TO with nonnegative integer ticks'
    start, end = (int(value) for value in match.groups())
    if end < start:
        return '--ao-fi-matrix-range requires TO >= FROM'
    if end - start > 120:
        return '--ao-fi-matrix-range window must be <= 120 ticks'
    if limit <= 0 or limit > 1024:
        return '--ao-fi-matrix-limit must be in 1..1024'
    return None


def parse_ao_fi_matrix_range(range_spec):
    """Parse an already-admitted inclusive FI tick range."""

    if range_spec is None:
        return None
    match = re.fullmatch(r'([0-9]+):([0-9]+)', range_spec)
    if match is None:
        raise ValueError('invalid AO FI matrix range')
    return tuple(int(value) for value in match.groups())


def ao_fi_pixels_error(args):
    """Admit only a bounded, source-bound same-phase diagnostic capture."""
    if args.ao_fi_pixels is None:
        return None
    match = re.fullmatch(r'([0-9]+):([0-9]+):([0-9]+)', args.ao_fi_pixels)
    if not match:
        return '--ao-fi-pixels must be FROM:TO:STRIDE'
    start, end, stride = map(int, match.groups())
    if not (3000 <= start <= end <= 2147483647 and end - start <= 120 and
            1 <= stride <= 120 and end < args.ticks):
        return '--ao-fi-pixels requires a <=120 tick window after tick 3000 and before shutdown'
    if not args.unlocked_fps or args.tick_hz <= 0:
        return '--ao-fi-pixels requires --unlocked-fps and paced --tick-hz'
    if args.implementation not in ('default', 'xegtao') or args.ao != 2 or args.aa != 0:
        return '--ao-fi-pixels requires default or explicit XeGTAO AO 2 with AA Off'
    if args.xegtao_resolution != 'full' or args.xegtao_depth_storage != 'r32float':
        return '--ao-fi-pixels requires full-resolution R32 XeGTAO'
    if args.xegtao_init_failure or args.xegtao_prepared_canonical:
        return '--ao-fi-pixels cannot combine initialization or preparation overrides'
    if not args.leaf_camera or not args.leaf_freeze:
        return '--ao-fi-pixels requires --leaf-camera and --leaf-freeze'
    if args.ao_fi_matrix_range != f'{start}:{end}':
        return '--ao-fi-pixels requires the same --ao-fi-matrix-range'
    if (args.capture_frames != (end-start)//stride+1 or args.capture_stride != 1 or
            args.capture_delay != 0 or args.capture_trigger != '-'):
        return '--ao-fi-pixels requires one capture per selected tick, stride 1, delay 0, trigger -'
    incompatible = ('resize_ao', 'raw_ao_depth', 'foreground_ao', 'water_ao',
                    'legacy_foliage_viewport', 'foliage_no_depth', 'no_grounding',
                    'no_lawn_lift', 'no_connector_lift', 'no_log_grounding',
                    'no_tree_grounding', 'no_foliage_mask', 'legacy_foliage_blend')
    if args.ao_cycle is not None or any(getattr(args, key) for key in incompatible):
        return '--ao-fi-pixels cannot combine geometry, scheduling, or AO-mask overrides'
    return None


def validate_ao_fi_pixel_policy(log, implementation, quality, denoise,
                                exit_code=0, reason='exited'):
    """Bind replay-pixel comparisons to the actual native Full/R32 policy."""
    from tests.integration.ao_fi_pixel_policy import validate_ao_fi_pixel_policy as validate

    return validate(log, implementation, quality, denoise, exit_code, reason)


def capture_rect_error(rect, capture_frames):
    if rect is None:
        return None
    if capture_frames <= 0:
        return '--capture-rect requires --capture-frames'
    x, y, width, height = rect
    if min(x, y) < 0 or min(width, height) <= 0 or x+width > 9999 or y+height > 9999:
        return '--capture-rect requires a positive rectangle within 9999x9999'
    return None


def leaf_camera_mode_error(leaf_camera, mode):
    """Return a parser error when a non-default leaf mode is not enabled."""

    if mode != 'pan' and not leaf_camera:
        return '--leaf-camera-mode requires --leaf-camera unless mode is pan'
    return None


def scene_camera_error(values, leaf_camera):
    """Validate the opt-in fixed scene camera before creating a run directory."""

    if values is None:
        return None
    if leaf_camera:
        return '--scene-camera cannot be combined with --leaf-camera'
    if len(values) != 7:
        return '--scene-camera requires 7 finite floats: eye xyz, center xyz, fov'
    try:
        eye = tuple(float(value) for value in values[:3])
        center = tuple(float(value) for value in values[3:6])
        fov = float(values[6])
    except (TypeError, ValueError):
        return '--scene-camera requires 7 finite floats: eye xyz, center xyz, fov'
    if any(not math.isfinite(value) for value in (*eye, *center, fov)):
        return '--scene-camera requires 7 finite floats: eye xyz, center xyz, fov'
    if not 0.0 < fov < 179.0:
        return '--scene-camera fov must be strictly between 0 and 179 degrees'
    direction = tuple(center[i] - eye[i] for i in range(3))
    distance = math.hypot(*direction)
    if not math.isfinite(distance) or distance <= 1.0e-9:
        return '--scene-camera eye and center must be distinct finite points'
    # The injected camera uses world +Y as its up vector. A view direction
    # with no horizontal component makes look-at's right vector singular.
    horizontal = math.hypot(direction[0], direction[2])
    if horizontal <= 1.0e-9 * distance:
        return '--scene-camera eye-to-center direction cannot be vertical-only'
    return None


def leaf_freeze_error(leaf_camera, leaf_freeze):
    """Return a parser error when freezing is not paired with leaf camera QA."""

    if leaf_freeze and not leaf_camera:
        return '--leaf-freeze requires --leaf-camera'
    return None


def geometry_at_second_error(geometry_at, geometry_at_second, ticks):
    """Return a parser error for an invalid optional second geometry tick."""

    if geometry_at_second is None:
        return None
    if geometry_at is None:
        return '--geometry-at-second requires --geometry-at'
    if geometry_at_second <= 0 or geometry_at_second > ticks:
        return '--geometry-at-second must be positive and within --ticks'
    if geometry_at_second == geometry_at:
        return '--geometry-at-second must differ from --geometry-at'
    return None


def xegtao_init_failure_error(enabled, implementation, ao):
    """Return a parser error unless the init-failure seam is meaningful."""

    if not enabled:
        return None
    if implementation != 'xegtao':
        return '--xegtao-init-failure requires --implementation xegtao'
    if ao <= 0:
        return '--xegtao-init-failure requires --ao > 0'
    return None


def xegtao_prepared_canonical_error(enabled, implementation, resolution, ao):
    """Return an admission error for the explicit prepared-canonical seam."""

    if not enabled:
        return None
    if implementation != 'xegtao':
        return '--xegtao-prepared-canonical requires --implementation xegtao'
    if resolution != 'full':
        return '--xegtao-prepared-canonical requires --xegtao-resolution full'
    if ao <= 0:
        return '--xegtao-prepared-canonical requires --ao > 0'
    return None


def xegtao_depth_mip_offset_error(args):
    """Admit the bounded QA-only native depth-mip offset diagnostic."""

    value = getattr(args, 'xegtao_depth_mip_offset', None)
    if value is None:
        return None
    if (isinstance(value, bool) or not isinstance(value, (int, float)) or
            not math.isfinite(value) or not 2.0 <= value <= 6.0):
        return '--xegtao-depth-mip-offset must be a finite float in [2,6]'
    if args.ao <= 0:
        return '--xegtao-depth-mip-offset requires --ao > 0'
    if args.implementation not in ('default', 'xegtao'):
        return '--xegtao-depth-mip-offset requires --implementation default or xegtao'
    if getattr(args, 'xegtao_init_failure', False):
        return '--xegtao-depth-mip-offset cannot combine with --xegtao-init-failure'
    if getattr(args, 'xegtao_prepared_canonical', False):
        return '--xegtao-depth-mip-offset cannot combine with --xegtao-prepared-canonical'
    if getattr(args, 'ao_cycle', None) is not None:
        return '--xegtao-depth-mip-offset cannot combine with live AO cycling'
    if getattr(args, 'frame_upload_mode', None) is not None:
        return '--xegtao-depth-mip-offset cannot combine with frame-upload diagnostics'
    if getattr(args, 'ao_admission', False):
        return '--xegtao-depth-mip-offset cannot combine with --ao-admission'
    incompatible = (
        'mobile_ao', 'ao_resources', 'resize_ao', 'raw_ao_depth', 'foreground_ao',
        'water_ao', 'legacy_foliage_viewport', 'foliage_no_depth', 'no_grounding',
        'no_lawn_lift', 'no_connector_lift', 'no_log_grounding', 'no_tree_grounding',
        'no_foliage_mask', 'legacy_foliage_blend',
    )
    if any(getattr(args, key, False) for key in incompatible):
        return '--xegtao-depth-mip-offset cannot combine with AO/geometry diagnostic masks'
    return None


def ao_admission_error(args):
    """Admit the QA-only AO resource/conservation probe."""

    if not getattr(args, 'ao_admission', False):
        return None
    if args.ao <= 0:
        return '--ao-admission requires --ao > 0'
    if args.implementation not in ('default', 'legacy'):
        return '--ao-admission requires --implementation default or legacy'
    if getattr(args, 'ao_cycle', None) is not None:
        return '--ao-admission cannot combine with --ao-cycle'
    if getattr(args, 'xegtao_init_failure', False):
        return '--ao-admission cannot combine with --xegtao-init-failure'
    return None


def foreground_ao_error(args):
    """Reject the ambiguous foreground/water mask combination."""

    if args.foreground_ao and args.water_ao:
        return '--foreground-ao cannot combine with --water-ao'
    return None


parser = argparse.ArgumentParser()
parser.add_argument('--exe', default='build/board-qa/release/mp6native.exe')
parser.add_argument('--name', default='baseline')
parser.add_argument('--seconds', type=int, default=240)
parser.add_argument('--capsule', type=int, default=-1)
parser.add_argument('--input-script', default='period:30;timeout:149000;pressuntil:a/qa.complete/done;wait:60')
parser.add_argument('--gpu-timings', action='store_true', help='Log real asynchronous GPU pass timestamps')
parser.add_argument('--gpu-validation', action='store_true', help='Enable Dawn validation and robustness for correctness runs, not timings')
parser.add_argument('--route', default='')
parser.add_argument('--window-size', default='1024x768')
parser.add_argument('--widescreen', action='store_true')
parser.add_argument('--results-geometry', action='store_true', help='QA-only results theater mesh census')
parser.add_argument('--ao', type=int, choices=(0, 1, 2), default=0)
parser.add_argument('--implementation', choices=('legacy', 'xegtao', 'default'), default='legacy',
                    help='AO implementation: legacy compatibility path or opt-in XeGTAO reference')
parser.add_argument('--ao-admission', action='store_true',
                    help='QA-only AO resource/conservation admission probe; not performance evidence')
parser.add_argument('--xegtao-quality', choices=('low','medium','high','ultra'), default='high')
parser.add_argument('--xegtao-denoise', type=int, choices=(1,2,3), default=2)
parser.add_argument('--xegtao-resolution', choices=('full','half'), default='full',
                    help='Explicit experimental AO resolution; never inherited from the environment')
parser.add_argument('--xegtao-depth-storage', choices=('r32float','r16float'), default='r32float',
                    help='XeGTAO working-depth storage format; never inherited from the environment')
parser.add_argument('--xegtao-depth-mip-offset', type=float, metavar='FLOAT',
                    help='QA-only native XeGTAO depth-mip sampling offset in [2,6]')
parser.add_argument('--xegtao-init-failure', action='store_true',
                    help='QA-only malformed XeGTAO shader initialization fallback diagnostic')
parser.add_argument('--xegtao-prepared-canonical', action='store_true',
                    help='QA-only explicit full-resolution canonical prepared-depth input')
parser.add_argument('--xegtao-batch-compute', action='store_true',
                    help='Opt-in XeGTAO shared compute-pass submission; shaders unchanged')
parser.add_argument('--leaf-camera', action='store_true', help='QA-only close-up pan of the decorative Treetop flower')
parser.add_argument('--leaf-camera-mode', choices=('pan','static','dolly','fov'), default='pan',
                    help='QA-only leaf camera motion: sinusoidal pan, static, dolly, or FOV sweep')
parser.add_argument('--scene-camera', nargs=7, type=float, metavar='FLOAT',
                    help='QA-only fixed scene camera: eye xyz, center xyz, fov')
parser.add_argument('--leaf-freeze', action='store_true',
                    help='QA-only freeze of the resolved b01_m002 flower fixture')
parser.add_argument('--mobile-ao', action='store_true', help='QA-only Android shading budget on the desktop renderer')
parser.add_argument('--ao-cycle', type=int, help='QA-only live AO sweep starting at this tick')
parser.add_argument('--ao-resources', action='store_true',
                    help='QA-only retained resource census; logical texel bytes, not VRAM or timing evidence')
parser.add_argument('--resize-ao', action='store_true',
                    help='QA-only hidden-window 1281x721/960x540 XeGTAO resize stress')
parser.add_argument('--ao-fi-matrix-range', metavar='FROM:TO',
                    help='QA-only FI matrix capture window (inclusive, at most 120 ticks)')
parser.add_argument('--ao-fi-matrix-limit', type=int, default=256,
                    help='QA-only FI matrix record cap (default 256, maximum 1024)')
parser.add_argument('--ao-fi-pixels', metavar='FROM:TO:STRIDE',
                    help='QA-only sampled replay pixels at deterministic phases; not cadence/performance evidence')
parser.add_argument('--deferred-capture', metavar='FROM:TO',
                    help='QA-only consecutive natural-alpha capture; separate cadence executable required')
parser.add_argument('--ao-buffers-tick', type=int,
                    help='Private QA-only AO intermediate capture at this tick and the next')
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
parser.add_argument('--geometry-at-second', type=int,
                    help='QA-only second world-space mesh capture tick for frozen-fixture proof')
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
parser.add_argument('--capture-rect', nargs=4, type=int, metavar=('X','Y','W','H'),
                    help='GPU-side capture crop in source framebuffer pixels')
parser.add_argument('--capture-trigger', default='capsule.begin',
                    help='substring of a game log line that starts the capture')
parser.add_argument('--one-turn', action='store_true')
parser.add_argument('--stop-timechange', action='store_true', help='Stop 600 ticks after a real day/night sequence finishes')
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
if (error := buffer_capture_error(args, root)):
    parser.error(error)
if (error := batch_compute_error(args)):
    parser.error(error)
if not re.fullmatch(r'[1-9][0-9]{2,3}x[1-9][0-9]{2,3}', args.window_size):
    parser.error('--window-size must be WIDTHxHEIGHT, each 100..9999')
if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
    parser.error('--name must be a single simple directory name')
if (error := leaf_camera_mode_error(args.leaf_camera, args.leaf_camera_mode)):
    parser.error(error)
if (error := scene_camera_error(args.scene_camera, args.leaf_camera)):
    parser.error(error)
if (error := xegtao_init_failure_error(args.xegtao_init_failure, args.implementation, args.ao)):
    parser.error(error)
if (error := xegtao_prepared_canonical_error(
        args.xegtao_prepared_canonical, args.implementation, args.xegtao_resolution, args.ao)):
    parser.error(error)
if (error := xegtao_depth_mip_offset_error(args)):
    parser.error(error)
if (error := ao_admission_error(args)):
    parser.error(error)
if (error := foreground_ao_error(args)):
    parser.error(error)
if args.ao_resources and not args.ao and args.ao_cycle is None:
    parser.error('--ao-resources requires --ao > 0 or --ao-cycle')
if args.seconds <= 0 or args.ticks <= 0 or not 0 <= args.tick_hz <= 1000 or args.stop_round < 0:
    parser.error('positive seconds/ticks, nonnegative stop-round, and tick-hz in 0..1000 required')
if not 0 <= args.capture_frames <= 1000 or not 1 <= args.capture_stride <= 1000:
    parser.error('capture-frames must be 0..1000 and capture-stride 1..1000')
if (error := leaf_freeze_error(args.leaf_camera, args.leaf_freeze)):
    parser.error(error)
if (error := geometry_at_second_error(args.geometry_at, args.geometry_at_second, args.ticks)):
    parser.error(error)
if (error := resize_ao_error(args.resize_ao, args.window_size, args.ao, args.implementation)):
    parser.error(error)
if (error := default_ao_error(args.implementation, args.ao, args.xegtao_quality,
                              args.xegtao_denoise, args.xegtao_resolution,
                              args.xegtao_depth_storage)):
    parser.error(error)
if (error := ao_fi_matrix_range_error(args.ao_fi_matrix_range, args.ao_fi_matrix_limit,
                                      args.unlocked_fps, args.tick_hz)):
    parser.error(error)
if (error := ao_fi_pixels_error(args)):
    parser.error(error)
if (error := deferred_capture_error(args)):
    parser.error(error)
if args.deferred_capture and (root / args.exe).resolve() != (root / 'build/board-cadence-qa/release/mp6native.exe').resolve():
    parser.error('--deferred-capture requires the separate board-cadence-QA executable')
if (error := capture_rect_error(args.capture_rect, args.capture_frames)):
    parser.error(error)
if args.ao_fi_pixels and (root / args.exe).resolve() != (root / 'build/board-qa/release/mp6native.exe').resolve():
    parser.error('--ao-fi-pixels requires the isolated board-QA executable')
if (args.xegtao_depth_mip_offset is not None and
        (root / args.exe).resolve() != (root / 'build/board-qa/release/mp6native.exe').resolve()):
    parser.error('--xegtao-depth-mip-offset requires the isolated board-QA executable')
if (args.ao_admission and
        (root / args.exe).resolve() != (root / 'build/board-qa/release/mp6native.exe').resolve()):
    parser.error('--ao-admission requires the isolated board-QA executable')
if (args.foreground_ao and
        (root / args.exe).resolve() != (root / 'build/board-qa/release/mp6native.exe').resolve()):
    parser.error('--foreground-ao requires the isolated board-QA executable')
ao_fi_matrix_range = parse_ao_fi_matrix_range(args.ao_fi_matrix_range)
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
if executable in ((root / 'build/board-qa/release/mp6native.exe').resolve(),
                   (root / 'build/board-cadence-qa/release/mp6native.exe').resolve()):
    ready_path = executable.parent.parent / 'build-ready.json'
    if not ready_path.exists():
        parser.error('QA build did not finish successfully; rebuild with build_board_qa.py')
    ready = json.loads(ready_path.read_text())
    builder_hash = hashlib.sha256((root / 'tests/integration/build_board_qa.py').read_bytes()).hexdigest()
    fixture_hash=hashlib.sha256((root/'tests/integration/capsule_audit_fixture.c').read_bytes()).hexdigest()
    resource_probe_hash = hashlib.sha256((root/'tests/integration/ao_resource_probe.py').read_bytes()).hexdigest()
    resize_probe_hash = hashlib.sha256((root/'tests/integration/ao_resize_probe.py').read_bytes()).hexdigest()
    fi_matrix_probe_hash = hashlib.sha256((root/'tests/integration/ao_fi_matrix_probe.py').read_bytes()).hexdigest()
    fi_pixel_probe_hash = hashlib.sha256((root/'tests/integration/ao_fi_pixel_probe.py').read_bytes()).hexdigest()
    mip_offset_probe_hash = None
    if args.xegtao_depth_mip_offset is not None:
        mip_offset_probe = root / 'tests/integration/ao_mip_offset_probe.py'
        if not mip_offset_probe.is_file():
            parser.error('AO MIP offset probe source is missing; rebuild with build_board_qa.py')
        mip_offset_probe_hash = hashlib.sha256(mip_offset_probe.read_bytes()).hexdigest()
    admission_probe_hash = None
    if args.ao_admission:
        admission_probe = root / 'tests/integration/ao_admission_probe.py'
        if not admission_probe.is_file():
            parser.error('AO admission probe source is missing; rebuild with build_board_qa.py')
        admission_probe_hash = hashlib.sha256(admission_probe.read_bytes()).hexdigest()
    foreground_probe_hash = None
    if args.foreground_ao:
        foreground_probe = root / 'tests/integration/ao_foreground_probe.py'
        if not foreground_probe.is_file():
            parser.error('AO foreground probe source is missing; rebuild with build_board_qa.py')
        foreground_probe_hash = hashlib.sha256(foreground_probe.read_bytes()).hexdigest()
    geometry_probe_hash = hashlib.sha256((root/'tests/integration/ground_geometry_probe.h').read_bytes()).hexdigest()
    if (ready['sha256'] != binary_hash or ready['builder_sha256'] != builder_hash or
            ready.get('ao_resource_probe_sha256') != resource_probe_hash or
            ready.get('ao_resize_probe_sha256') != resize_probe_hash or
            ready.get('ao_fi_matrix_probe_sha256') != fi_matrix_probe_hash or
            ready.get('ao_fi_pixel_probe_sha256') != fi_pixel_probe_hash or
            (args.xegtao_depth_mip_offset is not None and
             ready.get('ao_mip_offset_probe_sha256') != mip_offset_probe_hash) or
            (args.ao_admission and
             ready.get('ao_admission_probe_sha256') != admission_probe_hash) or
            (args.foreground_ao and
             ready.get('ao_foreground_probe_sha256') != foreground_probe_hash) or
            ready.get('geometry_probe_sha256') != geometry_probe_hash or
            ready.get('capsule_fixture_sha256') != fixture_hash):
        parser.error('QA executable or fixtures are stale; rebuild with build_board_qa.py')
    if args.deferred_capture:
        deferred = ready.get('deferred_capture', {})
        expected_sources = ('tests/integration/build_ao_cadence_qa.py',
                            'tests/integration/ao_deferred_capture_source.py',
                            'tests/integration/ao_deferred_capture_backend.cpp')
        if set(deferred.get('sources', {})) != set(expected_sources):
            parser.error('missing deferred capture build provenance')
        for name, expected in deferred['sources'].items():
            if hashlib.sha256((root/name).read_bytes()).hexdigest() != expected:
                parser.error('deferred capture source is stale; rebuild build_ao_cadence_qa.py')
        expected_generated = {'frame_interp_fi_matrix_probe.c', 'frame_dump_fi_pixel_probe.c'}
        if set(deferred.get('generated_sources', {})) != expected_generated:
            parser.error('missing deferred capture generated source provenance')
        for name, expected in deferred['generated_sources'].items():
            if hashlib.sha256((ready_path.parent/name).read_bytes()).hexdigest() != expected:
                parser.error('deferred capture generated source is stale')
env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
env.update(SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
if args.ao_buffers_tick is not None:
    (directory / 'ao-buffers').mkdir()
    env['MP6_QA_AO_BUFFERS_TICK'] = str(args.ao_buffers_tick)
    env['MP6_QA_AO_BUFFERS_DIR'] = str(directory / 'ao-buffers')
if args.stop_timechange:
    env['MP6_QA_STOP_TIMECHANGE'] = '1'
if args.frame_upload_mode is not None:
    env['MP6_UPLOAD_MODE'] = str(args.frame_upload_mode)
if args.gpu_timings:
    env['MP6_GPU_TIMINGS'] = '1'
if args.gpu_validation:
    env['MP6_GPU_VALIDATION'] = '1'
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
# The inherited environment was scrubbed above. Set this after the common
# variables so an ambient MP6_AO_XEGTAO value cannot silently select a path.
env['MP6_AO_XEGTAO'] = str(int(args.implementation == 'xegtao'))
env['MP6_AO_XEGTAO_QUALITY'] = args.xegtao_quality
env['MP6_AO_XEGTAO_DENOISE'] = str(args.xegtao_denoise)
env['MP6_AO_XEGTAO_RESOLUTION'] = args.xegtao_resolution
env['MP6_AO_XEGTAO_DEPTH_STORAGE'] = args.xegtao_depth_storage
if args.xegtao_prepared_canonical:
    env['MP6_AO_XEGTAO_PREPARED_CANONICAL'] = '1'
if args.xegtao_batch_compute:
    env['MP6_AO_XEGTAO_BATCH_COMPUTE'] = '1'
if args.xegtao_depth_mip_offset is not None:
    env['MP6_QA_AO_DEPTH_MIP_OFFSET'] = str(args.xegtao_depth_mip_offset)
if args.ao_admission:
    env['MP6_QA_AO_ADMISSION'] = '1'
if args.implementation == 'default':
    for key in ('MP6_AO_XEGTAO', 'MP6_AO_XEGTAO_QUALITY', 'MP6_AO_XEGTAO_DENOISE',
                'MP6_AO_XEGTAO_RESOLUTION', 'MP6_AO_XEGTAO_DEPTH_STORAGE',
                'MP6_AO_XEGTAO_PREPARED_CANONICAL'):
        env.pop(key, None)
if args.xegtao_init_failure:
    env['MP6_QA_XEGTAO_INVALID_SHADER'] = '1'
if args.ao_resources:
    env['MP6_QA_AO_RESOURCES'] = '1'
if args.resize_ao:
    # The resize fixture always needs the source-bound v2 census.  Keep this
    # opt-in env assignment after the MP6 scrub so an inherited stress lever
    # cannot affect ordinary board QA.
    env['MP6_QA_RESIZE_AO'] = '1'
    env['MP6_QA_AO_RESOURCES'] = '1'
    if args.unlocked_fps:
        env['MP6_FI_DIAG'] = '1'
if ao_fi_matrix_range is not None:
    env['MP6_QA_AO_FI_MATRIX'] = '1'
    env['MP6_QA_AO_FI_MATRIX_FROM'] = str(ao_fi_matrix_range[0])
    env['MP6_QA_AO_FI_MATRIX_TO'] = str(ao_fi_matrix_range[1])
    env['MP6_QA_AO_FI_MATRIX_LIMIT'] = str(args.ao_fi_matrix_limit)
if args.ao_fi_pixels is not None:
    env['MP6_QA_AO_FI_PIXELS'] = args.ao_fi_pixels
if args.deferred_capture is not None:
    deferred_start, deferred_end = map(int, args.deferred_capture.split(':'))
    env['MP6_QA_DEFERRED_FROM'] = str(deferred_start)
    env['MP6_QA_DEFERRED_TO'] = str(deferred_end)
    # The separate build removes phase forcing and per-tick capture gating;
    # this config only enables existing submitted-matrix collection.
    env['MP6_QA_AO_FI_PIXELS'] = f'{deferred_start-1}:{deferred_end}:1'
if args.leaf_camera:
    env['MP6_QA_LEAF_CAMERA'] = '1'
    env['MP6_QA_LEAF_CAMERA_MODE'] = args.leaf_camera_mode
if args.scene_camera is not None:
    env['MP6_QA_SCENE_CAMERA'] = ' '.join(f'{value:.9g}' for value in args.scene_camera)
if args.leaf_freeze:
    env['MP6_QA_LEAF_FREEZE'] = '1'
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
if args.geometry_at_second is not None:
    env['MP6_QA_GEOMETRY_AT_SECOND'] = str(args.geometry_at_second)
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
    if args.capture_rect is not None:
        env['MP6_FRAME_DUMP_RECT'] = ','.join(map(str, args.capture_rect))
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
isolate_background(env, executable)
with (directory / 'game.log').open('w', encoding='utf-8') as output:
    process = subprocess.Popen([str(executable), str(args.ticks), '--input-script',
                                args.input_script],
                               cwd=directory, env=env, stdout=output, stderr=subprocess.STDOUT,
                               creationflags=background_process_flags())
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
_DEPTH_STORAGE_LOG_RE = re.compile(
    r'\[MP6-AO\] XeGTAO depth storage: requested=(r32float|r16float) '
    r'actual=(r32float|r16float)'
)


def validate_xegtao_depth_storage(log, requested):
    """Require the native XeGTAO run to report the requested storage format."""

    records = _DEPTH_STORAGE_LOG_RE.findall(log)
    if not records:
        raise RuntimeError(
            'XeGTAO run did not report depth storage; expected '
            "'[MP6-AO] XeGTAO depth storage: requested=... actual=...'"
        )
    for logged_requested, actual in records:
        if logged_requested != requested or actual != requested:
            raise RuntimeError(
                f'XeGTAO depth storage mismatch: requested={requested} '
                f'log_requested={logged_requested} actual={actual}'
            )
    return dict(requested=requested, actual=records[-1][1], records=len(records))


_XEGTAO_PREPARED_CANONICAL_MARKER = '[MP6-AO] XeGTAO preparation:'
_XEGTAO_PREPARED_CANONICAL_ROW = '[MP6-AO] XeGTAO preparation: canonical'


def validate_xegtao_prepared_canonical(log, requested, init_failure=False):
    """Require exactly one canonical row only for a successful explicit opt-in."""

    rows = []
    for line in log.splitlines():
        if _XEGTAO_PREPARED_CANONICAL_MARKER in line:
            rows.append(line[line.index(_XEGTAO_PREPARED_CANONICAL_MARKER):].strip())
    if requested and not init_failure:
        if rows != [_XEGTAO_PREPARED_CANONICAL_ROW]:
            raise RuntimeError(
                'explicit prepared-canonical XeGTAO run requires exactly one canonical preparation row'
            )
        return dict(requested=True, actual='canonical', records=1)
    if rows:
        raise RuntimeError(
            'prepared-canonical XeGTAO preparation row is not allowed without a successful explicit opt-in'
        )
    return dict(requested=bool(requested), actual='not-emitted', records=0)


_AO_SHUTDOWN_COUNTERS_RE = re.compile(
    r'\[MP6-AO\] encoded=(\d+) composited=(\d+) foliage=(\d+) xegtao=(\d+)'
)
_AO_MIP_OFFSET_MARKER = '[AO-MIP-OFFSET]'
_AO_MIP_OFFSET_ERROR_MARKER = '[AO-MIP-OFFSET-ERROR]'
_AO_MIP_OFFSET_ROW_RE = re.compile(
    r'^\[AO-MIP-OFFSET\] requested=(?P<requested>[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?) '
    r'applied=(?P<applied>[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)$'
)


def validate_xegtao_depth_mip_offset(log, requested, exit_code=0, reason='exited'):
    """Validate one native MIP-offset proof, or its absence when disabled."""

    if not isinstance(log, str):
        raise TypeError('log must be text')
    if requested is not None:
        if (isinstance(requested, bool) or not isinstance(requested, (int, float)) or
                not math.isfinite(requested) or not 2.0 <= requested <= 6.0):
            raise ValueError('requested MIP offset must be a finite float in [2,6]')
    if _AO_MIP_OFFSET_ERROR_MARKER.casefold() in log.casefold():
        raise RuntimeError('AO MIP offset run reported [AO-MIP-OFFSET-ERROR]')
    rows = []
    for line in log.splitlines():
        if _AO_MIP_OFFSET_MARKER not in line:
            continue
        match = _AO_MIP_OFFSET_ROW_RE.fullmatch(line.strip())
        if match is None:
            raise RuntimeError('malformed [AO-MIP-OFFSET] proof row')
        logged_requested = float(match.group('requested'))
        applied = float(match.group('applied'))
        if not math.isfinite(logged_requested) or not math.isfinite(applied):
            raise RuntimeError('AO MIP offset proof contains a non-finite value')
        rows.append((logged_requested, applied))
    if requested is None:
        if rows:
            raise RuntimeError('AO MIP offset marker was emitted without a request')
        return None
    if exit_code != 0 or reason != 'exited':
        raise RuntimeError(
            f'AO MIP offset run did not exit cleanly: exit={exit_code} reason={reason}'
        )
    if len(rows) != 1:
        raise RuntimeError(
            f'AO MIP offset run requires exactly one proof row; found {len(rows)}'
        )
    logged_requested, applied = rows[0]
    if not math.isclose(logged_requested, float(requested), rel_tol=1.0e-6, abs_tol=1.0e-6):
        raise RuntimeError(
            f'AO MIP offset requested mismatch: requested={requested} logged={logged_requested}'
        )
    if not math.isclose(applied, float(requested), rel_tol=1.0e-6, abs_tol=1.0e-6):
        raise RuntimeError(
            f'AO MIP offset applied mismatch: requested={requested} applied={applied}'
        )
    return dict(requested=float(requested), logged_requested=logged_requested,
                applied=applied, records=1)


def validate_xegtao_mip_offset_runtime(log, exit_code=0, reason='exited'):
    """Require explicit XeGTAO selection/counters for the offset diagnostic."""

    if exit_code != 0 or reason != 'exited':
        raise RuntimeError(
            f'XeGTAO MIP offset run did not exit cleanly: exit={exit_code} reason={reason}'
        )
    selection_marker = '[MP6-AO] requested implementation: XeGTAO (environment override)'
    if log.count(selection_marker) != 1:
        raise RuntimeError('XeGTAO MIP offset run requires one explicit Xe selection marker')
    reference_rows = [line.strip() for line in log.splitlines()
                      if line.startswith('[MP6-AO] XeGTAO reference:') or
                      line.startswith('[MP6-AO] XeGTAO experimental:')]
    if len(reference_rows) != 1:
        raise RuntimeError('XeGTAO MIP offset run requires one Xe reference marker')
    bad_markers = (
        '[MP6-CRASH]', '[FATAL]', '[AURORA FATAL:', '[AURORA ERROR',
        'GPUValidationError', 'uncaptured error', '[MP6-AO] XeGTAO unavailable:',
        'using legacy', 'fallback',
    )
    bad = next((marker for marker in bad_markers if marker.casefold() in log.casefold()), None)
    if bad is not None:
        raise RuntimeError(f'XeGTAO MIP offset run reported fallback/fatal marker: {bad}')
    records = _AO_SHUTDOWN_COUNTERS_RE.findall(log)
    if len(records) != 1:
        raise RuntimeError('XeGTAO MIP offset run requires exactly one shutdown counter record')
    encoded, composited, foliage, xegtao = (int(value) for value in records[0])
    if encoded <= 0 or composited <= 0 or xegtao <= 0:
        raise RuntimeError('XeGTAO MIP offset run produced no positive XeGTAO work')
    if encoded != composited or encoded != xegtao:
        raise RuntimeError(
            f'XeGTAO MIP offset counters disagree: encoded={encoded} '
            f'composited={composited} xegtao={xegtao}'
        )
    return dict(
        implementation='xegtao', selection='environment-override',
        reference=reference_rows[0],
        markers=dict(selection=selection_marker, reference=reference_rows[0]),
        counters=dict(encoded=encoded, composited=composited,
                      foliage=foliage, xegtao=xegtao),
        counter_records=1, exit_code=exit_code, reason=reason,
    )


def validate_foreground_ao(log, implementation, enabled, exit_code=0, reason='exited'):
    """Validate the exact upstream foreground proof when the probe is active."""

    if not isinstance(log, str):
        raise TypeError('log must be text')
    error_marker = '[AO-FOREGROUND-ERROR]'
    if error_marker.casefold() in log.casefold():
        raise RuntimeError('AO foreground probe reported [AO-FOREGROUND-ERROR]')
    marker = '[AO-FOREGROUND] implementation=xegtao mode=premultiplied-rgb'
    rows = [line.strip() for line in log.splitlines() if '[AO-FOREGROUND]' in line]
    if not enabled:
        if rows:
            raise RuntimeError('unexpected AO foreground probe marker without --foreground-ao')
        return None
    if implementation == 'legacy':
        if rows:
            raise RuntimeError('legacy foreground mode must not emit the XeGTAO proof marker')
        return None
    if implementation not in ('default', 'xegtao'):
        raise RuntimeError('AO foreground proof requires default or xegtao implementation')
    if exit_code != 0 or reason != 'exited':
        raise RuntimeError(
            f'AO foreground run did not exit cleanly: exit={exit_code} reason={reason}'
        )
    if rows != [marker]:
        raise RuntimeError(
            f'AO foreground proof requires exactly one marker; found {len(rows)}'
        )
    return dict(implementation='xegtao', mode='premultiplied-rgb', records=1,
                exit_code=exit_code, reason=reason)


_XEGTAO_INIT_FAILURE_INJECTION_MARKER = '[MP6-AO-QA] injecting invalid XeGTAO main shader'
_XEGTAO_INIT_FAILURE_MARKER = '[MP6-AO] XeGTAO unavailable:'
_XEGTAO_INIT_FAILURE_BAD_MARKERS = (
    '[MP6-CRASH]', '[FATAL]', '[AURORA FATAL:', '[AURORA ERROR', 'GPUValidationError',
    'uncaptured error',
)


def validate_xegtao_init_failure(log, exit_code=0, reason='exited'):
    """Validate that malformed XeGTAO initialization fell back safely."""

    if exit_code != 0 or reason != 'exited':
        raise RuntimeError(
            f'XeGTAO init-failure run did not exit cleanly: exit={exit_code} reason={reason}'
        )
    if _XEGTAO_INIT_FAILURE_INJECTION_MARKER not in log:
        raise RuntimeError(
            "XeGTAO init-failure run did not report "
            "'[MP6-AO-QA] injecting invalid XeGTAO main shader'"
        )
    if _XEGTAO_INIT_FAILURE_MARKER not in log:
        raise RuntimeError(
            "XeGTAO init-failure run did not report "
            "'[MP6-AO] XeGTAO unavailable:'"
        )
    if 'using legacy' not in log.casefold():
        raise RuntimeError("XeGTAO init-failure run did not report 'using legacy'")
    bad = next((marker for marker in _XEGTAO_INIT_FAILURE_BAD_MARKERS
                if marker.casefold() in log.casefold()), None)
    if bad is not None:
        raise RuntimeError(f'XeGTAO init-failure run reported fatal/validation marker: {bad}')
    records = _AO_SHUTDOWN_COUNTERS_RE.findall(log)
    if not records:
        raise RuntimeError(
            "XeGTAO init-failure run did not report shutdown counters; expected "
            "'[MP6-AO] encoded=... composited=... foliage=... xegtao=...'"
        )
    encoded, composited, foliage, xegtao = (int(value) for value in records[-1])
    if encoded <= 0 or composited <= 0:
        raise RuntimeError(
            f'XeGTAO init-failure run produced no AO work: encoded={encoded} '
            f'composited={composited}'
        )
    if xegtao != 0:
        raise RuntimeError(
            f'XeGTAO init-failure run unexpectedly encoded XeGTAO work: xegtao={xegtao}'
        )
    return dict(unavailable=True, legacy_fallback=True,
                counters=dict(encoded=encoded, composited=composited,
                              foliage=foliage, xegtao=xegtao),
                counter_records=len(records), exit_code=exit_code, reason=reason)


xegtao_depth_storage = None
xegtao_prepared_canonical = None
expected_init_failure = None
ao_resources = None
resize_ao_evidence = None
default_selection = None
ao_fi_matrix = None
ao_mip_offset = None
ao_admission = None
foreground_ao_evidence = None
if args.xegtao_init_failure:
    expected_init_failure = validate_xegtao_init_failure(log, result, reason)
elif args.implementation == 'default':
    default_selection = validate_default_ao(log, result, reason)
elif args.ao and args.implementation == 'xegtao':
    xegtao_depth_storage = validate_xegtao_depth_storage(log, args.xegtao_depth_storage)
xegtao_prepared_canonical = validate_xegtao_prepared_canonical(
    log, args.xegtao_prepared_canonical, args.xegtao_init_failure)
xegtao_batch_compute = validate_batch_compute(
    log, args.xegtao_batch_compute, args.xegtao_quality, args.xegtao_denoise, result, reason)
if args.xegtao_depth_mip_offset is not None:
    mip_offset_proof = validate_xegtao_depth_mip_offset(
        log, args.xegtao_depth_mip_offset, result, reason)
    if args.implementation == 'default':
        mip_offset_runtime = default_selection
    else:
        mip_offset_runtime = validate_xegtao_mip_offset_runtime(log, result, reason)
    ao_mip_offset = dict(
        requested=args.xegtao_depth_mip_offset,
        logged_requested=mip_offset_proof['logged_requested'],
        applied=mip_offset_proof['applied'],
        records=mip_offset_proof['records'],
        probe='ao_mip_offset_probe',
        probe_sha256=mip_offset_probe_hash,
        runtime=mip_offset_runtime,
        scope='Native fixed-parameter diagnostic/provenance evidence; not pixel quality, cadence, or performance acceptance',
    )
else:
    validate_xegtao_depth_mip_offset(log, None, result, reason)
foreground_ao_evidence = validate_foreground_ao(
    log, args.implementation, args.foreground_ao, result, reason)
if args.ao_admission:
    if result != 0 or reason != 'exited':
        raise RuntimeError(
            f'AO admission run did not exit cleanly: exit={result} reason={reason}'
        )
    from tests.integration.ao_admission_probe import parse_admission_log
    ao_admission = parse_admission_log(log)
    if not isinstance(ao_admission, dict):
        raise RuntimeError('AO admission probe did not return a report object')
elif '[AO-ADMISSION]' in log or '[AO-ADMISSION-ERROR]' in log:
    raise RuntimeError('unexpected AO admission probe marker without --ao-admission')
if args.ao_resources or args.resize_ao:
    ao_resources = parse_resource_log(log)
if args.resize_ao:
    resize_ao_evidence = validate_resize_ao_evidence(
        log, ao_resources, require_fi=args.unlocked_fps)
if ao_fi_matrix_range is not None:
    ao_fi_matrix = {
        'from': ao_fi_matrix_range[0],
        'to': ao_fi_matrix_range[1],
        'limit': args.ao_fi_matrix_limit,
        'probe': 'ao_fi_matrix_probe',
        'probe_sha256': hashlib.sha256(
            (root / 'tests/integration/ao_fi_matrix_probe.py').read_bytes()).hexdigest(),
    }
report = dict(request=vars(args), executable=str(executable), sha256=binary_hash,
              exit_code=result, reason=reason, elapsed_seconds=time.monotonic()-start,
              completion_marker='[EVENT] qa.complete=done' in log,
              turn_end_count=log.count('[BOARD-QA] turn.end'),
              capture_count=len(list((directory/'frames').glob('*.mfd'))),
              crash_marker=any(marker in log for marker in
                               ('[MP6-CRASH]', '[FATAL]', '[AURORA FATAL:')))
if xegtao_depth_storage is not None:
    report['xegtao_depth_storage'] = xegtao_depth_storage
if xegtao_prepared_canonical is not None:
    report['xegtao_prepared_canonical'] = xegtao_prepared_canonical
if xegtao_batch_compute is not None:
    report['xegtao_batch_compute'] = xegtao_batch_compute
if args.ao_buffers_tick is not None:
    report['ao_buffers'] = validate_buffer_capture(
        args, root, directory, executable, binary_hash, result, reason, log)
    report['ao_buffers']['effective_policy'] = validate_ao_fi_pixel_policy(
        log, args.implementation, args.xegtao_quality, args.xegtao_denoise, result, reason)
if expected_init_failure is not None:
    report['expected_init_failure'] = expected_init_failure
if default_selection is not None:
    report['default_selection'] = default_selection
if ao_resources is not None:
    # Keep the report tied to the same strict parser used for admission;
    # this also preserves the existing source-level wiring contract.
    report['ao_resources'] = parse_resource_log(log)
if resize_ao_evidence is not None:
    report['resize_ao'] = resize_ao_evidence
if ao_fi_matrix is not None:
    report['ao_fi_matrix'] = ao_fi_matrix
if ao_mip_offset is not None:
    report['ao_mip_offset'] = ao_mip_offset
if ao_admission is not None:
    report['ao_admission'] = dict(
        ao_admission,
        probe='ao_admission_probe',
        probe_sha256=admission_probe_hash,
        scope='Native AO admission/resource diagnostic; not pixel quality, cadence, or performance evidence',
    )
if foreground_ao_evidence is not None:
    report['foreground_ao'] = dict(
        foreground_ao_evidence,
        probe='ao_foreground_probe',
        probe_sha256=foreground_probe_hash,
        scope='Native foreground AO routing proof; not pixel quality or performance evidence',
    )
if args.ao_fi_pixels is not None:
    report['ao_fi_pixels'] = dict(
        schedule=args.ao_fi_pixels,
        phases=[0.0, 0.25, 0.5, 0.75],
        effective_policy=validate_ao_fi_pixel_policy(
            log, args.implementation, args.xegtao_quality, args.xegtao_denoise, result, reason),
        probe_sha256=hashlib.sha256((root/'tests/integration/ao_fi_pixel_probe.py').read_bytes()).hexdigest(),
        scope='Sampled fixed-phase native replay pixels; not natural cadence or performance evidence')
if args.deferred_capture is not None:
    report['deferred_capture'] = validate_deferred_capture(
        log, directory, args.capture_frames, result, reason)
    if args.ao:
        report['deferred_capture']['effective_policy'] = validate_ao_fi_pixel_policy(
            log, args.implementation, args.xegtao_quality, args.xegtao_denoise, result, reason)
(directory / 'result.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print('Diagnostic only: inspect effect/state and camera output; exit 0 is not a gameplay pass.', flush=True)
