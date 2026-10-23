"""Strict admission and evidence checks for consecutive natural-alpha captures."""
import hashlib
import math
from pathlib import Path
import re

from tests.integration import ao_fi_pixel_evidence as pixel


def deferred_capture_error(args):
    spec = getattr(args, 'deferred_capture', None)
    if spec is None:
        if Path(args.exe).as_posix().endswith('board-cadence-qa/release/mp6native.exe'):
            return 'the cadence QA executable requires --deferred-capture'
        return None
    match = re.fullmatch(r'([0-9]+):([0-9]+)', spec)
    if not match:
        return '--deferred-capture must be FROM:TO'
    start, end = map(int, match.groups())
    if not 3000 <= start <= end < args.ticks or end-start > 119:
        return '--deferred-capture requires a <=119 tick window after 3000 and before shutdown'
    if args.ao_fi_matrix_range != f'{start-1}:{end}' or args.ao_fi_pixels is not None:
        return '--deferred-capture requires preceding-tick matrix coverage and no fixed phases'
    if not args.unlocked_fps or args.tick_hz != 60 or not args.leaf_camera or not args.leaf_freeze:
        return '--deferred-capture requires unlocked FPS, tick-hz 60 and frozen leaf camera fixture'
    if args.implementation not in ('default', 'xegtao') or args.ao not in (0, 2) or args.aa != 0:
        return '--deferred-capture requires XeGTAO AO 0/2 and AA Off'
    if args.xegtao_resolution != 'full' or args.xegtao_depth_storage != 'r32float':
        return '--deferred-capture requires Full/R32'
    if (not 2 <= args.capture_frames <= 128 or args.capture_stride != 1 or
            args.capture_delay != 0 or args.capture_trigger != '-' or not args.capture_rect):
        return '--deferred-capture requires 2..128 consecutive frames and an explicit crop'
    x, y, w, h = args.capture_rect
    if min(x, y) < 0 or min(w, h) <= 0 or ((w*4+255)&~255)*h*args.capture_frames > 64*1024*1024:
        return '--deferred-capture exceeds the 64 MiB readback budget or has an invalid crop'
    for key in ('xegtao_init_failure', 'xegtao_prepared_canonical', 'resize_ao',
                'raw_ao_depth', 'foreground_ao', 'water_ao', 'no_grounding',
                'no_lawn_lift', 'no_connector_lift', 'no_log_grounding',
                'no_tree_grounding', 'no_foliage_mask', 'legacy_foliage_viewport',
                'foliage_no_depth', 'legacy_foliage_blend'):
        if getattr(args, key):
            return f'--deferred-capture cannot combine {key}'
    if args.ao_cycle is not None or args.scene_camera is not None or args.xegtao_depth_mip_offset is not None:
        return '--deferred-capture cannot combine live/camera/MIP overrides'
    return None


