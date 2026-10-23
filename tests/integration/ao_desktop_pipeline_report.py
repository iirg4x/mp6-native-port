"""Bounded desktop AO pipeline report.

This is deliberately a small runner for the final desktop evidence matrix.  It
reuses the established strict benchmark/GPU/frame parsers, but owns the
matrix contract: both AO states use the same XeGTAO request, every run is
timestamped, and inline composite/seed timestamps are attribution rows rather
than additional GPU work to sum.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys

try:  # Direct execution from tests/integration.
    from ao_preparation_benchmark import (
        parse_engine_benchmark,
        parse_engine_gpu_block,
        read_board_result,
        validate_failure_markers,
    )
    from engine_benchmark_metrics import frame_profile
except ImportError:  # Package import from the repository root.
    from tests.integration.ao_preparation_benchmark import (
        parse_engine_benchmark,
        parse_engine_gpu_block,
        read_board_result,
        validate_failure_markers,
    )
    from tests.integration.engine_benchmark_metrics import frame_profile


ROOT = Path(__file__).resolve().parents[2]
INPUT_SCRIPT = "period:30;timeout:190000;pressuntil:a/w01.live/120;wait:18000"
SCHEDULE = ("off-warmup", "on-warmup", "off-1", "on-1", "on-2", "off-2")
AA_VALUES = (0, 1)

# These are the non-inline timestamp rows.  Composite is nested in the EFB
# recording path and foliage seed is nested in foreground mask for AA1.
XE_GPU_ROWS = (
    "AO depth preparation",
    "XeGTAO canonical depth",
    "XeGTAO depth MIPs",
    "XeGTAO normals",
    "XeGTAO main",
    "XeGTAO denoise",
    "XeGTAO final denoise",
)
INDEPENDENT_GPU_ROWS = ("AO depth snapshot", "AO foreground mask") + XE_GPU_ROWS
# Composite is a required core attribution row, but it is nested in EFB and
# therefore intentionally absent from the independent sum below.
CORE_GPU_ROWS = INDEPENDENT_GPU_ROWS + ("AO composite inline",)
NESTED_GPU_ROWS = ("AO composite inline", "AO foliage seed inline")
AO_GPU_ROWS = frozenset(CORE_GPU_ROWS + NESTED_GPU_ROWS)
INLINE_MARKERS = (
    "[MP6-INLINE-AO] feature=supported",
    "[MP6-INLINE-AO] scope=nested-efb timestamps=supported",
)
AO_GAME_FIELDS = (
    "renderInclusive", "renderExclusive", "faceDrawInclusive",
    "faceDrawExclusive", "renderCalls", "faceDrawCalls",
)

_HEX64 = re.compile(r"[0-9a-fA-F]{64}\Z")
_PREFIX = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*\Z")

_FORBIDDEN_OVERRIDE_KEYS = (
    "results_geometry", "leaf_camera", "leaf_freeze", "mobile_ao",
    "gpu_validation", "ao_admission", "ao_resources", "resize_ao", "unshaded_ao",
    "raw_ao_depth", "uncached_settings", "foreground_ao", "water_ao",
    "legacy_foliage_viewport", "foliage_no_depth", "geometry_at",
    "geometry_at_second", "no_grounding", "no_lawn_lift",
    "no_connector_lift", "no_log_grounding", "no_tree_grounding",
    "no_foliage_mask", "legacy_foliage_blend", "unlocked_fps", "audio_trace",
    "capture_frames", "capture_delay", "capture_rect", "one_turn", "hold_bad_camera",
    "hold_event_end", "one_turn", "stop_timechange", "save_at", "load_at",
    "state_path", "ao_cycle", "ao_fi_matrix_range", "ao_fi_pixels",
    "deferred_capture", "ao_buffers_tick", "xegtao_init_failure",
    "xegtao_prepared_canonical", "xegtao_batch_compute",
)


def schedule_names(prefix: str, aa: int) -> tuple[str, ...]:
    """Return the exact six-run order for one AA lane."""

    _validate_prefix(prefix)
    if aa not in AA_VALUES:
        raise ValueError("AA lane must be 0 or 1")
    return tuple(f"{prefix}-aa{aa}-{token}" for token in SCHEDULE)


def _validate_prefix(prefix: str) -> None:
    if not isinstance(prefix, str) or _PREFIX.fullmatch(prefix) is None:
        raise ValueError("prefix must contain only letters, digits, '_' or '-'")


def _schedule_item(prefix: str, aa: int, token: str) -> dict:
    if token not in SCHEDULE:
        raise ValueError(f"unknown desktop AO schedule item: {token}")
    condition, suffix = token.split("-", 1)
    return {
        "name": f"{prefix}-aa{aa}-{token}",
        "aa": aa,
        "condition": condition,
        "warmup": suffix == "warmup",
        "suffix": suffix,
    }


def board_command(executable, name: str, aa: int, condition: str,
                  *, cache_seed=None) -> list[str]:
    """Build one exact board-qa command; no scene/camera override is allowed."""

    if condition not in ("off", "on"):
        raise ValueError("condition must be off or on")
    if aa not in AA_VALUES:
        raise ValueError("AA lane must be 0 or 1")
    command = [
        sys.executable,
        "tests/integration/run_board_qa.py",
        "--exe", str(executable),
        "--name", name,
        "--seconds", "90",
        "--ticks", "200000",
        "--stop-round", "0",
        "--input-script", INPUT_SCRIPT,
        "--window-size", "1920x1080",
        "--widescreen",
        "--aa", str(aa),
        "--ao", "0" if condition == "off" else "2",
        "--implementation", "xegtao",
        "--xegtao-resolution", "full",
        "--xegtao-depth-storage", "r32float",
        "--xegtao-quality", "high",
        "--xegtao-denoise", "2",
        # Warmups are intentionally timestamped too, so a missing/incomplete
        # window cannot be hidden by the unmeasured half of the matrix.
        "--gpu-timings",
    ]
    if cache_seed is not None:
        command.extend(("--cache-seed", str(cache_seed)))
    return command


def _positive(value, label):
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"{label} must be finite and positive")
    return value


def _validate_game_marker(profile: dict, *, require_positive: bool) -> dict:
    game = profile.get("ao_game_average")
    if not isinstance(game, dict):
        raise ValueError("every run requires an ao_game_average marker")
    for field in (
        "renderInclusive", "renderExclusive", "faceDrawInclusive",
        "faceDrawExclusive", "renderCalls", "faceDrawCalls",
    ):
        if field not in game:
            raise ValueError(f"ao_game_average is missing {field}")
        if not math.isfinite(game[field]) or game[field] < 0:
            raise ValueError(f"invalid ao_game_average {field}")
    # A marker with no transferred HSF lifetime state is not evidence for an
    # AO-on run.  AO-off still requires the marker, but its intentionally
    # disabled path may report zero work.
    if require_positive:
        _positive(game["renderCalls"], "ao_game_average.renderCalls")
        _positive(game["faceDrawCalls"], "ao_game_average.faceDrawCalls")
        _positive(game["renderInclusive"], "ao_game_average.renderInclusive")
        _positive(game["faceDrawInclusive"], "ao_game_average.faceDrawInclusive")
    return game


def _validate_off(log: str, passes: dict) -> None:
    observed = sorted(name for name in passes
                      if name == "GTAO" or name.startswith(("AO ", "XeGTAO ")))
    if observed:
        raise ValueError("AO-off run contains AO GPU rows: " + ", ".join(observed))
    if any(line.startswith("[MP6-AO] ") for line in log.splitlines()):
        raise ValueError("AO-off run contains native AO evidence")
    if any("[MP6-INLINE-AO] scope=" in line for line in log.splitlines()):
        raise ValueError("AO-off run contains inline AO execution evidence")


def _validate_on_policy(log: str) -> None:
    """Require the exact runtime policy before the final GPU window."""

    lines = log.splitlines()
    header_indices = [index for index, line in enumerate(lines)
                      if line.startswith("[ENGINE-GPU]")]
    if len(header_indices) != 1:
        raise ValueError("AO-on run requires exactly one ENGINE-GPU header")
    header_index = header_indices[0]
    expected = (
        "[MP6-AO] requested implementation: XeGTAO (environment override)",
        "[MP6-AO] shader/scheduling path: optimized",
        "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float",
        "[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, 2 spatial denoise passes",
    )
    for marker in expected:
        indices = [index for index, line in enumerate(lines) if line == marker]
        if len(indices) != 1 or indices[0] >= header_index:
            raise ValueError("AO-on runtime policy missing/late: " + marker)
    inline = [line[line.index("[MP6-INLINE-AO]"):].strip()
              for line in lines if "[MP6-INLINE-AO]" in line]
    if sorted(inline) != sorted(INLINE_MARKERS):
        raise ValueError("AO-on inline marker inventory mismatch")
    inline_indices = [index for index, line in enumerate(lines)
                      if "[MP6-INLINE-AO]" in line]
    if any(index >= header_index for index in inline_indices):
        raise ValueError("AO-on inline marker is late")


def parse_run_log(log: str, *, aa: int, condition: str, warmup: bool = False) -> dict:
    """Parse one run and return CPU/GPU rows with explicit nesting metadata."""

    if aa not in AA_VALUES:
        raise ValueError("AA lane must be 0 or 1")
    if condition not in ("off", "on"):
        raise ValueError("condition must be off or on")
    validate_failure_markers(log)
    if "[FRAME-AO-GAME-ERROR]" in log:
        raise ValueError("run contains unbalanced AO game-thread scope marker")
    benchmark = parse_engine_benchmark(log)
    gpu = parse_engine_gpu_block(log)
    profile = frame_profile(log)
    game = _validate_game_marker(profile, require_positive=condition == "on")
    cpu = profile["cpu_average_ms"]
    required_cpu = ("encode", "aoEncode", "aoCompositeRecord", "aoFoliageSeedRecord")
    missing_cpu = [field for field in required_cpu if field not in cpu]
    if missing_cpu:
        raise ValueError("frame profile is missing AO CPU fields: " + ", ".join(missing_cpu))
    if profile.get("ao_record_scope") != "complete callback CPU recording (lookup/binding/setup/draw); nested inside encode; not GPU execution":
        raise ValueError("every run requires complete AO callback-record scope evidence")

    passes = gpu["passes"]
    if condition == "off":
        _validate_off(log, passes)
        required_rows = ()
    else:
        _validate_on_policy(log)
        missing = [name for name in CORE_GPU_ROWS if name not in passes]
        if missing:
            raise ValueError("AO-on run is missing GPU rows: " + ", ".join(missing))
        unexpected = sorted({name for name in passes
                             if (name == "GTAO" or name.startswith(("AO ", "XeGTAO ")))
                             and name not in AO_GPU_ROWS})
        if unexpected:
            raise ValueError("AO-on run contains unknown/fallback GPU rows: " + ", ".join(unexpected))
        required_rows = INDEPENDENT_GPU_ROWS
        if aa == 1 and "AO foliage seed inline" not in passes:
            raise ValueError("AA1 AO-on run requires nested AO foliage seed inline row")
        if aa == 0 and "AO foliage seed inline" in passes:
            raise ValueError("AA0 AO-on run contains AO foliage seed inline row")

    rows = {}
    for name in required_rows:
        rows[name] = dict(passes[name], nested=False)
    if condition == "on" and "AO composite inline" in passes:
        rows["AO composite inline"] = dict(
            passes["AO composite inline"], nested=True, nested_in="EFB")
    if condition == "on" and "AO foliage seed inline" in passes:
        rows["AO foliage seed inline"] = dict(
            passes["AO foliage seed inline"], nested=True,
            nested_in="AO foreground mask")

    # Only independent rows contribute to this partial attribution.  The
    # inline rows remain visible for diagnosis but are never added again.
    independent_sum = sum(rows[name]["average_ms"] for name in required_rows)
    return {
        "aa": aa,
        "condition": condition,
        "warmup": bool(warmup),
        "cpu_frame_ms": benchmark["frame"],
        "game_ms": benchmark["game"],
        "submit_ms": benchmark["submit"],
        "worker_encode_ms": cpu["encode"],
        "ao_encode_ms": cpu["aoEncode"],
        "ao_composite_record_ms": cpu["aoCompositeRecord"],
        "ao_foliage_seed_record_ms": cpu["aoFoliageSeedRecord"],
        "ao_game_average": game,
        "gpu_status": gpu["status"],
        "gpu_sample_count": gpu["sample_count"],
        "gpu_dropped_frames": gpu["dropped_frames"],
        "gpu_span_ms": gpu["span_average_ms"],
        "gpu_between_ms": gpu["between_average_ms"],
        "gpu_passes": rows,
        "gpu_all_passes": gpu["passes"],
        "independent_ao_gpu_sum_ms": independent_sum,
        "independent_ao_gpu_scope": (
            "Sum of independent AO rows only; inline composite and AA1 foliage "
            "seed rows are nested attribution and excluded from this sum."
        ),
        "cpu_scope": benchmark["scope"],
        "gpu_scope": gpu["scope"],
        "frame_profile_scope": profile["scope"],
        "frame_profile": profile,
    }


def _request_guard(request: dict, *, executable: Path, name: str, aa: int,
                   condition: str, warmup: bool, cache_seed):
    """Reject a result whose recorded request differs from the matrix."""

    expected = {
        "name": name,
        "aa": aa,
        "seconds": 90,
        "ticks": 200000,
        "stop_round": 0,
        "widescreen": True,
        "window_size": "1920x1080",
        "ao": 0 if condition == "off" else 2,
        "implementation": "xegtao",
        "xegtao_quality": "high",
        "xegtao_denoise": 2,
        "xegtao_resolution": "full",
        "xegtao_depth_storage": "r32float",
        "gpu_timings": True,
        "input_script": INPUT_SCRIPT,
    }
    for key, value in expected.items():
        if request.get(key) != value:
            raise RuntimeError(f"{name}: request {key}={request.get(key)!r}, expected {value!r}")
    if request.get("scene_camera") is not None:
        raise RuntimeError(f"{name}: scene camera override is forbidden")
    exact_defaults = {
        "capsule": -1,
        "route": "",
        "xegtao_depth_mip_offset": None,
        "leaf_camera_mode": "pan",
        "ao_fi_matrix_limit": 256,
        "shadow_quality": 1,
        "tick_hz": 0,
        "capture_stride": 4,
        "capture_trigger": "capsule.begin",
    }
    for key, value in exact_defaults.items():
        if request.get(key) != value:
            raise RuntimeError(f"{name}: request {key}={request.get(key)!r}, expected {value!r}")
    for key in _FORBIDDEN_OVERRIDE_KEYS:
        if request.get(key) not in (None, False, 0, ""):
            raise RuntimeError(f"{name}: scene/diagnostic override {key} is forbidden")
    recorded_seed = request.get("cache_seed")
    if warmup:
        if recorded_seed not in (None, ""):
            raise RuntimeError(f"{name}: warmup must not use a cache seed")
    elif not recorded_seed or cache_seed is None:
        raise RuntimeError(f"{name}: measured run is missing matching warmup cache seed")
    else:
        recorded_path = Path(recorded_seed)
        expected_path = Path(cache_seed)
        if not recorded_path.is_absolute():
            recorded_path = ROOT / recorded_path
        if not expected_path.is_absolute():
            expected_path = ROOT / expected_path
        if recorded_path.resolve() != expected_path.resolve():
            raise RuntimeError(f"{name}: measured run cache seed does not match warmup")
    recorded_exe = request.get("exe") or request.get("executable")
    if recorded_exe is not None:
        recorded_path = Path(recorded_exe)
        if not recorded_path.is_absolute():
            recorded_path = ROOT / recorded_path
        if recorded_path.resolve() != executable.resolve():
            raise RuntimeError(f"{name}: result executable differs from requested executable")


def _digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _resolve_root_path(value) -> Path:
    path = Path(value)
    return (ROOT / path).resolve() if not path.is_absolute() else path.resolve()


def _run_one(executable: Path, item: dict, cache_seed=None,
             expected_sha256: str | None = None) -> tuple[dict, Path]:
    name = item["name"]
    # This report is intentionally read-only.  The parent matrix owner starts
    # the twelve serialized native jobs; this helper only consumes their
    # result/log artifacts and fails on a missing or stale run.
    directory = ROOT / "build" / "board-qa-runs" / name
    provenance = read_board_result(directory)
    if expected_sha256 is not None and provenance["sha256"] != expected_sha256.casefold():
        raise RuntimeError(f"{name}: board result executable hash is stale")
    report = json.loads((directory / "result.json").read_text(encoding="utf-8"))
    if expected_sha256 is not None and report.get("sha256", "").casefold() != expected_sha256.casefold():
        raise RuntimeError(f"{name}: result executable hash is stale")
    result_exe = report.get("executable")
    if not isinstance(result_exe, str) or not result_exe:
        raise RuntimeError(f"{name}: result executable is missing")
    result_exe_path = Path(result_exe)
    if not result_exe_path.is_absolute():
        result_exe_path = ROOT / result_exe_path
    if result_exe_path.resolve() != executable.resolve():
        raise RuntimeError(f"{name}: result executable differs from supplied executable")
    _request_guard(report.get("request", {}), executable=executable, name=name,
                   aa=item["aa"], condition=item["condition"],
                   warmup=item["warmup"], cache_seed=cache_seed)
    log = (directory / "game.log").read_text(encoding="utf-8", errors="replace")
    parsed = parse_run_log(log, aa=item["aa"], condition=item["condition"],
                           warmup=item["warmup"])
    parsed.update({"name": name, "sha256": provenance["sha256"],
                   "result_path": provenance["report_path"],
                   "log_path": str(directory / "game.log"),
                   "cache_seed": report.get("request", {}).get("cache_seed"),
                   "request": report.get("request", {}),
                   "result_sha256": _digest(directory / "result.json"),
                   "log_sha256": _digest(directory / "game.log"),
                   "executable_sha256": provenance["sha256"]})
    # Keep the strict parser's complete windows available to downstream audit
    # readers; the compact fields above are the stable summary surface.
    parsed["gpu"] = {
        "status": parsed["gpu_status"],
        "sample_count": parsed["gpu_sample_count"],
        "dropped_frames": parsed["gpu_dropped_frames"],
        "span_average_ms": parsed["gpu_span_ms"],
        "between_average_ms": parsed["gpu_between_ms"],
        "passes": parsed["gpu_all_passes"],
    }
    return parsed, directory


def _median_rows(rows: list[dict]) -> dict:
    measured = [row for row in rows if not row["warmup"]]
    if len(measured) != 2:
        raise ValueError("each AO condition must have exactly two measured runs")
    fields = (
        "cpu_frame_ms", "game_ms", "submit_ms", "worker_encode_ms",
        "ao_encode_ms", "ao_composite_record_ms", "ao_foliage_seed_record_ms",
        "gpu_span_ms", "gpu_between_ms", "independent_ao_gpu_sum_ms",
    )
    medians = {field: statistics.median(row[field] for row in measured)
               for field in fields}
    game = {
        field: statistics.median(row["ao_game_average"][field] for row in measured)
        for field in AO_GAME_FIELDS
    }
    medians["ao_game_average"] = game
    # Keep the marker fields available without requiring consumers to infer
    # units from the nested parser spelling.  Calls are counts, the first four
    # values are milliseconds.
    for field in AO_GAME_FIELDS:
        suffix = {
            "renderInclusive": "render_inclusive_ms",
            "renderExclusive": "render_exclusive_ms",
            "faceDrawInclusive": "face_draw_inclusive_ms",
            "faceDrawExclusive": "face_draw_exclusive_ms",
            "renderCalls": "render_calls",
            "faceDrawCalls": "face_draw_calls",
        }[field]
        medians["ao_game_" + suffix] = game[field]

    pass_names = set(measured[0]["gpu_all_passes"])
    if any(set(row["gpu_all_passes"]) != pass_names for row in measured[1:]):
        raise ValueError("measured runs have different GPU pass inventories")
    medians["gpu_pass_medians"] = {
        name: {
            "average_ms": statistics.median(
                row["gpu_all_passes"][name]["average_ms"] for row in measured),
            "max_ms": statistics.median(
                row["gpu_all_passes"][name]["max_ms"] for row in measured),
        }
        for name in sorted(pass_names)
    }
    medians["gpu_independent_rows"] = list(INDEPENDENT_GPU_ROWS)
    medians["gpu_nested_rows"] = [name for name in NESTED_GPU_ROWS if name in pass_names]
    medians["gpu_pass_scope"] = (
        "Each GPU component has its own two-run median. Inline composite and "
        "foliage-seed rows are nested attribution and are excluded from "
        "independent_ao_gpu_sum_ms."
    )
    return medians


def _same_aa_on_minus_off(on: dict, off: dict) -> dict:
    """Compare measured medians within one AA lane, always on minus off."""

    fields = (
        "cpu_frame_ms", "game_ms", "submit_ms", "worker_encode_ms",
        "ao_encode_ms", "ao_composite_record_ms", "ao_foliage_seed_record_ms",
        "gpu_span_ms", "gpu_between_ms", "independent_ao_gpu_sum_ms",
    )
    delta = {
        field: on[field] - off[field]
        for field in fields
    }
    delta["ao_game_average"] = {
        field: on["ao_game_average"][field] - off["ao_game_average"][field]
        for field in AO_GAME_FIELDS
    }
    for field in AO_GAME_FIELDS:
        suffix = {
            "renderInclusive": "render_inclusive_ms",
            "renderExclusive": "render_exclusive_ms",
            "faceDrawInclusive": "face_draw_inclusive_ms",
            "faceDrawExclusive": "face_draw_exclusive_ms",
            "renderCalls": "render_calls",
            "faceDrawCalls": "face_draw_calls",
        }[field]
        delta["ao_game_" + suffix] = delta["ao_game_average"][field]

    on_passes = on["gpu_pass_medians"]
    off_passes = off["gpu_pass_medians"]
    names = sorted(set(on_passes) | set(off_passes))
    delta["gpu_pass_deltas"] = {
        name: {
            "average_ms": on_passes.get(name, {}).get("average_ms", 0.0)
            - off_passes.get(name, {}).get("average_ms", 0.0),
            "max_ms": on_passes.get(name, {}).get("max_ms", 0.0)
            - off_passes.get(name, {}).get("max_ms", 0.0),
        }
        for name in names
    }
    delta["gpu_nested_pass_deltas"] = {
        name: delta["gpu_pass_deltas"][name]
        for name in NESTED_GPU_ROWS if name in delta["gpu_pass_deltas"]
    }
    delta["scope"] = (
        "Same-AA measured median comparison, AO-on minus AO-off. CPU values "
        "are overlapping attributions; GPU component deltas do not form a "
        "whole-frame cost. Nested composite/seed deltas remain separate and "
        "are not included in independent_ao_gpu_sum_ms."
    )
    return delta


def build_report(*, executable: Path, exe_sha256: str, prefix: str,
                 output: Path) -> dict:
    """Consume completed matrix artifacts and write one fresh report."""

    _validate_prefix(prefix)
    if _HEX64.fullmatch(exe_sha256 or "") is None:
        raise ValueError("exe-sha256 must be exactly 64 hexadecimal characters")
    executable = _resolve_root_path(executable).resolve(strict=True)
    actual = _digest(executable)
    if actual.casefold() != exe_sha256.casefold():
        raise ValueError(f"executable SHA-256 mismatch: {actual}")
    rows = []
    for aa in AA_VALUES:
        seeds = {}
        lane_items = [_schedule_item(prefix, aa, token) for token in SCHEDULE]
        for item in lane_items:
            seed = seeds.get(item["condition"])
            row, directory = _run_one(executable, item, seed, actual)
            rows.append(row)
            if item["warmup"]:
                seeds[item["condition"]] = str(directory / "gpu-cache")
    measured = {f"aa{aa}": {
        condition: _median_rows([row for row in rows
                                 if row["aa"] == aa and row["condition"] == condition])
        for condition in ("off", "on")
    } for aa in AA_VALUES}
    same_aa_on_minus_off = {
        f"aa{aa}": _same_aa_on_minus_off(
            measured[f"aa{aa}"]["on"], measured[f"aa{aa}"]["off"])
        for aa in AA_VALUES
    }
    report = {
        "schema": "ao-desktop-pipeline-v1",
        "executable": str(executable),
        "exe_sha256": actual,
        "prefix": prefix,
        "schedule": list(SCHEDULE),
        "aa_lanes": list(AA_VALUES),
        "request": {
            "seconds": 90,
            "ticks": 200000,
            "stop_round": 0,
            "input_script": INPUT_SCRIPT,
            "window_size": "1920x1080",
            "widescreen": True,
            "implementation": "xegtao",
            "resolution": "full",
            "depth_storage": "r32float",
            "quality": "high",
            "denoise": 2,
            "gpu_timings": True,
            "scene_override": None,
        },
        "core_gpu_rows": list(CORE_GPU_ROWS),
        "nested_gpu_rows": list(NESTED_GPU_ROWS),
        "measured_medians": measured,
        "same_aa_on_minus_off": same_aa_on_minus_off,
        "runs": rows,
        "scope": (
            "Desktop diagnostic evidence only. CPU rows are overlapping 4,000-frame "
            "attributions; GPU rows are a strict 120-sample window. Inline composite "
            "and foliage-seed timestamps are nested and excluded from independent sums."
        ),
    }
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    report = build_report(
        executable=Path(args.exe), exe_sha256=args.exe_sha256,
        prefix=args.prefix, output=Path(args.output))
    print(json.dumps({"schema": report["schema"], "output": str(args.output),
                      "runs": len(report["runs"])}, sort_keys=True))
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
