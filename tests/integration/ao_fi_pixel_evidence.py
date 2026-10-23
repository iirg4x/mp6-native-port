"""Stdlib-only loader and same-phase validator for AO/FI pixel evidence."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import re
import struct
from typing import Any

try:
    from ao_fi_pixel_policy import validate_ao_fi_pixel_policy
except ImportError:  # pragma: no cover - package import fallback
    from .ao_fi_pixel_policy import validate_ao_fi_pixel_policy

MFD_HEADER_BYTES = 96
MFD_MAGIC = b"MP6FDUMP"
MFD_VERSION = 1
MFD_BPP = 4
SUPPORTED_FORMATS = frozenset((1, 2, 3, 4))
INDEX_FIELDS = (
    "seq", "present_index", "tick", "wall_ns", "replay", "width", "height",
    "src_width", "src_height", "crop_x", "crop_y", "crop_w", "crop_h",
    "downscale", "format",
)
PROOF_MARKER = "[AO-FI-PIXEL]"
PROOF_FIELDS = frozenset({
    "seq", "presentIndex", "captureViTick", "viTick", "previousViTick",
    "seal", "previousSeal", "alpha", "interpolationEnabled", "matrixCount",
    "hintOverflow", "previousHintOverflow", "matrices",
})
MATRIX_FIELDS = frozenset({
    "index", "object", "name", "key", "previousKey", "currentBits",
    "previousBits", "outputBits", "cameraViewBits", "cameraBits", "submitted",
})
KEY_FIELDS = frozenset({"valid", "camera", "model", "generation", "sub", "ordinal"})
FIXED_PHASES = (0.0, 0.25, 0.5, 0.75)
ERROR_MARKERS = (
    "[MP6-CRASH]", "[FATAL]", "[AURORA FATAL:", "[AURORA ERROR",
    "GPUValidationError", "uncaptured error", "[ERROR]", "[AO-FI-PIXEL-ERROR]",
)
_SHA256_RE = re.compile(r"[0-9a-fA-F]{64}")


def _json_object(text: str, label: str) -> dict[str, Any]:
    def reject(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON field: {key}")
            result[key] = value
        return result

    try:
        result = json.loads(text, object_pairs_hook=reject)
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise ValueError(f"malformed {label} JSON") from exc
    if not isinstance(result, dict):
        raise ValueError(f"{label} JSON must be an object")
    return result


def _int(value: Any, name: str, minimum: int | None = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    if minimum is not None and value < minimum:
        raise ValueError(f"{name} must be >= {minimum}")
    return value


def _u32(value: Any, name: str) -> int:
    value = _int(value, name, 0)
    if value > 0xFFFFFFFF:
        raise ValueError(f"{name} does not fit uint32")
    return value


def _csv_int(value: str, name: str) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        raise ValueError(f"{name} must be a nonnegative decimal integer")
    return int(value, 10)


def _sha256(value: Any, name: str) -> str:
    if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
        raise ValueError(f"{name} must be a 64-character hexadecimal SHA-256")
    return value.lower()


def _phase_code(alpha: Any) -> int:
    if isinstance(alpha, bool) or not isinstance(alpha, (int, float)):
        raise ValueError("alpha must be numeric")
    alpha = float(alpha)
    if not math.isfinite(alpha):
        raise ValueError("alpha must be finite")
    code = round(alpha * 4.0)
    if code not in range(4) or alpha != FIXED_PHASES[code]:
        raise ValueError("alpha must be one of 0, .25, .5, .75")
    return code


def parse_schedule(value: Any) -> tuple[int, int, int]:
    """Parse an inclusive ``FROM:TO:STRIDE`` diagnostic schedule."""
    if not isinstance(value, str):
        raise ValueError("AO FI pixel schedule must be FROM:TO:STRIDE")
    match = re.fullmatch(r"([0-9]+):([0-9]+):([0-9]+)", value)
    if match is None:
        raise ValueError("AO FI pixel schedule must be FROM:TO:STRIDE")
    start, end, stride = (int(part) for part in match.groups())
    if start > end or stride <= 0 or end - start > 120:
        raise ValueError("invalid AO FI pixel schedule")
    return start, end, stride


def _expected_phase(vi_tick: int, schedule: tuple[int, int, int]) -> int:
    start, end, stride = schedule
    if vi_tick < start or vi_tick > end or (vi_tick - start) % stride:
        raise ValueError(f"viTick {vi_tick} is outside the requested pixel schedule")
    return ((vi_tick - start) // stride) % 4


def _key(value: Any, name: str) -> dict[str, int]:
    if not isinstance(value, dict) or set(value) != KEY_FIELDS:
        raise ValueError(f"{name} has an invalid key schema")
    result = {field: _int(value[field], f"{name}.{field}") for field in KEY_FIELDS}
    for field in ("valid", "generation", "sub", "ordinal"):
        _u32(result[field], f"{name}.{field}")
    if result["valid"] not in (0, 1):
        raise ValueError(f"{name}.valid must be 0 or 1")
    return result


def _bits(value: Any, name: str, count: int) -> list[int]:
    if not isinstance(value, list) or len(value) != count:
        raise ValueError(f"{name} must contain exactly {count} uint32 values")
    return [_u32(item, f"{name}[{index}]") for index, item in enumerate(value)]


def _matrix(value: Any, number: int) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != MATRIX_FIELDS:
        raise ValueError(f"matrix {number} has an invalid schema")
    result = dict(value)
    result["index"] = _int(result["index"], f"matrix[{number}].index", 0)
    result["object"] = _int(result["object"], f"matrix[{number}].object", 0)
    if not isinstance(result["name"], str) or not result["name"]:
        raise ValueError(f"matrix[{number}].name must be nonempty text")
    result["key"] = _key(result["key"], f"matrix[{number}].key")
    result["previousKey"] = _key(result["previousKey"], f"matrix[{number}].previousKey")
    if result["key"]["valid"] != 1 or result["previousKey"]["valid"] != 1:
        raise ValueError(f"matrix[{number}] keys must both be valid")
    if result["previousKey"] != result["key"]:
        raise ValueError(f"matrix[{number}] previousKey does not equal key")
    result["currentBits"] = _bits(result["currentBits"], f"matrix[{number}].currentBits", 12)
    result["previousBits"] = _bits(result["previousBits"], f"matrix[{number}].previousBits", 12)
    result["outputBits"] = _bits(result["outputBits"], f"matrix[{number}].outputBits", 12)
    result["cameraViewBits"] = _bits(result["cameraViewBits"], f"matrix[{number}].cameraViewBits", 12)
    result["cameraBits"] = _bits(result["cameraBits"], f"matrix[{number}].cameraBits", 13)
    if result["submitted"] is not True:
        raise ValueError(f"matrix[{number}] is not a submitted replay")
    return result


def _proof(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != PROOF_FIELDS:
        raise ValueError("AO-FI-PIXEL row has an invalid schema")
    result = dict(value)
    for field in ("seq", "presentIndex", "captureViTick", "viTick", "previousViTick",
                  "seal", "previousSeal"):
        result[field] = _int(result[field], f"proof.{field}")
    if isinstance(result["alpha"], bool) or not isinstance(result["alpha"], (int, float)):
        raise ValueError("proof.alpha must be numeric")
    result["alpha"] = float(result["alpha"])
    _phase_code(result["alpha"])
    if (isinstance(result["interpolationEnabled"], bool)
            or not isinstance(result["interpolationEnabled"], int)
            or result["interpolationEnabled"] != 1):
        raise ValueError("AO-FI-PIXEL row is not an interpolation capture")
    result["matrixCount"] = _int(result["matrixCount"], "proof.matrixCount", 1)
    result["hintOverflow"] = _int(result["hintOverflow"], "proof.hintOverflow", 0)
    result["previousHintOverflow"] = _int(result["previousHintOverflow"], "proof.previousHintOverflow", 0)
    if result["hintOverflow"] or result["previousHintOverflow"]:
        raise ValueError("AO-FI-PIXEL row has hint overflow")
    if not isinstance(result["matrices"], list) or len(result["matrices"]) != result["matrixCount"]:
        raise ValueError("AO-FI-PIXEL matrixCount does not match matrices")
    result["matrices"] = [_matrix(item, index) for index, item in enumerate(result["matrices"])]
    indexes = [item["index"] for item in result["matrices"]]
    if len(indexes) != len(set(indexes)):
        raise ValueError("AO-FI-PIXEL row contains duplicate matrix indexes")
    return result


def parse_pixel_proof(log: str) -> list[dict[str, Any]]:
    """Parse one strict successful-capture proof row per frame."""
    if not isinstance(log, str):
        raise TypeError("log must be text")
    rows = []
    for line_number, line in enumerate(log.splitlines(), 1):
        if PROOF_MARKER not in line:
            continue
        if line.count(PROOF_MARKER) != 1:
            raise ValueError(f"duplicate AO-FI-PIXEL marker on line {line_number}")
        rows.append(_proof(_json_object(
            line.split(PROOF_MARKER, 1)[1].strip(), f"AO-FI-PIXEL line {line_number}")))
    if not rows:
        raise ValueError("missing AO-FI-PIXEL successful-capture proof")
    keys = [(row["seq"], row["presentIndex"], row["captureViTick"]) for row in rows]
    if len(keys) != len(set(keys)):
        raise ValueError("duplicate AO-FI-PIXEL proof for one frame")
    return rows


def _load_mfd(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as stream:
            raw = stream.read(MFD_HEADER_BYTES)
        size = path.stat().st_size
    except OSError as exc:
        raise ValueError(f"cannot read MFD frame: {path}") from exc
    if len(raw) != MFD_HEADER_BYTES or raw[:8] != MFD_MAGIC:
        raise ValueError(f"invalid MFD magic/header: {path}")
    version, width, height, bpp, fmt, seq, present, replay = struct.unpack_from("<8I", raw, 8)
    tick, wall_ns = struct.unpack_from("<2Q", raw, 40)
    src_w, src_h, crop_x, crop_y, crop_w, crop_h, downscale = struct.unpack_from("<7I", raw, 56)
    if version != MFD_VERSION:
        raise ValueError(f"unsupported MFD version {version}: {path}")
    if min(width, height, src_w, src_h, crop_w, crop_h, downscale) <= 0:
        raise ValueError(f"invalid MFD dimensions/crop: {path}")
    if bpp != MFD_BPP:
        raise ValueError(f"unsupported MFD bytes-per-pixel {bpp}: {path}")
    if fmt not in SUPPORTED_FORMATS:
        raise ValueError(f"unsupported MFD format {fmt}: {path}")
    if replay not in (0, 1):
        raise ValueError(f"invalid MFD replay flag: {path}")
    if crop_x + crop_w > src_w or crop_y + crop_h > src_h:
        raise ValueError(f"MFD crop lies outside source dimensions: {path}")
    if (width, height) != ((crop_w + downscale - 1) // downscale,
                           (crop_h + downscale - 1) // downscale):
        raise ValueError(f"MFD dimensions do not match crop/downscale: {path}")
    payload_length = size - MFD_HEADER_BYTES
    expected_payload = width * height * bpp
    if payload_length != expected_payload:
        raise ValueError(f"MFD payload length mismatch: {path}")
    if any(struct.unpack_from("<3I", raw, 84)):
        raise ValueError(f"MFD reserved header bytes are nonzero: {path}")
    return {
        "path": str(path.resolve()), "magic": MFD_MAGIC.decode("ascii"), "version": version,
        "width": width, "height": height, "bytes_per_pixel": bpp, "format": fmt,
        "seq": seq, "present_index": present, "replay": replay, "tick": tick,
        "wall_ns": wall_ns, "src_width": src_w, "src_height": src_h,
        "crop_x": crop_x, "crop_y": crop_y, "crop_w": crop_w, "crop_h": crop_h,
        "downscale": downscale, "payload_length": payload_length,
    }


def load_mfd(path: str | Path) -> dict[str, Any]:
    """Validate one native 96-byte-header MFD; return metadata and path."""
    return _load_mfd(Path(path).resolve(strict=True))


def _load_index(path: Path) -> list[dict[str, int]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.reader(stream)
            if next(reader, None) != list(INDEX_FIELDS):
                raise ValueError("index.csv has an unexpected header")
            rows = []
            for line_number, fields in enumerate(reader, 2):
                if len(fields) != len(INDEX_FIELDS):
                    raise ValueError(f"index.csv row {line_number} has wrong field count")
                row = {field: _csv_int(value, f"index.csv row {line_number} {field}")
                       for field, value in zip(INDEX_FIELDS, fields)}
                if row["replay"] not in (0, 1):
                    raise ValueError(f"index.csv row {line_number} has invalid replay")
                rows.append(row)
    except OSError as exc:
        raise ValueError(f"cannot read index.csv: {path}") from exc
    if not rows:
        raise ValueError("index.csv has no captured frames")
    seqs = [row["seq"] for row in rows]
    if seqs != list(range(len(rows))) or len(set(seqs)) != len(seqs):
        raise ValueError("index.csv seq values must be contiguous and unique from zero")
    return rows


def load_index(path: str | Path) -> list[dict[str, int]]:
    return _load_index(Path(path).resolve(strict=True))


def _validate_default_selection(result: dict[str, Any]) -> None:
    selection = result.get("default_selection")
    if not isinstance(selection, dict):
        raise ValueError("result is missing default_selection runtime evidence")
    if selection.get("implementation") != "default" or selection.get("selection") != "platform-default":
        raise ValueError("default_selection does not prove platform default Xe selection")
    if selection.get("reference") != "High2-full-r32":
        raise ValueError("default_selection reference is not High2-full-r32")
    markers = selection.get("markers")
    expected_markers = {
        "selection": "[MP6-AO] requested implementation: XeGTAO (platform default)",
        "reference": "[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, 2 spatial denoise passes",
        "depth_storage": "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float",
    }
    if not isinstance(markers, dict) or any(markers.get(field) != expected
                                              for field, expected in expected_markers.items()):
        raise ValueError("default_selection markers do not prove High2-full-r32 Xe")
    counters = selection.get("counters")
    if not isinstance(counters, dict):
        raise ValueError("default_selection is missing runtime counters")
    values = {field: _int(counters.get(field), f"default_selection.counters.{field}", 0)
              for field in ("encoded", "composited", "foliage", "xegtao")}
    if values["xegtao"] <= 0 or values["encoded"] != values["composited"] \
            or values["composited"] != values["xegtao"]:
        raise ValueError("default_selection counters do not prove active Xe compositing")
    _int(selection.get("counter_records"), "default_selection.counter_records", 1)
    if selection.get("exit_code") != 0 or selection.get("reason") != "exited":
        raise ValueError("default_selection does not prove a successful Xe run")


def _validate_result(run: Path, log: str) -> tuple[dict[str, Any], dict[str, Any], str, str, str]:
    try:
        result = _json_object((run / "result.json").read_text(encoding="utf-8"), "result")
    except OSError as exc:
        raise ValueError(f"missing result.json: {run / 'result.json'}") from exc
    request = result.get("request")
    if not isinstance(request, dict):
        raise ValueError("result.json has no request object")
    sha = _sha256(result.get("sha256", result.get("executable_sha256")),
                  "executable sha256")
    if result.get("exit_code") != 0:
        raise ValueError(f"run did not exit cleanly: {result.get('exit_code')!r}")
    if result.get("reason") != "exited":
        raise ValueError(f"run does not prove reason='exited': {result.get('reason')!r}")
    if result.get("crash_marker") is not False:
        raise ValueError("run does not prove crash_marker=false")
    if result.get("errors") or result.get("error"):
        raise ValueError("run result contains errors")
    pixel_report = result.get("ao_fi_pixels")
    matrix_report = result.get("ao_fi_matrix")
    if not isinstance(pixel_report, dict):
        raise ValueError("result is missing ao_fi_pixels runtime evidence")
    if not isinstance(matrix_report, dict):
        raise ValueError("result is missing ao_fi_matrix runtime evidence")
    pixel_probe_sha = _sha256(pixel_report.get("probe_sha256"),
                              "ao_fi_pixels.probe_sha256")
    matrix_probe_sha = _sha256(matrix_report.get("probe_sha256"),
                               "ao_fi_matrix.probe_sha256")
    if "effective_policy" in pixel_report:
        actual_policy = validate_ao_fi_pixel_policy(
            log, request.get("implementation"), request.get("xegtao_quality"),
            request.get("xegtao_denoise"), result.get("exit_code"), result.get("reason"))
        if pixel_report["effective_policy"] != actual_policy:
            raise ValueError("ao_fi_pixels.effective_policy does not match actual policy proof")
        if request.get("implementation") == "default":
            _validate_default_selection(result)
    elif request.get("implementation") == "xegtao":
        raise ValueError("explicit XeGTAO FI pixel run is missing effective_policy proof")
    else:
        # Older default captures predate effective_policy and retain the strict
        # result-side High2/full/R32 compatibility proof.
        _validate_default_selection(result)
    for marker in ERROR_MARKERS:
        if marker.casefold() in log.casefold():
            raise ValueError(f"run log contains error marker: {marker}")
    return result, request, sha, pixel_probe_sha, matrix_probe_sha


def _schedule_from_result(result: dict[str, Any], request: dict[str, Any]) -> tuple[int, int, int]:
    report = result.get("ao_fi_pixels")
    report_schedule = report.get("schedule") if isinstance(report, dict) else None
    value = request.get("ao_fi_pixels", report_schedule)
    schedule = parse_schedule(value)
    if report_schedule is not None and parse_schedule(report_schedule) != schedule:
        raise ValueError("result and request AO FI schedules differ")
    matrix_range = request.get("ao_fi_matrix_range")
    if matrix_range is not None and matrix_range != f"{schedule[0]}:{schedule[1]}":
        raise ValueError("AO FI matrix range does not match pixel schedule")
    if isinstance(report, dict) and report.get("phases") is not None and report["phases"] != list(FIXED_PHASES):
        raise ValueError("AO FI report does not prove fixed phases")
    return schedule


def _validate_request(result: dict[str, Any], request: dict[str, Any]) -> tuple[int, int, int]:
    schedule = _schedule_from_result(result, request)
    required = {
        "ao": 2, "aa": 0, "leaf_camera": True,
        "leaf_freeze": True, "unlocked_fps": True,
        "xegtao_resolution": "full", "xegtao_depth_storage": "r32float",
        "capture_stride": 1, "capture_delay": 0, "capture_trigger": "-",
    }
    for field, expected in required.items():
        if request.get(field) != expected:
            raise ValueError(f"AO FI pixel request field {field} must be {expected!r}")
    implementation = request.get("implementation")
    quality = request.get("xegtao_quality")
    denoise = request.get("xegtao_denoise")
    if implementation not in ("default", "xegtao"):
        raise ValueError("AO FI pixel request implementation must be default or xegtao")
    if implementation == "default":
        if quality != "high" or type(denoise) is not int or denoise != 2:
            raise ValueError("default AO FI pixel request must remain High/2")
    elif (quality not in ("low", "medium", "high", "ultra")
          or type(denoise) is not int or denoise not in (1, 2, 3)):
        raise ValueError("explicit XeGTAO AO FI pixel request has invalid quality/denoise")
    if not isinstance(request.get("unshaded_ao"), bool):
        raise ValueError("AO FI pixel request must explicitly set unshaded_ao")
    if not isinstance(request.get("leaf_camera_mode"), str) or not request["leaf_camera_mode"]:
        raise ValueError("AO FI pixel request must specify leaf_camera_mode")
    if (isinstance(request.get("tick_hz"), bool)
            or not isinstance(request.get("tick_hz"), (int, float))
            or request["tick_hz"] <= 0):
        raise ValueError("AO FI pixel request must be paced")
    expected_frames = (schedule[1] - schedule[0]) // schedule[2] + 1
    if request.get("capture_frames") != expected_frames:
        raise ValueError("capture_frames does not match the admitted AO FI schedule")
    return schedule


def _crosscheck(frame: dict[str, Any], row: dict[str, int]) -> None:
    for field in INDEX_FIELDS:
        if frame[field] != row[field]:
            raise ValueError(f"MFD/CSV metadata mismatch for seq {row['seq']}: {field}")


def load_capture(run: str | Path, *, require_replay: bool = True) -> dict[str, Any]:
    """Load MFD/CSV/proof and join each proof to exactly one native frame."""
    run_path = Path(run).resolve(strict=True)
    if not run_path.is_dir():
        raise ValueError(f"capture run is not a directory: {run_path}")
    try:
        log = (run_path / "game.log").read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ValueError(f"missing game.log: {run_path / 'game.log'}") from exc
    result, request, sha, pixel_probe_sha, matrix_probe_sha = _validate_result(run_path, log)
    schedule = _validate_request(result, request)
    frames_dir = run_path / "frames"
    frame_paths = sorted(frames_dir.glob("f*.mfd"))
    if not frame_paths:
        raise ValueError(f"no MFD frames found: {frames_dir}")
    if any(path.name != f"f{index:06d}.mfd" for index, path in enumerate(frame_paths)):
        raise ValueError("MFD files must be contiguous f%06d.mfd names")
    frames = [load_mfd(path) for path in frame_paths]
    capture_count = result.get("capture_count")
    if (isinstance(capture_count, bool) or not isinstance(capture_count, int)
            or capture_count != len(frames)):
        raise ValueError("result capture_count does not match MFD frame count")
    rows = load_index(frames_dir / "index.csv")
    if len(frames) != len(rows):
        raise ValueError("MFD and index.csv frame counts differ")
    for frame, row in zip(frames, rows):
        _crosscheck(frame, row)
        if require_replay and frame["replay"] != 1:
            raise ValueError(f"AO FI pixel capture requires replay=1: seq {frame['seq']}")
    proofs = parse_pixel_proof(log)
    if len(proofs) != capture_count:
        raise ValueError("result capture_count does not match AO-FI-PIXEL proof count")
    frame_by_key: dict[tuple[int, int, int], dict[str, Any]] = {}
    for frame in frames:
        key = (frame["seq"], frame["present_index"], frame["tick"])
        if key in frame_by_key:
            raise ValueError(f"duplicate native frame key: {key}")
        frame_by_key[key] = frame
    for path, row in zip(frame_paths, rows):
        if path.name != f"f{row['seq']:06d}.mfd":
            raise ValueError(f"MFD filename does not map to index.csv seq {row['seq']}")
    proof_by_key: dict[tuple[int, int, int], dict[str, Any]] = {}
    proof_ticks: set[int] = set()
    for proof in proofs:
        key = (proof["seq"], proof["presentIndex"], proof["captureViTick"])
        if key in proof_by_key:
            raise ValueError(f"duplicate AO-FI-PIXEL proof key: {key}")
        proof_by_key[key] = proof
        frame = frame_by_key.get(key)
        if frame is None:
            raise ValueError(f"AO-FI-PIXEL proof has no exact MFD frame: {key}")
        if frame["replay"] != 1:
            raise ValueError(f"AO-FI-PIXEL proof is not a replay frame: {key}")
        if proof["viTick"] in proof_ticks:
            raise ValueError(f"duplicate AO-FI-PIXEL viTick: {proof['viTick']}")
        proof_ticks.add(proof["viTick"])
        if _phase_code(proof["alpha"]) != _expected_phase(proof["viTick"], schedule):
            raise ValueError(f"AO-FI-PIXEL alpha phase mismatch at viTick {proof['viTick']}")
    unmatched_frames = sorted(set(frame_by_key) - set(proof_by_key))
    unmatched_proof = sorted(set(proof_by_key) - set(frame_by_key))
    if unmatched_frames or unmatched_proof:
        raise ValueError(f"capture/proof join incomplete: unmatched_frames={len(unmatched_frames)} unmatched_proof={len(unmatched_proof)}")
    start, end, stride = schedule
    expected_ticks = set(range(start, end + 1, stride))
    pairs = [{"key": key, "frame": frame_by_key[key], "proof": proof_by_key[key]}
             for key in sorted(frame_by_key)]
    return {
        "run_path": str(run_path), "result_path": str((run_path / "result.json").resolve()),
        "executable_sha256": sha, "request": request, "result": result,
        "ao_fi_pixels_probe_sha256": pixel_probe_sha,
        "ao_fi_matrix_probe_sha256": matrix_probe_sha,
        "effective_policy": result["ao_fi_pixels"].get("effective_policy"),
        "schedule": schedule, "frames": frames, "proofs": proofs, "pairs": pairs,
        "expected_schedule_ticks": len(expected_ticks),
        "captured_schedule_ticks": len(proof_ticks),
        "unmatched_schedule_ticks": len(expected_ticks - proof_ticks),
        "unmatched_frames": 0, "unmatched_proof": 0,
        "scope": "capture/provenance join only; not pixel quality, natural cadence, or flicker acceptance",
    }


PAIR_REQUEST_EXCLUDED = frozenset({"name", "cache_seed", "unshaded_ao"})
EFFECTIVE_POLICY_FIELDS = (
    "implementation", "quality", "denoise_passes", "resolution", "depth_storage",
)


def _normalize(value: Any) -> Any:
    if isinstance(value, list):
        return tuple(_normalize(item) for item in value)
    if isinstance(value, dict):
        return tuple(sorted((key, _normalize(item)) for key, item in value.items()))
    return value


def _fixture_signature(request: dict[str, Any]) -> tuple[Any, ...]:
    # The run name/cache seed are per-process bookkeeping.  Everything else
    # in the admitted request is fixture or capture configuration and must
    # match exactly; comparing the complete normalized object avoids silently
    # forgetting a newly added runner knob.
    return _normalize({key: value for key, value in request.items()
                       if key not in PAIR_REQUEST_EXCLUDED})


def _effective_policy_signature(policy: Any) -> tuple[Any, ...] | None:
    """Compare policy identity while ignoring per-run conserved-pass counts."""
    if policy is None:
        return None
    if not isinstance(policy, dict):
        raise ValueError("effective AO FI policy must be an object")
    try:
        return tuple(policy[field] for field in EFFECTIVE_POLICY_FIELDS)
    except KeyError as exc:
        raise ValueError("effective AO FI policy is missing semantic fields") from exc


def _matrix_signature(matrix: dict[str, Any]) -> tuple[Any, ...]:
    return (
        matrix["index"], matrix["object"], matrix["name"], _normalize(matrix["key"]),
        _normalize(matrix["previousKey"]), tuple(matrix["currentBits"]),
        tuple(matrix["previousBits"]), tuple(matrix["outputBits"]),
        tuple(matrix["cameraViewBits"]),
        tuple(matrix["cameraBits"]), matrix["submitted"],
    )


def _compare_phase(left: dict[str, Any], right: dict[str, Any], key: tuple[int, int, float]) -> None:
    for field in ("interpolationEnabled", "matrixCount", "hintOverflow", "previousHintOverflow"):
        if left[field] != right[field]:
            raise ValueError(f"same-phase proof mismatch at {key}: {field}")
    left_matrices = {item["index"]: item for item in left["matrices"]}
    right_matrices = {item["index"]: item for item in right["matrices"]}
    if set(left_matrices) != set(right_matrices):
        raise ValueError(f"same-phase matrix index mismatch at {key}")
    for index in sorted(left_matrices):
        if _matrix_signature(left_matrices[index]) != _matrix_signature(right_matrices[index]):
            raise ValueError(f"same-phase camera/matrix bits mismatch at {key}, index {index}")


def pair_captures(left: dict[str, Any] | str | Path,
                  right: dict[str, Any] | str | Path, *, min_shared_ticks: int = 8,
                  require_all_phases: bool = True) -> dict[str, Any]:
    """Validate explicit shaded/unshaded runs and return frame/proof pairs."""
    if isinstance(left, (str, Path)):
        left = load_capture(left)
    if isinstance(right, (str, Path)):
        right = load_capture(right)
    if not isinstance(left, dict) or not isinstance(right, dict):
        raise TypeError("pair_captures expects run paths or capture dictionaries")
    if min_shared_ticks < 0:
        raise ValueError("min_shared_ticks must be nonnegative")
    if left["executable_sha256"] != right["executable_sha256"]:
        raise ValueError("paired runs use different executable sha256 values")
    for field in ("ao_fi_pixels_probe_sha256", "ao_fi_matrix_probe_sha256"):
        if left[field] != right[field]:
            raise ValueError(f"paired runs use different {field} values")
    if (_effective_policy_signature(left.get("effective_policy"))
            != _effective_policy_signature(right.get("effective_policy"))):
        raise ValueError("paired runs use different effective AO FI policies")
    if left["request"].get("unshaded_ao") == right["request"].get("unshaded_ao"):
        raise ValueError("paired runs must contain one unshaded_ao=true and one false")
    if _fixture_signature(left["request"]) != _fixture_signature(right["request"]):
        raise ValueError("paired runs have different fixture inputs or phase schedule")
    if left["schedule"] != right["schedule"]:
        raise ValueError("paired runs have different AO FI schedules")

    def phase_map(capture: dict[str, Any]) -> dict[tuple[int, int, float], dict[str, Any]]:
        mapped: dict[tuple[int, int, float], dict[str, Any]] = {}
        by_vi_tick: dict[int, tuple[int, int, float]] = {}
        for proof in capture["proofs"]:
            key = (proof["viTick"], proof["previousViTick"], proof["alpha"])
            if key in mapped:
                raise ValueError(f"duplicate same-phase proof key: {key}")
            previous = by_vi_tick.get(proof["viTick"])
            if previous is not None and previous != key:
                raise ValueError(f"duplicate phase proof for viTick: {proof['viTick']}")
            by_vi_tick[proof["viTick"]] = key
            mapped[key] = proof
        return mapped

    left_map, right_map = phase_map(left), phase_map(right)
    left_by_tick = {key[0]: key for key in left_map}
    right_by_tick = {key[0]: key for key in right_map}
    for tick in sorted(set(left_by_tick) & set(right_by_tick)):
        if left_by_tick[tick] != right_by_tick[tick]:
            raise ValueError(f"same viTick has mismatched previousViTick/alpha: {tick}")
    common = sorted(set(left_map) & set(right_map))
    common_ticks = {key[0] for key in common}
    if len(common_ticks) < min_shared_ticks:
        raise ValueError(f"insufficient shared same-phase ticks: {len(common_ticks)} < {min_shared_ticks}; "
                         f"unmatched_left={len(set(left_map)-set(right_map))} "
                         f"unmatched_right={len(set(right_map)-set(left_map))}")
    phases = sorted({_phase_code(key[2]) for key in common})
    if require_all_phases and phases != [0, 1, 2, 3]:
        raise ValueError(f"same-phase pair is missing fixed phases: {phases}")
    left_frames = {item["key"]: item["frame"] for item in left["pairs"]}
    right_frames = {item["key"]: item["frame"] for item in right["pairs"]}
    matches = []
    for key in common:
        _compare_phase(left_map[key], right_map[key], key)
        left_key = (left_map[key]["seq"], left_map[key]["presentIndex"], left_map[key]["captureViTick"])
        right_key = (right_map[key]["seq"], right_map[key]["presentIndex"], right_map[key]["captureViTick"])
        left_frame, right_frame = left_frames.get(left_key), right_frames.get(right_key)
        if left_frame is None or right_frame is None:
            raise ValueError(f"same-phase proof has no joined frame path: {key}")
        for field in ("width", "height", "src_width", "src_height", "crop_x", "crop_y",
                      "crop_w", "crop_h", "downscale", "format"):
            if left_frame[field] != right_frame[field]:
                raise ValueError(f"same-phase frame metadata mismatch at {key}: {field}")
        matches.append({"phase_key": key, "left_frame": left_frame["path"],
                        "right_frame": right_frame["path"], "left_proof": left_map[key],
                        "right_proof": right_map[key]})
    return {
        "scope": "capture/provenance join only; fixed-phase diagnostic, not pixel quality, natural cadence, or flicker acceptance",
        "executable_sha256": left["executable_sha256"], "schedule": left["schedule"],
        "ao_fi_pixels_probe_sha256": left["ao_fi_pixels_probe_sha256"],
        "ao_fi_matrix_probe_sha256": left["ao_fi_matrix_probe_sha256"],
        "effective_policy": left.get("effective_policy"),
        "shared_ticks": len(common_ticks), "shared_phase_pairs": len(common),
        "shared_phases": [FIXED_PHASES[index] for index in phases],
        "required_shared_ticks": min_shared_ticks,
        "unmatched_left": len(set(left_map) - set(right_map)),
        "unmatched_right": len(set(right_map) - set(left_map)),
        "unmatched_left_ticks": len(set(left_by_tick) - set(right_by_tick)),
        "unmatched_right_ticks": len(set(right_by_tick) - set(left_by_tick)),
        "unmatched_left_schedule_ticks": left["unmatched_schedule_ticks"],
        "unmatched_right_schedule_ticks": right["unmatched_schedule_ticks"],
        "pairs": matches,
    }


validate_pair = pair_captures
load_ao_fi_capture = load_capture


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--min-shared-ticks", type=int, default=8)
    parser.add_argument("--allow-missing-phases", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    try:
        report = pair_captures(args.left, args.right, min_shared_ticks=args.min_shared_ticks,
                               require_all_phases=not args.allow_missing_phases)
    except (OSError, TypeError, ValueError) as exc:
        parser.error(str(exc))
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report is not None:
        args.report.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
