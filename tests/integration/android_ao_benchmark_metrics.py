"""Pure, fail-closed evidence parsing for a bounded native Android board run."""
import math
import re

try:
    from .engine_benchmark_metrics import gpu_window
except ImportError:
    from engine_benchmark_metrics import gpu_window


AO_COVERAGE_SCOPE = (
    'Named-stage presence only: required and observed rows prove that the named AO stages '
    'were recorded in the final GPU window, not their per-frame execution counts. '
    'Integrated EFB composite and copy-barrier work is not separately attributed, so this '
    'is not a complete isolated AO cost.'
)
AO_SHARED_STAGES = ('AO depth preparation',)
LEGACY_REQUIRED_STAGES = AO_SHARED_STAGES + ('GTAO', 'AO blur X', 'AO blur Y')
XEGTAO_REQUIRED_STAGES = AO_SHARED_STAGES + (
    'XeGTAO canonical depth', 'XeGTAO depth MIPs', 'XeGTAO normals',
    'XeGTAO main', 'XeGTAO final denoise',
)
XEGTAO_CANONICAL_REQUIRED_STAGES = AO_SHARED_STAGES + (
    'XeGTAO depth MIPs', 'XeGTAO normals', 'XeGTAO main',
    'XeGTAO final denoise',
)
LEGACY_STAGES = frozenset(('GTAO', 'AO blur X', 'AO blur Y'))
PREPARATION_MARKER = '[MP6-AO] XeGTAO preparation:'
CANONICAL_PREPARATION_ROW = '[MP6-AO] XeGTAO preparation: canonical'
CANONICAL_PREPARATION_ENV = 'MP6_AO_XEGTAO_PREPARED_CANONICAL'


def _is_named_ao_stage(name):
    """Return whether a GPU timing row is an AO-attributed stage.

    The timing producer uses the ``AO ``/``XeGTAO `` prefixes for both current
    required work and optional adapter/foreground/decal/foliage work.  Keep the
    classifier deliberately name-based: the snapshot does not prove dispatch
    counts, and unknown non-AO rows must remain ordinary scene/presentation
    work.
    """
    return name == 'GTAO' or name.startswith(('AO ', 'XeGTAO '))


def _ao_stage_coverage(pass_names, required):
    """Build deterministic named-stage coverage for the final GPU window."""
    observed = [name for name in pass_names if _is_named_ao_stage(name)]
    missing = [name for name in required if name not in observed]
    return dict(required=list(required), observed=observed, missing=missing,
                scope=AO_COVERAGE_SCOPE)


