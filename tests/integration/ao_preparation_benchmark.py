"""Bounded CPU/GPU evidence helpers for the prepared-canonical AO matrix.

This module deliberately keeps the matrix's accounting separate from the
older generic benchmark parser.  The matrix compares one current-frame
executable across three native depth paths; it is not an Android benchmark,
an FPS claim, or a complete isolated AO-cost measurement.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import sys


MATRIX_SCHEDULE = ("off", "viewz", "canonical", "canonical", "viewz", "off")
MATRIX_CONDITIONS = ("off", "viewz", "canonical")
DEFAULT_MATRIX_REPETITIONS = 2

_ENGINE_BENCH_RE = re.compile(
    r"\[ENGINE-BENCH\] ticks=4000 game=(?P<game>[0-9]+(?:\.[0-9]+)?) "
    r"submit=(?P<submit>[0-9]+(?:\.[0-9]+)?) "
    r"seal=(?P<seal>[0-9]+(?:\.[0-9]+)?) "
    r"post=(?P<post>[0-9]+(?:\.[0-9]+)?) "
    r"frame=(?P<frame>[0-9]+(?:\.[0-9]+)?)"
)
_ENGINE_GPU_RE = re.compile(
    r"\[ENGINE-GPU\] status=(?P<status>[0-9]+) "
    r"samples=(?P<samples>[0-9]+) dropped=(?P<dropped>[0-9]+) "
    r"span=(?P<span>[0-9]+(?:\.[0-9]+)?) "
    r"between=(?P<between>[0-9]+(?:\.[0-9]+)?)"
)
_ENGINE_GPU_PASS_RE = re.compile(
    r"\[ENGINE-GPU-PASS\] (?P<name>.+) "
    r"avg=(?P<average>[0-9]+(?:\.[0-9]+)?) "
    r"max=(?P<maximum>[0-9]+(?:\.[0-9]+)?)"
)

# The prepared-canonical matrix is admitted only when the native renderer
# proves the policy it was asked to run.  Keep these records separate from
# the generic Android parser: this matrix requires the current full/reference
# policy on every view-Z/canonical run, including the unmeasured warmups.
_REQUESTED_IMPLEMENTATION_RE = re.compile(
    r"\[MP6-AO\] requested implementation: (?P<implementation>XeGTAO) "
    r"\((?P<selection>environment override|platform default)\)"
)
_SCHEDULING_PATH_RE = re.compile(
    r"\[MP6-AO\] shader/scheduling path: (?P<path>optimized|compatibility)"
)
_DEPTH_STORAGE_RE = re.compile(
    r"\[MP6-AO\] XeGTAO depth storage: requested=(?P<requested>r32float|r16float) "
    r"actual=(?P<actual>r32float|r16float)"
)
_EFFECTIVE_POLICY_RE = re.compile(
    r"\[MP6-AO\] XeGTAO (?P<mode>reference|experimental): "
    r"(?P<output>full-resolution direct|half-resolution bilateral) uint output, "
    r"(?P<quality>Low|Medium|High|Ultra), "
    r"(?P<denoise>[123]) spatial denoise (?P<plural>pass|passes)"
)
_EXPECTED_AURORA_CARD_MISS = (
    "[AURORA ERROR: aurora::card] Failed to open file: MARIPA6"
)

_PREPARED_ROW = "[MP6-AO] XeGTAO preparation: canonical"
_PREPARED_MARKER = "[MP6-AO] XeGTAO preparation:"
_FALLBACK_MARKERS = (
    "[MP6-CRASH]",
    "[FATAL]",
    "[AURORA FATAL",
    "[VALIDATION-ERROR]",
    "GPUValidationError",
    "uncaptured error",
    "using legacy",
    "xegtao unavailable:",
    " fallback",
)

CURRENT_XE_STAGES = (
    "AO depth preparation",
    "XeGTAO depth MIPs",
    "XeGTAO normals",
    "XeGTAO main",
    "XeGTAO denoise",
    "XeGTAO final denoise",
)
VIEWZ_EXTRA_STAGES = ("XeGTAO canonical depth",)
LEGACY_STAGE_NAMES = frozenset(("GTAO", "AO blur X", "AO blur Y"))

CPU_METRIC_FIELDS = (
    "frame_ms",
    "game_ms",
    "submit_ms",
    "worker_encode_ms",
    "ao_encode_ms",
    "ao_composite_record_ms",
    "ao_foliage_seed_record_ms",
)


def matrix_schedule(repetitions=DEFAULT_MATRIX_REPETITIONS):
    """Return the fixed symmetric schedule repeated the requested number of times."""

    if type(repetitions) is not int or repetitions < 1:
        raise ValueError("matrix repetitions must be a positive integer")
    return MATRIX_SCHEDULE * repetitions


def condition_request(condition):
    """Return the explicit board-run selector for one matrix condition."""

    if condition == "off":
        return dict(ao=0, implementation="legacy", prepared_canonical=False)
    if condition == "viewz":
        return dict(ao=2, implementation="xegtao", prepared_canonical=False)
    if condition == "canonical":
        return dict(ao=2, implementation="xegtao", prepared_canonical=True)
    raise ValueError(f"unknown prepared-canonical matrix condition: {condition}")


def _finite_nonnegative(value, label):
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"invalid {label}: {value!r}")
    return value


def parse_engine_benchmark(log):
    """Parse exactly one completed 4,000-frame CPU benchmark row."""

    rows = []
    for line in log.splitlines():
        if "[ENGINE-BENCH]" in line:
            match = _ENGINE_BENCH_RE.fullmatch(line)
            if match is None:
                raise ValueError("malformed ENGINE-BENCH row")
            rows.append(match)
    if len(rows) != 1:
        raise ValueError(f"expected one ENGINE-BENCH row, found {len(rows)}")
    values = {key: _finite_nonnegative(float(value), f"ENGINE-BENCH {key}")
              for key, value in rows[0].groupdict().items()}
    return dict(samples=4000, **values,
                scope="4000 contiguous board-age CPU frames; phase slices overlap and are not a total AO cost")


def parse_engine_gpu_block(log):
    """Parse the strict, completed ENGINE GPU block emitted by frame profiling.

    Every header/pass line is matched as a complete line.  This prevents an
    embedded or suffixed diagnostic line from being mistaken for timing
    evidence.  The returned named-stage sum is intentionally labelled as a
    partial attribution: it excludes integrated composite/copy/barrier work.
    """

    lines = log.splitlines()
    headers = []
    pass_rows = []
    for index, line in enumerate(lines):
        if "[ENGINE-GPU]" in line:
            match = _ENGINE_GPU_RE.fullmatch(line)
            if match is None:
                raise ValueError("malformed ENGINE-GPU header")
            headers.append((index, match))
        if "[ENGINE-GPU-PASS]" in line:
            match = _ENGINE_GPU_PASS_RE.fullmatch(line)
            if match is None:
                raise ValueError("malformed ENGINE-GPU-PASS row")
            pass_rows.append((index, match))
    if len(headers) != 1:
        raise ValueError(f"expected one ENGINE-GPU header, found {len(headers)}")
    header_index, header = headers[0]
    if not pass_rows or any(index <= header_index for index, _ in pass_rows):
        raise ValueError("ENGINE-GPU-PASS rows must follow the ENGINE-GPU header")

    status = int(header["status"])
    samples = int(header["samples"])
    dropped = int(header["dropped"])
    span = _finite_nonnegative(float(header["span"]), "GPU span")
    between = _finite_nonnegative(float(header["between"]), "GPU between")
    if status != 2 or samples != 120 or dropped != 0:
        raise ValueError(
            f"incomplete ENGINE GPU window: status={status} samples={samples} dropped={dropped}"
        )
    if between > span:
        raise ValueError("GPU between-pass time exceeds GPU span")

    passes = {}
    for _, row in pass_rows:
        name = row["name"]
        if name in passes:
            raise ValueError(f"duplicate ENGINE-GPU-PASS row: {name}")
        average = _finite_nonnegative(float(row["average"]), f"GPU pass {name} average")
        maximum = _finite_nonnegative(float(row["maximum"]), f"GPU pass {name} maximum")
        if maximum < average:
            raise ValueError(f"GPU pass maximum precedes average: {name}")
        passes[name] = dict(average_ms=average, max_ms=maximum)
    return dict(status=status, sample_count=samples, dropped_frames=dropped,
                span_average_ms=span, between_average_ms=between, passes=passes,
                scope="Final 120 completed GPU frames; not the 4000-frame CPU timing window")


def _named_stage_names(passes):
    return tuple(name for name in passes
                 if name == "GTAO" or name.startswith(("AO ", "XeGTAO ")))


def validate_failure_markers(log):
    """Reject fatal, validation, and native AO fallback diagnostics."""

    lowered = log.casefold()
    for marker in _FALLBACK_MARKERS:
        if marker.casefold() in lowered:
            raise ValueError(f"AO run contains forbidden failure/fallback marker: {marker}")
    if re.search(r"validation\s+(?:error|failed|failure)", lowered):
        raise ValueError("AO run contains a validation failure marker")
    # A fresh isolated board is allowed to report the exact missing save/card
    # record used by the existing board and Android QA gates.  Every other
    # AURORA ERROR is a runtime failure, including errors from the card and
    # save subsystems with a different path or message.
    for line in log.splitlines():
        if "[aurora error" in line.casefold() and line.strip() != _EXPECTED_AURORA_CARD_MISS:
            raise ValueError("AO run contains an AURORA ERROR: " + line.strip())


def _validate_off_native_evidence(log):
    """Reject AO implementation, policy, and execution evidence for Off."""

    lines = log.splitlines()
    native_rows = [line for line in lines if line.startswith("[MP6-AO] ")]
    if native_rows:
        raise ValueError(
            "AO-off run contains native AO evidence: " + native_rows[0].strip()
        )
    # Warmups do not have an admitted ENGINE-GPU block, so also reject native
    # stage diagnostics emitted by the lower-level GPU window logger.
    gpu_native = [
        line for line in lines
        if line.startswith("[MP6-GPU")
        and re.search(r'(?i)(?:^|[\s="_-])(xegtao|gtao|ao)(?:[\s_"=-]|$)', line)
    ]
    if gpu_native:
        raise ValueError(
            "AO-off run contains native AO GPU evidence: " + gpu_native[0].strip()
        )


def parse_native_policy(log, condition):
    """Parse and strictly admit the native policy for one matrix run.

    ``viewz`` and ``canonical`` each need one requested implementation row,
    one scheduling-path row, one requested/actual R32 storage row, and one
    effective full/reference High/2 policy row.  All four records must precede
    the final ENGINE-GPU header.  The returned mapping is intentionally kept
    on each report row so later review cannot infer policy from CLI arguments.
    """

    if condition not in ("viewz", "canonical"):
        raise ValueError("native policy is only valid for viewz/canonical conditions")
    lines = log.splitlines()
    header_indices = [index for index, line in enumerate(lines)
                      if _ENGINE_GPU_RE.fullmatch(line)]
    if len(header_indices) != 1:
        raise ValueError("native policy requires exactly one ENGINE-GPU header")
    header_index = header_indices[0]

    requested_rows = [
        (index, line) for index, line in enumerate(lines)
        if line.startswith("[MP6-AO] requested implementation:")
    ]
    path_rows = [
        (index, line) for index, line in enumerate(lines)
        if line.startswith("[MP6-AO] shader/scheduling path:")
    ]
    storage_rows = [
        (index, line) for index, line in enumerate(lines)
        if line.startswith("[MP6-AO] XeGTAO depth storage:")
    ]
    effective_rows = [
        (index, line) for index, line in enumerate(lines)
        if line.startswith("[MP6-AO] XeGTAO reference:")
        or line.startswith("[MP6-AO] XeGTAO experimental:")
    ]
    if len(requested_rows) != 1:
        raise ValueError("native policy requires exactly one requested implementation row")
    if len(path_rows) != 1:
        raise ValueError("native policy requires exactly one shader/scheduling path row")
    if len(storage_rows) != 1:
        raise ValueError("native policy requires exactly one depth-storage row")
    if len(effective_rows) != 1:
        raise ValueError("native policy requires exactly one effective XeGTAO policy row")

    # Every native policy record must precede the final GPU timing header.  A
    # late duplicate is not execution evidence for the measured window.
    records = requested_rows + path_rows + storage_rows + effective_rows
    if any(index >= header_index for index, _ in records):
        raise ValueError("native policy records must precede the ENGINE-GPU header")

    requested = _REQUESTED_IMPLEMENTATION_RE.fullmatch(requested_rows[0][1])
    path = _SCHEDULING_PATH_RE.fullmatch(path_rows[0][1])
    storage = _DEPTH_STORAGE_RE.fullmatch(storage_rows[0][1])
    effective = _EFFECTIVE_POLICY_RE.fullmatch(effective_rows[0][1])
    if requested is None:
        raise ValueError("malformed requested native implementation row")
    if path is None:
        raise ValueError("malformed native shader/scheduling path row")
    if storage is None:
        raise ValueError("malformed native depth-storage row")
    if effective is None:
        raise ValueError("malformed native effective XeGTAO policy row")

    # Unknown XeGTAO records are not silently ignored.  The preparation row
    # is handled by validate_stage_gate; storage and effective rows are parsed
    # above.  Any other native XeGTAO row could hide fallback/alternate policy.
    known_xe_rows = {storage_rows[0][1], effective_rows[0][1], _PREPARED_ROW}
    unknown_xe_rows = [
        line for line in lines
        if line.startswith("[MP6-AO] XeGTAO") and line not in known_xe_rows
    ]
    if unknown_xe_rows:
        raise ValueError("unknown native XeGTAO policy row: " + unknown_xe_rows[0])

    requested_depth = storage["requested"]
    actual_depth = storage["actual"]
    if requested_depth != "r32float" or actual_depth != "r32float":
        raise ValueError("prepared-canonical matrix requires requested/actual R32 storage")
    mode = effective["mode"]
    output = effective["output"]
    quality = effective["quality"].lower()
    denoise = int(effective["denoise"])
    resolution = "full" if output == "full-resolution direct" else "half"
    plural = effective["plural"]
    if ((mode, resolution, quality, denoise, plural)
            != ("reference", "full", "high", 2, "passes")):
        raise ValueError(
            "native effective policy must be reference full-resolution High/2"
        )

    return {
        "requested_implementation": requested["implementation"],
        "selection": requested["selection"],
        "shader_scheduling_path": path["path"],
        "requested_depth_storage": requested_depth,
        "actual_depth_storage": actual_depth,
        "effective_mode": mode,
        "effective_output": output,
        "effective_resolution": resolution,
        "effective_quality": quality,
        "effective_denoise_passes": denoise,
        "requested_row": requested_rows[0][1],
        "path_row": path_rows[0][1],
        "storage_row": storage_rows[0][1],
        "effective_row": effective_rows[0][1],
        "record_indices": {
            "requested": requested_rows[0][0],
            "path": path_rows[0][0],
            "storage": storage_rows[0][0],
            "effective": effective_rows[0][0],
            "engine_gpu": header_index,
        },
    }


def validate_native_evidence(log, condition):
    """Validate execution policy for both warmups and measured runs."""
    if condition not in MATRIX_CONDITIONS:
        raise ValueError(f"unknown prepared-canonical matrix condition: {condition}")
    lines = log.splitlines()
    prep_rows = [line for line in lines if _PREPARED_MARKER in line]
    if condition == "canonical":
        if prep_rows != [_PREPARED_ROW]:
            raise ValueError("canonical run requires exactly one native canonical preparation row")
        header_indices = [index for index, line in enumerate(lines)
                          if _ENGINE_GPU_RE.fullmatch(line)]
        prep_indices = [index for index, line in enumerate(lines)
                        if line == _PREPARED_ROW]
        if len(header_indices) != 1 or len(prep_indices) != 1 or prep_indices[0] >= header_indices[0]:
            raise ValueError("canonical preparation row must precede the ENGINE-GPU timing block")
    elif prep_rows:
        raise ValueError("native canonical preparation row is forbidden for noncanonical run")

    if condition == "off":
        _validate_off_native_evidence(log)

    # The stage inventory catches pass-level fallback; these markers catch
    # initialization, validation, or legacy fallback before a pass is emitted.
    validate_failure_markers(log)
    return None if condition == "off" else parse_native_policy(log, condition)


def validate_stage_gate(condition, passes, log):
    """Validate the native stage inventory for one matrix condition."""
    effective_policy = validate_native_evidence(log, condition)
    names = set(passes)
    observed = _named_stage_names(passes)
    required = ()
    if condition == "off":
        if observed:
            raise ValueError("AO-off run contains named AO GPU stages: " + ", ".join(observed))
    else:
        required = CURRENT_XE_STAGES + (VIEWZ_EXTRA_STAGES if condition == "viewz" else ())
        missing = [name for name in required if name not in names]
        if missing:
            raise ValueError(condition + " run is missing AO GPU stage(s): " + ", ".join(missing))
        if names & LEGACY_STAGE_NAMES:
            raise ValueError(condition + " run contains legacy GTAO/blur fallback stages")
        if condition == "canonical" and "XeGTAO canonical depth" in names:
            raise ValueError("canonical run contains a separate canonical-depth stage")
    return dict(required=(list(CURRENT_XE_STAGES + VIEWZ_EXTRA_STAGES)
                          if condition == "viewz" else
                          list(CURRENT_XE_STAGES) if condition == "canonical" else []),
                observed=list(observed), missing=[], effective_policy=effective_policy,
                scope="Named-stage presence only; not a complete isolated AO cost")


def _parse_frame_profile(log):
    # Keep the established parser as the source of truth for the complete
    # front-end/work inventory while enforcing the matrix-specific fields.
    try:
        from engine_benchmark_metrics import frame_profile
    except ImportError:  # pragma: no cover - direct package invocation fallback
        from .engine_benchmark_metrics import frame_profile
    profile = frame_profile(log)
    record_scope = [line for line in log.splitlines()
                    if line.startswith("[FRAME-AO-RECORD-SCOPE]")]
    if record_scope != ["[FRAME-AO-RECORD-SCOPE] callbacks=complete"]:
        raise ValueError("matrix requires complete AO callback-record scope evidence")
    cpu = profile["cpu_average_ms"]
    required = ("encode", "aoEncode", "aoCompositeRecord", "aoFoliageSeedRecord")
    missing = [field for field in required if field not in cpu]
    if missing:
        raise ValueError("frame profile is missing matrix CPU fields: " + ", ".join(missing))
    return profile


def parse_matrix_run(log, condition):
    """Validate one measured run and return separately-scoped metrics."""

    bench = parse_engine_benchmark(log)
    gpu = parse_engine_gpu_block(log)
    profile = _parse_frame_profile(log)
    stage_gate = validate_stage_gate(condition, gpu["passes"], log)
    cpu = profile["cpu_average_ms"]
    named_sum = sum(gpu["passes"][name]["average_ms"]
                    for name in _named_stage_names(gpu["passes"]))
    row = dict(
        condition=condition,
        effective_policy=stage_gate["effective_policy"],
        cpu_frame_ms=bench["frame"],
        game_ms=bench["game"],
        submit_ms=bench["submit"],
        worker_encode_ms=cpu["encode"],
        ao_encode_ms=cpu["aoEncode"],
        ao_composite_record_ms=cpu["aoCompositeRecord"],
        ao_foliage_seed_record_ms=cpu["aoFoliageSeedRecord"],
        gpu_span_ms=gpu["span_average_ms"],
        gpu_between_ms=gpu["between_average_ms"],
        named_ao_stage_sum_ms=named_sum,
        named_ao_stage_scope=(
            "Sum of named AO timestamp rows only; excludes integrated EFB composite, "
            "copies, barriers and foliage seeding, so it is not complete AO cost"
        ),
        cpu_scope=bench["scope"],
        gpu_scope=gpu["scope"],
        frame_profile_scope=profile["scope"],
        stage_gate=stage_gate,
        gpu=gpu,
        frame_profile=profile,
    )
    # Preserve the original benchmark names alongside the explicit names used
    # by the matrix report for downstream readers.
    row.update(game=bench["game"], submit=bench["submit"], frame=bench["frame"])
    return row


def read_board_result(run_directory):
    """Read and validate the board-run provenance needed by the matrix."""

    path = Path(run_directory) / "result.json"
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"missing or malformed board result: {path}") from error
    digest = report.get("sha256")
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-fA-F]{64}", digest) is None:
        raise RuntimeError(f"board result has no exact executable hash: {path}")
    if report.get("exit_code") != 0 or report.get("reason") != "exited":
        raise RuntimeError(f"board run did not complete cleanly: {path}")
    if report.get("crash_marker") or report.get("capture_count") != 0:
        raise RuntimeError(f"board run contains a crash or capture: {path}")
    return dict(sha256=digest.lower(), exit_code=0, reason="exited",
                capture_count=0, report_path=str(path))


def aggregate_medians(rows, repetitions=DEFAULT_MATRIX_REPETITIONS):
    """Aggregate repeated rows without turning them into FPS/speedup claims."""

    if type(repetitions) is not int or repetitions < 1:
        raise ValueError("matrix repetitions must be a positive integer")
    medians = {}
    for condition in MATRIX_CONDITIONS:
        condition_rows = [row for row in rows if row.get("condition") == condition]
        if len(condition_rows) != len(MATRIX_SCHEDULE) // len(MATRIX_CONDITIONS) * repetitions:
            # The schedule has two occurrences of off/viewz/canonical per
            # repetition.  Count exact rows so a partial run cannot look valid.
            raise ValueError(f"incomplete repeated rows for condition {condition}")
        medians[condition] = {
            field: statistics.median(row[field] for row in condition_rows)
            for field in (
                "cpu_frame_ms", "game_ms", "submit_ms", "worker_encode_ms",
                "ao_encode_ms", "ao_composite_record_ms", "ao_foliage_seed_record_ms",
                "gpu_span_ms", "gpu_between_ms", "named_ao_stage_sum_ms",
            )
        }
    return medians


def matrix_scope():
    return {
        "device": "Windows PC; no Android measurement",
        "cpu": "4000 contiguous board-age frames from one current-frame executable",
        "gpu": "Final 120 completed GPU frames with status=2, samples=120, dropped=0",
        "named_ao_stage_sum": "Partial named timestamp attribution only; not complete AO cost",
        "comparison": "Repeated medians are diagnostic whole-job evidence, not FPS or mobile claims",
    }


def _write_checkpoint(path, payload):
    """Write the resumable partial report owned by this matrix invocation."""

    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def _board_command(root, executable, name, condition, args, cache_seed=None, timed=False):
    request = condition_request(condition)
    command = [
        sys.executable,
        "tests/integration/run_board_qa.py",
        "--exe", str(executable),
        "--name", name,
        "--seconds", "90",
        "--widescreen",
        "--window-size", "1920x1080",
        "--ao", str(request["ao"]),
        "--implementation", request["implementation"],
        "--xegtao-quality", "high",
        "--xegtao-denoise", "2",
        "--xegtao-resolution", "full",
        "--xegtao-depth-storage", "r32float",
        "--aa", "0",
        "--ticks", str(args.startup_ticks),
        "--stop-round", "0",
        "--input-script",
        f"period:30;timeout:{args.startup_ticks - 1000};pressuntil:a/w01.live/120;wait:18000",
    ]
    if cache_seed is not None:
        command.extend(("--cache-seed", str(cache_seed)))
    if request["prepared_canonical"]:
        # This is the only condition allowed to carry the explicit opt-in.
        command.append("--xegtao-prepared-canonical")
    if timed:
        command.append("--gpu-timings")
    return command


def _run_board(root, executable, name, condition, args, cache_seed=None, timed=False):
    command = _board_command(root, executable, name, condition, args, cache_seed, timed)
    subprocess.run(command, cwd=root, check=True)
    directory = root / "build/board-qa-runs" / name
    provenance = read_board_result(directory)
    log_path = directory / "game.log"
    try:
        log = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise RuntimeError(f"missing board log: {log_path}") from error
    # No capture is admissible even if a future board runner forgets to update
    # result.json; the filesystem check is deliberately redundant.
    if list((directory / "frames").glob("*.mfd")):
        raise RuntimeError(f"board run contains frame captures: {directory}")
    validate_native_evidence(log, condition)
    return directory, provenance, log


def _initial_matrix_report(args, schedule, executable):
    return {
        "schema": "prepared-canonical-matrix-v1",
        "complete": False,
        "prepared_canonical_matrix": True,
        "matrix_schedule": list(schedule),
        "matrix_repetitions": args.matrix_repetitions,
        "executable": str(executable),
        "requested_policy": {
            "resolution": "full",
            "depth_storage": "r32float",
            "quality": "high",
            "denoise_passes": 2,
        },
        "warmups": [],
        "runs": [],
        "condition_medians": None,
        "condition_medians_ms": None,
        "scope": matrix_scope(),
    }


def run_prepared_canonical_matrix(args, root):
    """Run and checkpoint the current-frame prepared-canonical matrix.

    Warmups are intentionally retained only as provenance/cache seed records;
    they do not enter the measured medians.  A failed or interrupted matrix
    leaves ``.partial.json`` so a new name is required for a later attempt.
    """

    root = Path(root)
    if type(args.matrix_repetitions) is not int or args.matrix_repetitions < 1:
        raise RuntimeError("matrix repetitions must be a positive integer")
    schedule = matrix_schedule(args.matrix_repetitions)
    executable = (root / "build/engine-benchmark/current-frame/release/mp6native.exe").resolve(strict=True)
    result_path = root / "build/engine-benchmark" / f"{args.name}.json"
    partial_path = root / "build/engine-benchmark" / f"{args.name}.partial.json"
    if result_path.exists() or partial_path.exists():
        raise RuntimeError(f"refusing to clobber existing benchmark result: {result_path}")
    result_path.parent.mkdir(parents=True, exist_ok=True)

    report = _initial_matrix_report(args, schedule, executable)
    condition_seeds = {}
    executable_hash = None

    # One cache-warming run per condition, in stable order.  It uses the same
    # current-frame binary but omits GPU timing admission; its log is not used
    # as measured evidence.
    for condition in MATRIX_CONDITIONS:
        warmup_name = f"{args.name}-warmup-{condition}"
        directory, provenance, warmup_log = _run_board(
            root, executable, warmup_name, condition, args, timed=False)
        # The warmup is excluded from medians, but it must still have reached
        # the same complete CPU/frame-profile window before seeding caches.
        parse_engine_benchmark(warmup_log)
        _parse_frame_profile(warmup_log)
        if executable_hash is None:
            executable_hash = provenance["sha256"]
        elif provenance["sha256"] != executable_hash:
            raise RuntimeError("warmup executable hash changed within matrix")
        condition_seeds[condition] = directory / "gpu-cache"
        report["warmups"].append(dict(condition=condition, run=warmup_name,
                                      measured=False,
                                      effective_policy=validate_native_evidence(warmup_log, condition),
                                      executable_sha256=provenance["sha256"],
                                      cache_seed=str(condition_seeds[condition])))
        _write_checkpoint(partial_path, report)

    for index, condition in enumerate(schedule):
        run_name = f"{args.name}-{index}-{condition}"
        directory, provenance, log = _run_board(
            root, executable, run_name, condition, args,
            cache_seed=condition_seeds[condition], timed=True)
        if provenance["sha256"] != executable_hash:
            raise RuntimeError(
                f"executable hash changed for {run_name}: {provenance['sha256']} != {executable_hash}"
            )
        row = parse_matrix_run(log, condition)
        row.update(
            run=run_name,
            measured=True,
            executable_sha256=provenance["sha256"],
            cache_seed=str(condition_seeds[condition]),
            board_result=provenance["report_path"],
        )
        report["runs"].append(row)
        # Reuse only this condition's cache; a canonical cache must never leak
        # into view-Z/off runs (or vice versa).
        condition_seeds[condition] = directory / "gpu-cache"
        _write_checkpoint(partial_path, report)

    report["condition_medians"] = aggregate_medians(report["runs"], args.matrix_repetitions)
    report["condition_medians_ms"] = report["condition_medians"]
    report["executable_sha256"] = executable_hash
    report["complete"] = True
    if result_path.exists():
        raise RuntimeError(f"refusing to clobber benchmark result: {result_path}")
    result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "result_path": str(result_path),
        "executable_sha256": executable_hash,
        "condition_medians": report["condition_medians"],
        "condition_medians_ms": report["condition_medians_ms"],
        "scope": report["scope"],
    }, indent=2), flush=True)
    return report