def validate_deferred_capture(log, directory, requested, exit_code, reason):
    if exit_code != 0 or reason != 'exited':
        raise ValueError('deferred capture did not exit cleanly')
    for bad in (*pixel.ERROR_MARKERS, '[AO-DEFERRED-ERROR]'):
        if bad.casefold() in log.casefold():
            raise ValueError(f'deferred capture error: {bad}')
    # Normal shutdown destroys Dawn's device after the completed burst. Do
    # not confuse that message with device/surface failure during capture.
    capture_prefix = log.split('[AO-DEFERRED-CAPTURE]', 1)[0].casefold()
    for bad in ('device lost:', 'surface present failed', 'surface texture acquisition timed out',
                'releasing surface', 'dropping surface', 'buffer mapping aborted'):
        if bad in capture_prefix:
            raise ValueError(f'deferred device/surface failure before completion: {bad}')
    def one(pattern):
        rows = re.findall(pattern, log, re.M)
        if len(rows) != 1:
            raise ValueError(f'missing/duplicate deferred summary: {pattern}')
        return tuple(map(int, rows[0]))
    queued, captured, written, byte_count, error = one(
        r'^\[AO-DEFERRED-CAPTURE\] queued=(\d+) captured=(\d+) written=(\d+) bytes=(\d+) error=(\d+)$')
    wanted, front_queued, complete = one(
        r'^\[AO-DEFERRED-FINISH\] requested=(\d+) queued=(\d+) complete=(\d+)$')
    meta_count, meta_error = one(r'^\[AO-DEFERRED-METADATA\] count=(\d+) error=(\d+) alpha=natural$')
    if ({queued, captured, written, wanted, front_queued, meta_count} != {requested}
            or error or meta_error or complete != 1 or not 0 < byte_count <= 64*1024*1024):
        raise ValueError('incomplete deferred capture or exceeded memory bound')
    frames_path = Path(directory)/'frames'
    rows = pixel.load_index(frames_path/'index.csv')
    if len(rows) != requested or len(list(frames_path.glob('*.mfd'))) != requested:
        raise ValueError('deferred file/index count mismatch')
    proofs = [pixel._json_object(line.split('[AO-DEFERRED-FRAME]', 1)[1].strip(), 'deferred frame')
              for line in log.splitlines() if '[AO-DEFERRED-FRAME]' in line]
    if len(proofs) != requested:
        raise ValueError('deferred proof count mismatch')
    phases, tick_frames, replay_frames = [], 0, 0
    source_geometry = None
    for i, (row, proof) in enumerate(zip(rows, proofs)):
        if set(proof) != pixel.PROOF_FIELDS:
            raise ValueError('deferred proof schema mismatch')
        if (proof['seq'], proof['presentIndex'], proof['captureViTick']) != (i, row['present_index'], row['tick']):
            raise ValueError('deferred proof/image identity mismatch')
        for key in ('seq', 'presentIndex', 'captureViTick', 'viTick', 'previousViTick', 'seal', 'previousSeal',
                    'matrixCount', 'hintOverflow', 'previousHintOverflow', 'interpolationEnabled'):
            pixel._int(proof[key], key, 0)
        alpha = proof['alpha']
        if isinstance(alpha, bool) or not isinstance(alpha, (float, int)) or not math.isfinite(alpha) or not 0 <= alpha <= 1:
            raise ValueError('invalid natural alpha')
        if proof['interpolationEnabled'] != row['replay'] or proof['captureViTick'] != proof['viTick']+1:
            raise ValueError('deferred replay/tick association mismatch')
        if proof['previousViTick'] >= proof['viTick'] or proof['previousSeal'] >= proof['seal']:
            raise ValueError('invalid retained predecessor')
        if proof['hintOverflow'] or proof['previousHintOverflow'] or not proof['matrixCount']:
            raise ValueError('missing/overflowed matrices')
        matrices = [pixel._matrix(value, j) for j, value in enumerate(proof['matrices'])]
        if len(matrices) != proof['matrixCount'] or len({m['index'] for m in matrices}) != len(matrices):
            raise ValueError('matrix inventory mismatch')
        if not row['replay']:
            tick_frames += 1
            if alpha != 0 or any(m['outputBits'] != m['currentBits'] for m in matrices):
                raise ValueError('real-frame matrices were rewritten')
        else:
            replay_frames += 1
            phases.append(alpha)
        image = pixel.load_mfd(frames_path/f'f{i:06d}.mfd')
        if any(image[field] != row[field] for field in pixel.INDEX_FIELDS):
            raise ValueError('MFD/index identity mismatch')
        geometry = tuple(row[key] for key in ('src_width', 'src_height', 'crop_x', 'crop_y', 'crop_w', 'crop_h', 'format'))
        if source_geometry is None: source_geometry = geometry
        if geometry != source_geometry or row['downscale'] != 1:
            raise ValueError('geometry/format changed during burst')
        if i and (row['present_index'] != rows[i-1]['present_index']+1 or
                  row['wall_ns'] <= rows[i-1]['wall_ns'] or row['tick'] < rows[i-1]['tick']):
            raise ValueError('nonconsecutive/nonmonotonic presented frames')
    if tick_frames < 2 or replay_frames < 2 or not any(a not in pixel.FIXED_PHASES for a in phases):
        raise ValueError('capture does not cover real and naturally phased replay frames')
    return dict(frames=requested, real_frames=tick_frames, replay_frames=replay_frames,
                natural_alpha_min=min(phases), natural_alpha_max=max(phases), gpu_copy_bytes=byte_count,
                tick_range=[rows[0]['tick'], rows[-1]['tick']],
                index_sha256=hashlib.sha256((frames_path/'index.csv').read_bytes()).hexdigest(),
                scope='Consecutive pre-overlay source frames with natural alpha and submitted matrix proof; GPU copy, allocation and metadata overhead remain. Not uninstrumented timing, quality acceptance or Android evidence.')