def benchmark_result(log, request):
    """Validate CPU/GPU execution only; thermal admission remains independent."""
    coverage = _ao_stage_coverage((), ())
    result = dict(valid=False, errors=[], stage='startup', cpu_window=None,
                  gpu_final_window=None, gpu_distribution=None, ao_policy=None,
                  ao_stage_coverage=coverage,
                  measurement_scope='full-native-board-job', thermal_evidence_included=False,
                  scope='Full native board job, not isolated AO kernels. CPU and GPU windows have separate scopes.')
    errors = result['errors']
    if not isinstance(log, str) or not isinstance(request, dict):
        errors.append('log and request must be text and a mapping')
        return result
    lines = log.splitlines()
    if '[EVENT] w01.live=' in log:
        result['stage'] = 'board_warmup'

    def rows(marker):
        return [(index, line[line.index(marker):].strip()) for index, line in enumerate(lines) if marker in line]

    def one(marker, pattern):
        found = rows(marker)
        if len(found) != 1:
            errors.append(f'expected exactly one {marker} record; found {len(found)}')
            return None
        match = re.fullmatch(pattern, found[0][1])
        if not match:
            errors.append('malformed ' + marker + ' record')
            return None
        return found[0][0], match

    evidence = request.get('board_benchmark')
    if not isinstance(evidence, dict):
        errors.append('missing verified board benchmark request')
        evidence = {}
    warmup, count, last = (evidence.get(key) for key in ('warmup_board_age', 'sample_count', 'last_board_age'))
    expected_valid = (type(warmup) is int and 120 <= warmup <= 6000 and
                      type(count) is int and 120 <= count <= 4000 and
                      type(last) is int and last == warmup + count - 1 and last < 10000)
    if not expected_valid:
        errors.append('invalid requested board measurement window')

    begin = one('[ANDROID-AO-BENCH-BEGIN]',
        r'\[ANDROID-AO-BENCH-BEGIN\] first_age=(\d+) target_last_age=(\d+) ticks=(\d+) global_tick=(\d+)')
    end = one('[ANDROID-AO-BENCH]',
        r'\[ANDROID-AO-BENCH\] ticks=(\d+) first_age=(\d+) last_age=(\d+) global_tick=(\d+) '
        r'game=([\d.]+) submit=([\d.]+) seal=([\d.]+) post=([\d.]+) frame=([\d.]+)')
    if begin:
        result['stage'] = 'measurement'
    if begin and end:
        first_age, target_last, begin_count, first_tick = map(int, begin[1].groups())
        end_count, actual_first, actual_last, last_tick = map(int, end[1].groups()[:4])
        try:
            means = dict(zip(('game', 'submit', 'seal', 'post', 'frame'), map(float, end[1].groups()[4:])))
            finite = all(math.isfinite(value) and value >= 0 for value in means.values())
        except ValueError:
            means, finite = {}, False
        if not finite or not means.get('frame', 0):
            errors.append('invalid CPU timing values')
        if begin[0] >= end[0]:
            errors.append('CPU completion does not follow its begin marker')
        if expected_valid and (first_age, target_last, begin_count, end_count, actual_first, actual_last) != (
                warmup, last, count, count, warmup, last):
            errors.append('actual CPU count or board ages differ from the verified request')
        if first_tick < first_age or last_tick - first_tick != end_count - 1:
            errors.append('global tick duration does not match the contiguous CPU window')
        result['cpu_window'] = dict(sample_count=end_count, first_age=actual_first, last_age=actual_last,
            first_global_tick=first_tick, last_global_tick=last_tick, global_tick_delta=last_tick-first_tick,
            average_ms=means, scope='Native game-thread tick interval including pacing; phase slices are not total GPU cost')
    cpu_evidence_valid = bool(begin and end and not errors)

    failure_pattern = (r'\[ANDROID-AO-BENCH-INVALID\]|\[MP6-CRASH\]|\[FATAL(?:\]|:)|'
                       r'\[AURORA FATAL(?:\]|:)|\bFatal signal\b|\[VALIDATION-ERROR\]|'
                       r'\bvalidation (?:error|failed)\b|\bFORTIFY:')
    if re.search(failure_pattern, log, re.IGNORECASE):
        errors.append('invalid benchmark, crash, device loss, or validation failure in log')
    if re.search(r'\[MP6-AO\].*(?:unavailable|using legacy|invalid .*policy)', log, re.IGNORECASE):
        errors.append('AO unavailable or fallback diagnostic in log')

    gpu_evidence_valid = False
    try:
        gpu = gpu_window(log)
        if gpu['dropped_frames'] != 0:
            errors.append('final GPU window contains dropped frames')
        headers = rows('[ENGINE-GPU]')
        if end and (not headers or headers[0][0] <= end[0]):
            errors.append('final GPU window does not follow CPU completion')
        gpu_evidence_valid = bool(end and headers and headers[0][0] > end[0] and gpu['dropped_frames'] == 0)
        cpu_count = (result['cpu_window'] or {}).get('sample_count', count if expected_valid else 'unknown')
        gpu['scope'] = (f"Final {gpu['sample_count']} completed GPU frames; "
                        f'not the {cpu_count}-tick CPU timing window')
        result['gpu_final_window'] = gpu
        # ``gpu_window`` intentionally extracts records from logcat lines and
        # historically used an unanchored regex.  Enforce an exact ordered
        # final-window block here for both timing evidence versions, otherwise
        # a malformed required AO row could be accepted by the v1 path.
        headers = rows('[ENGINE-GPU]')
        pass_rows = rows('[ENGINE-GPU-PASS]')
        if (len(headers) != 1 or not re.fullmatch(
                r'\[ENGINE-GPU\] status=\d+ samples=\d+ dropped=\d+ span=[\d.]+ between=[\d.]+',
                headers[0][1]) or len(pass_rows) != len(gpu['passes']) or
                any(index <= headers[0][0] or not re.fullmatch(
                    r'\[ENGINE-GPU-PASS\] .+ avg=[\d.]+ max=[\d.]+', line)
                    for index, line in pass_rows)):
            errors.append('GPU snapshot statistics are malformed or outside their ordered record block')
    except (ValueError, TypeError, OverflowError) as error:
        errors.append('invalid final GPU window: ' + str(error))

    timing_version = evidence.get('gpu_timing_version', 1)
    if type(timing_version) is not int or timing_version not in (1, 2):
        errors.append('unsupported GPU timing evidence version')
    if timing_version == 2:
        requested = one('[ANDROID-AO-GPU-REQUEST]',
            r'\[ANDROID-AO-GPU-REQUEST\] id=(\d+) global_tick=(\d+)')
        snapshot = one('[ENGINE-GPU-SNAPSHOT]',
            r'\[ENGINE-GPU-SNAPSHOT\] id=(\d+) generation=(\d+) global_tick=(\d+) '
            r'span_median=([\d.]+) span_p95=([\d.]+) span_max=([\d.]+) '
            r'between_median=([\d.]+) between_p95=([\d.]+) between_max=([\d.]+) '
            r'ao_mean=([\d.]+) ao_median=([\d.]+) ao_p95=([\d.]+) ao_max=([\d.]+)')
        if requested and snapshot and result['cpu_window'] and result['gpu_final_window']:
            request_id, request_tick = map(int, requested[1].groups())
            snapshot_id, generation, snapshot_tick = map(int, snapshot[1].groups()[:3])
            gpu = result['gpu_final_window']
            header = rows('[ENGINE-GPU]')
            if (not 0 < request_id < 2**64 or request_id != snapshot_id or
                    not 0 < generation < 2**64 or not generation & 1 or
                    request_tick != result['cpu_window']['last_global_tick'] or
                    not 0 <= snapshot_tick - request_tick < 30):
                errors.append('GPU snapshot request identity, generation or bounded response tick mismatch')
            if not (end[0] < requested[0] < snapshot[0] < header[0][0]):
                errors.append('GPU snapshot is not ordered after its CPU-end request and before its statistics')
            keys = ('span_median_ms', 'span_p95_ms', 'span_max_ms',
                    'between_median_ms', 'between_p95_ms', 'between_max_ms',
                    'ao_mean_ms', 'ao_median_ms', 'ao_p95_ms', 'ao_max_ms')
            try:
                values = dict(zip(keys, map(float, snapshot[1].groups()[3:])))
            except ValueError:
                values = dict.fromkeys(keys, float('nan'))
            if not all(math.isfinite(value) and value >= 0 for value in values.values()):
                errors.append('invalid GPU distribution values')
            else:
                for field, mean in (('span', gpu['span_average_ms']),
                                    ('between', gpu['between_average_ms']),
                                    ('ao', values['ao_mean_ms'])):
                    if (not values[field + '_median_ms'] <= values[field + '_p95_ms'] <= values[field + '_max_ms']
                            or mean > values[field + '_max_ms'] + 0.000001):
                        errors.append('inconsistent GPU distribution: ' + field)
                for measure in ('median', 'p95', 'max'):
                    if values['between_' + measure + '_ms'] > values['span_' + measure + '_ms']:
                        errors.append('GPU between-pass distribution exceeds whole span')
                ao_rows = [value['average_ms'] for name, value in gpu['passes'].items()
                           if name == 'GTAO' or name.startswith(('AO ', 'XeGTAO '))]
                # Only means are additive. Median/p95 must come from the
                # per-frame sum retained by the worker, never pass percentiles.
                # Pass intervals may overlap: this sum is not bounded by the
                # first-to-last span, unlike the uncovered between-pass time.
                if abs(sum(ao_rows) - values['ao_mean_ms']) > (len(ao_rows) + 1) * 0.000001:
                    errors.append('timestamped AO mean disagrees with the same snapshot pass inventory')
            if all(math.isfinite(value) for value in values.values()):
                result['gpu_distribution'] = dict(request_id=request_id, generation=generation,
                    request_global_tick=request_tick, snapshot_global_tick=snapshot_tick,
                    sample_count=gpu['sample_count'], **values,
                    scope='Same final 120 completed GPU frames as means; AO is the per-frame sum of named timestamped AO passes, whose intervals may overlap, excluding integrated EFB composite and copy/barrier residuals. Whole span is first-to-last GPU timestamps, not wall time.')
    elif rows('[ANDROID-AO-GPU-REQUEST]') or rows('[ENGINE-GPU-SNAPSHOT]'):
        errors.append('GPU distribution records require attested timing version 2')

    # Native teardown explicitly destroys the GPU device. Admit that one exact
    # warning only after the completed measurement and its attested +60-tick
    # exit request. A BOOT line is not proof that shutdown finished safely:
    # fatal/validation/FORTIFY records anywhere in the log remain failures.
    shutdown_rows = rows('[BOOT] reached ')
    shutdown = None
    if len(shutdown_rows) == 1:
        match = re.fullmatch(r'\[BOOT\] reached (\d+) VIWaitForRetrace ticks \(limit (\d+)\) '
                             r'-- shutting down Aurora, exiting 0', shutdown_rows[0][1])
        gpu_records = rows('[ENGINE-GPU]') + rows('[ENGINE-GPU-PASS]')
        if (match and cpu_evidence_valid and gpu_evidence_valid and gpu_records and
                int(match[1]) == int(match[2]) == result['cpu_window']['last_global_tick'] + 60 and
                shutdown_rows[0][0] > max(index for index, _ in gpu_records)):
            shutdown = shutdown_rows[0][0]
    result['shutdown'] = dict(requested_clean_shutdown=shutdown is not None,
                              expected_device_destruction_count=0)
    expected_warning = '[AURORA WARNING: aurora::gpu] Device lost: Device was destroyed.'
    for index, line in enumerate(lines):
        if not re.search(r'\bdevice lost\b', line, re.IGNORECASE):
            continue
        warning = line[line.index('[AURORA WARNING:'):] if '[AURORA WARNING:' in line else line
        if shutdown is not None and index > shutdown and warning.strip() == expected_warning:
            result['shutdown']['expected_device_destruction_count'] += 1
        else:
            errors.append('unexpected device loss in log')

    env = {}
    tokens = request.get('args', [])
    if not isinstance(tokens, list) or any(not isinstance(token, str) for token in tokens):
        errors.append('invalid launch argument evidence')
        tokens = []
    for token in tokens:
        # The prepared-canonical selector is intentionally opt-in only.  Keep
        # malformed spellings visible instead of silently treating them as an
        # unrelated launch token.
        if (token.startswith(CANONICAL_PREPARATION_ENV)
                and not token.startswith(CANONICAL_PREPARATION_ENV + '=')):
            errors.append('malformed launch setting: ' + CANONICAL_PREPARATION_ENV)
        if token.startswith('MP6_') and '=' in token:
            key, value = token.split('=', 1)
            if key in env:
                errors.append('duplicate launch setting: ' + key)
            env[key] = value
    level = env.get('MP6_ENH_AMBIENT_OCCLUSION')
    implementation = request.get('requested_ao_implementation')
    if level not in ('0', '1', '2') or implementation not in ('legacy', 'xegtao'):
        errors.append('missing or invalid requested AO implementation/level')
    if env.get('MP6_AO_XEGTAO') != ('1' if implementation == 'xegtao' else '0'):
        errors.append('implementation request disagrees with launch environment')
    if env.get('MP6_PROFILE_SKIP_AO_COMPOSITE') == '1' or request.get('diagnostic'):
        errors.append('ablation or diagnostic request cannot establish full-job AO cost')

    prepared_requested = request.get('requested_xegtao_prepared_canonical', False)
    if 'requested_xegtao_prepared_canonical' in request and type(prepared_requested) is not bool:
        errors.append('invalid requested prepared-canonical selector')
        prepared_requested = False
    prepared_env = env.get(CANONICAL_PREPARATION_ENV)
    if prepared_env is not None and prepared_env != '1':
        errors.append('prepared-canonical launch environment must be exactly 1')
    if prepared_requested:
        if implementation != 'xegtao' or level == '0':
            errors.append('prepared-canonical request requires active XeGTAO')
        if prepared_env != '1':
            errors.append('prepared-canonical request is missing explicit launch environment')
    elif prepared_env is not None:
        errors.append('prepared-canonical environment requires explicit request')

    preparation_rows = rows(PREPARATION_MARKER)
    preparation_indices = {index for index, _ in preparation_rows}
    canonical_preparation_rows = [line for _, line in preparation_rows
                                  if line == CANONICAL_PREPARATION_ROW]
    if any(line != CANONICAL_PREPARATION_ROW for _, line in preparation_rows):
        errors.append('malformed or unknown XeGTAO preparation attestation')
    if prepared_requested:
        if canonical_preparation_rows != [CANONICAL_PREPARATION_ROW]:
            errors.append('prepared-canonical request requires exactly one native canonical preparation row')
        gpu_headers = rows('[ENGINE-GPU]')
        if (len(canonical_preparation_rows) == 1 and len(preparation_rows) == 1
                and (len(gpu_headers) != 1 or preparation_rows[0][0] >= gpu_headers[0][0])):
            errors.append('canonical preparation attestation must precede the final GPU window')
    elif preparation_rows:
        errors.append('native canonical preparation row is forbidden without explicit opt-in')

    xegtao_rows = rows('[MP6-AO] XeGTAO')
    storage_rows = rows('[MP6-AO] XeGTAO depth storage:')
    # Depth format and effective quality are separate native records. Unknown
    # XeGTAO records stay in the policy inventory and still fail closed. The
    # preparation attestation is a separate native record, not a policy line.
    storage_indices = {index for index, _ in storage_rows}
    policy_rows = [(index, line) for index, line in xegtao_rows
                   if index not in storage_indices and index not in preparation_indices]
    path_rows = rows('[MP6-AO] shader/scheduling path:')
    pass_names = list((result['gpu_final_window'] or {}).get('passes', {}))
    passes = set(pass_names)
    xegtao_passes = {name for name in passes if name.startswith('XeGTAO ')}
    ao_passes = {name for name in passes if _is_named_ao_stage(name)}
    legacy_passes = passes & LEGACY_STAGES
    policy = request.get('requested_xegtao_policy')
    # Missing resolution belongs to historical full-resolution runs. A half
    # request needs both an explicit launch argument and effective native proof.
    resolution = policy.get('resolution', 'full') if isinstance(policy, dict) else 'full'
    if resolution not in ('full', 'half'):
        errors.append('invalid requested XeGTAO resolution policy')
    if prepared_requested and resolution != 'full':
        errors.append('prepared-canonical request requires full-resolution XeGTAO')
    if env.get('MP6_AO_XEGTAO_RESOLUTION', 'full') != resolution:
        errors.append('XeGTAO policy disagrees with launch environment: MP6_AO_XEGTAO_RESOLUTION')
    if implementation != 'xegtao' and resolution != 'full':
        errors.append('nondefault XeGTAO resolution requires the XeGTAO implementation')
    storage = policy.get('depth_storage', 'r32float') if isinstance(policy, dict) else 'r32float'
    if storage not in ('r32float', 'r16float'):
        errors.append('invalid requested XeGTAO depth storage policy')
    if env.get('MP6_AO_XEGTAO_DEPTH_STORAGE', 'r32float') != storage:
        errors.append('XeGTAO policy disagrees with launch environment: MP6_AO_XEGTAO_DEPTH_STORAGE')
    if implementation != 'xegtao' and storage != 'r32float':
        errors.append('nondefault XeGTAO depth storage requires the XeGTAO implementation')
    result['ao_policy'] = dict(implementation=implementation, ao_level=int(level) if level in ('0', '1', '2') else None,
                               prepared_canonical=prepared_requested,
                               resolution=resolution, effective_resolution=None,
                               depth_storage=storage, effective_depth_storage=None,
                               shader_scheduling_path=None)
    if path_rows:
        path = (re.fullmatch(r'\[MP6-AO\] shader/scheduling path: (optimized|compatibility)', path_rows[0][1])
                if len(path_rows) == 1 else None)
        if path is None:
            errors.append('malformed or ambiguous native AO shader/scheduling path')
        else:
            # This path is chosen by the renderer/device, not a request option.
            # Absence in a historical log stays unverified, never inferred.
            result['ao_policy']['shader_scheduling_path'] = path[1]
    required_stages = ()
    if level == '0':
        required_stages = ()
    elif implementation == 'legacy':
        required_stages = LEGACY_REQUIRED_STAGES
    elif implementation == 'xegtao':
        denoise_for_coverage = policy.get('denoise_passes') if isinstance(policy, dict) else None
        required_stages = (XEGTAO_CANONICAL_REQUIRED_STAGES if prepared_requested
                           else XEGTAO_REQUIRED_STAGES)
        if type(denoise_for_coverage) is int and denoise_for_coverage > 1:
            required_stages += ('XeGTAO denoise',)
    coverage = _ao_stage_coverage(pass_names, required_stages)
    result['ao_stage_coverage'] = coverage
    if coverage['missing']:
        errors.append('missing mandatory AO GPU stage(s): ' + ', '.join(coverage['missing']))
    if level == '0':
        if xegtao_rows or path_rows or ao_passes:
            errors.append('AO-off request contains native AO execution evidence')
    elif implementation == 'legacy':
        if xegtao_rows or xegtao_passes:
            errors.append('legacy request executed XeGTAO')
    elif implementation == 'xegtao':
        quality = policy.get('quality') if isinstance(policy, dict) else None
        denoise = policy.get('denoise_passes') if isinstance(policy, dict) else None
        result['ao_policy'].update(quality=quality, denoise_passes=denoise)
        if quality not in ('low', 'medium', 'high', 'ultra') or type(denoise) is not int or denoise not in (1, 2, 3):
            errors.append('invalid requested XeGTAO quality/denoise policy')
        for key, value in (('MP6_AO_XEGTAO_QUALITY', quality), ('MP6_AO_XEGTAO_DENOISE', str(denoise))):
            if key in env and env[key] != value:
                errors.append('XeGTAO policy disagrees with launch environment: ' + key)
        actual_storage = None
        if storage_rows:
            observed_storage = (re.fullmatch(
                r'\[MP6-AO\] XeGTAO depth storage: requested=(r32float|r16float) actual=(r32float|r16float)',
                storage_rows[0][1]) if len(storage_rows) == 1 else None)
            if observed_storage is None:
                errors.append('malformed or ambiguous native XeGTAO depth storage')
            else:
                logged_request, actual_storage = observed_storage.groups()
                result['ao_policy']['effective_depth_storage'] = actual_storage
                if logged_request != storage or actual_storage != storage:
                    errors.append('native effective XeGTAO depth storage differs from the request')
        elif storage != 'r32float':
            errors.append('nondefault XeGTAO depth storage requires native storage evidence')
        observed = None
        if len(policy_rows) == 1:
            observed = re.fullmatch(r'\[MP6-AO\] XeGTAO (reference|experimental): '
                                   r'(full-resolution direct|half-resolution bilateral) uint output, '
                                   r'(Low|Medium|High|Ultra), ([123]) spatial denoise (pass|passes)(; opt-in only)?', policy_rows[0][1])
        if observed is None:
            errors.append('missing or ambiguous native effective XeGTAO policy')
        else:
            mode, actual_output, actual_quality, actual_denoise, plural, historical_suffix = observed.groups()
            actual_resolution = actual_output.split('-')[0]
            result['ao_policy']['effective_resolution'] = actual_resolution
            if historical_suffix is None and actual_storage is None:
                errors.append('current native XeGTAO policy requires explicit depth storage evidence')
            expected_mode = ('reference' if resolution == 'full' and quality == 'high' and denoise == 2
                             and (actual_storage or storage) == 'r32float'
                             else 'experimental')
            if ((actual_quality.lower(), int(actual_denoise), actual_resolution, mode, plural) !=
                    (quality, denoise, resolution, expected_mode, 'pass' if denoise == 1 else 'passes')):
                errors.append('native effective XeGTAO policy differs from the request')
        if legacy_passes:
            errors.append('XeGTAO request executed legacy GTAO/blur stages')
        if prepared_requested and 'XeGTAO canonical depth' in passes:
            errors.append('prepared-canonical request executed the old canonical-depth raster stage')
        # The native encoder labels every non-final denoise dispatch
        # "XeGTAO denoise", while the GPU timing snapshot may aggregate
        # repeated labels into one row. Presence is therefore the stable
        # evidence: every policy with more than one pass needs the shared
        # intermediate row, and a one-pass policy must not contain it.
        requires_intermediate = type(denoise) is int and denoise > 1
        if ('XeGTAO denoise' in passes) != requires_intermediate:
            errors.append('native XeGTAO denoise pass inventory differs from the request')

    if expected_valid:
        result['scope'] = (f'Full native board job: {count} CPU ticks at board ages {warmup}..{last}; '
                           'final 120 completed GPU frames only. Includes scene rendering and AO composition; '
                           'GPU span is first-to-last GPU timestamps, not app wall time or isolated AO cost. '
                           'Thermal validity must be checked separately.')
    result['valid'] = not errors
    if result['valid']:
        result['stage'] = 'complete'
    elif end:
        result['stage'] = 'rejected'
    return result
