"""Admission and evidence checks for the QA-only XeGTAO buffer capture.

The native side writes a deliberately small, fixed-schema bundle: two
consecutive camera-zero jobs, eleven tightly packed R32 planes per job, and an
``index.json`` receipt.  This module is intentionally read-only.  It admits
the runner configuration and authenticates the resulting bundle, but it does
not interpret pixels or turn the dump into quality/performance evidence.
"""

from __future__ import annotations

from collections.abc import Mapping
import hashlib
import json
import math
from pathlib import Path
import re
from typing import Any


EXPECTED_EXECUTABLE = Path("build/ao-buffer-qa/release/mp6native.exe")
BUILD_READY = Path("build/ao-buffer-qa/build-ready.json")

_MIN_TICK = 3000
_MAX_TICK = 1_000_000
_JOB_COUNT = 2
_PLANES_PER_JOB = 11
_MAX_CAPTURE_BYTES = 96 * 1024 * 1024

# This order is part of the native writer's file/index contract.
_STAGES = (
    ("clean_depth", 0),
    ("canonical_depth", 0),
    ("depth_mip", 0),
    ("depth_mip", 1),
    ("depth_mip", 2),
    ("depth_mip", 3),
    ("depth_mip", 4),
    ("normals", 0),
    ("raw_ao", 0),
    ("edges", 0),
    ("final_ao", 0),
)
_FLOAT_STAGES = {"clean_depth", "canonical_depth", "depth_mip", "edges"}
_UINT_STAGES = {"normals", "raw_ao", "final_ao"}
_U32_FIELDS = {
    "params_bits": 20,
    "constants_bits": 24,
    "adapter_bits": 12,
}
_INDEX_FIELDS = {"version", "jobs", "complete"}
_JOB_FIELDS = {
    "frame", "tick", "camera", "width", "height",
    "params_bits", "constants_bits", "adapter_bits", "planes",
}
_PLANE_FIELDS = {"stage", "mip", "width", "height", "format", "file", "bytes"}
_HASH_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_COMPLETE_RE = re.compile(
    r"\[AO-BUFFERS\] jobs=(?P<jobs>[0-9]+) planes=(?P<planes>[0-9]+) "
    r"complete=(?P<complete>[0-9]+) bytes=(?P<bytes>[0-9]+)"
)


def _resolved(root: Path, value: str | Path) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else root / path).resolve()


def _is_number(value: Any) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(float(value)))


def _disallowed(args: Any, names: tuple[str, ...]) -> str | None:
    for name in names:
        if getattr(args, name, False):
            return name
    return None


def buffer_capture_error(args: Any, root: str | Path) -> str | None:
    """Return an admission error for the opt-in native AO buffer capture.

    ``None`` is the normal production path: an absent tick leaves the helper
    completely inert.  The error text is suitable for ``argparse``.
    """

    tick = getattr(args, "ao_buffers_tick", None)
    if tick is None:
        return None
    if isinstance(tick, bool) or not isinstance(tick, int):
        return "--ao-buffers-tick must be an integer"

    ticks = getattr(args, "ticks", None)
    if isinstance(ticks, bool) or not isinstance(ticks, int):
        return "--ao-buffers-tick requires an integer --ticks"
    if not _MIN_TICK <= tick < ticks - 1 or tick > _MAX_TICK:
        return "--ao-buffers-tick requires 3000 <= TICK < --ticks-1 and TICK <= 1000000"

    tick_hz = getattr(args, "tick_hz", None)
    if not _is_number(tick_hz) or float(tick_hz) <= 0:
        return "--ao-buffers-tick requires paced --tick-hz > 0"
    if getattr(args, "unlocked_fps", False):
        return "--ao-buffers-tick cannot combine with --unlocked-fps"

    expected = _resolved(Path(root), EXPECTED_EXECUTABLE)
    requested_exe = getattr(args, "exe", None)
    if requested_exe is None or _resolved(Path(root), requested_exe) != expected:
        return "--ao-buffers-tick requires build/ao-buffer-qa/release/mp6native.exe"

    required = (
        ("ao", 2),
        ("implementation", "xegtao"),
        ("xegtao_quality", "high"),
        ("xegtao_denoise", 2),
        ("xegtao_resolution", "full"),
        ("xegtao_depth_storage", "r32float"),
    )
    for name, value in required:
        if getattr(args, name, None) != value:
            return f"--ao-buffers-tick requires {name}={value!r}"

    # These seams can alter the source textures, scheduling, or replay state,
    # so a successful bundle must not silently be attributed to the canonical
    # AO configuration.
    flag = _disallowed(args, (
        "xegtao_batch_compute", "xegtao_prepared_canonical", "xegtao_init_failure",
        "resize_ao", "raw_ao_depth", "unshaded_ao", "foreground_ao", "water_ao",
    ))
    if flag is not None:
        return f"--ao-buffers-tick cannot combine with {flag}"

    for name in (
        "ao_cycle", "ao_fi_matrix_range", "ao_fi_pixels", "deferred_capture",
        "save_at", "load_at", "state_path",
    ):
        if getattr(args, name, None) is not None:
            return f"--ao-buffers-tick cannot combine with {name}"
    if getattr(args, "ao_fi_matrix_limit", 256) != 256:
        return "--ao-buffers-tick cannot combine with ao_fi_matrix_limit"
    return None


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _raise(message: str) -> None:
    raise ValueError(message)


def _read_json(path: Path, label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid {label}: {exc}") from exc


def _hash_value(value: Any) -> str | None:
    if isinstance(value, str) and _HASH_RE.fullmatch(value):
        return value.lower()
    if isinstance(value, Mapping):
        for key in ("sha256", "hash", "digest"):
            candidate = value.get(key)
            if isinstance(candidate, str) and _HASH_RE.fullmatch(candidate):
                return candidate.lower()
    return None


def _hash_map(section: Mapping[str, Any], aliases: tuple[str, ...], label: str) -> dict[str, str]:
    raw = None
    selected = None
    for name in aliases:
        if name in section:
            raw = section[name]
            selected = name
            break
    if not isinstance(raw, Mapping) or not raw:
        _raise(f"ao_buffers build receipt has no {label} hash map")
    result: dict[str, str] = {}
    for name, value in raw.items():
        digest = _hash_value(value)
        if not isinstance(name, str) or not name or digest is None:
            _raise(f"invalid {label} hash entry {name!r}")
        result[name] = digest
    if selected is None:  # pragma: no cover - guarded above, keeps type checkers honest
        _raise(f"ao_buffers build receipt has no {label} hash map")
    return result


def _manifest_path(root: Path, ready_parent: Path, name: str) -> Path:
    raw = Path(name)
    choices = [raw] if raw.is_absolute() else [root / raw, ready_parent / raw]
    root_resolved = root.resolve()
    for choice in choices:
        candidate = choice.resolve()
        if candidate.is_file():
            if not candidate.is_relative_to(root_resolved):
                _raise(f"build receipt path escapes repository: {name}")
            return candidate
    _raise(f"build receipt artifact is missing: {name}")


def _validate_receipt_hashes(root: Path, ready_path: Path,
                             section: Mapping[str, Any]) -> tuple[dict[str, str], dict[str, str]]:
    source_aliases = ("sources", "source_hashes")
    generated_aliases = (
        "generated_sources", "private_generated_sources", "generated",
        "generated_hashes", "private_generated_hashes",
    )
    source_maps = [_hash_map(section, (name,), "source")
                   for name in source_aliases if name in section]
    generated_maps = [_hash_map(section, (name,), "generated-source")
                      for name in generated_aliases if name in section]
    sources = source_maps[0] if source_maps else _hash_map(section, source_aliases, "source")
    generated = (generated_maps[0] if generated_maps else
                 _hash_map(section, generated_aliases, "generated-source"))
    if any(candidate != sources for candidate in source_maps[1:]):
        _raise("ao_buffers source hash maps disagree")
    if any(candidate != generated for candidate in generated_maps[1:]):
        _raise("ao_buffers generated-source hash maps disagree")
    for label, entries in (("source", sources), ("generated source", generated)):
        for name, expected in entries.items():
            artifact = _manifest_path(root, ready_path.parent, name)
            actual = _sha256(artifact)
            if actual != expected:
                _raise(f"stale {label} build receipt hash: {name}")
    return sources, generated


def _validate_receipt_executable(root: Path, ready_path: Path,
                                 section: Mapping[str, Any],
                                 binary_hash: str) -> None:
    expected_path = _resolved(root, EXPECTED_EXECUTABLE)
    candidates: list[tuple[Path | None, str | None]] = []
    declared_executables: list[Path] = []

    def receipt_path(name: str) -> Path:
        raw = Path(name)
        if raw.is_absolute():
            return raw.resolve()
        # Existing build receipts use absolute protected-executable paths, but
        # accepting the output-relative spelling keeps the schema portable.
        for base in (root, ready_path.parent):
            candidate = (base / raw).resolve()
            if candidate == expected_path or candidate.is_file():
                return candidate
        return (root / raw).resolve()

    for key in ("protected_executables", "executables"):
        raw = section.get(key)
        if isinstance(raw, Mapping):
            for name, value in raw.items():
                if isinstance(name, str):
                    candidates.append((receipt_path(name), _hash_value(value)))

    for key in ("executable", "exe"):
        value = section.get(key)
        if isinstance(value, Mapping):
            path = value.get("path", value.get("file", value.get("name")))
            digest = _hash_value(value)
            if isinstance(path, str):
                declared = receipt_path(path)
                declared_executables.append(declared)
                candidates.append((declared, digest))
        elif isinstance(value, str) and _HASH_RE.fullmatch(value):
            candidates.append((None, value.lower()))
        elif isinstance(value, str):
            declared_executables.append(receipt_path(value))

    if any(path != expected_path for path in declared_executables):
        _raise("ao_buffers build receipt names the wrong executable")

    for key in ("executable_sha256", "exe_sha256", "sha256"):
        digest = _hash_value(section.get(key))
        if digest is not None:
            candidates.append((declared_executables[0] if key == "sha256" and declared_executables
                               else None, digest))

    matching = [digest for path, digest in candidates
                if (path is None or path == expected_path) and digest is not None]
    if not matching:
        _raise("ao_buffers build receipt has no attestation for the exact executable")
    if any(digest != binary_hash for digest in matching):
        _raise("ao_buffers build receipt executable hash mismatch")


def _capture_directory(directory: str | Path) -> Path:
    base = Path(directory).resolve()
    if not base.is_dir():
        _raise("AO buffer capture directory is missing")
    candidates = (base / "ao-buffers", base / "ao_buffers", base)
    found = [candidate for candidate in candidates
             if candidate.is_dir() and (candidate / "index.json").is_file()]
    if not found:
        _raise("AO buffer capture index.json is missing")
    return found[0]


def _u32_list(value: Any, field: str) -> list[int]:
    if not isinstance(value, list) or len(value) != _U32_FIELDS[field]:
        _raise(f"{field} must contain exactly {_U32_FIELDS[field]} raw u32 values")
    if any(type(item) is not int or not 0 <= item <= 0xFFFFFFFF for item in value):
        _raise(f"{field} contains a value outside raw u32 range")
    return list(value)


def _positive_int(value: Any, field: str) -> int:
    if type(value) is not int or value <= 0:
        _raise(f"{field} must be a positive integer")
    return value


def _safe_plane_file(capture: Path, name: Any, expected: str) -> Path:
    if not isinstance(name, str) or name != expected:
        _raise(f"unexpected AO buffer file name: {name!r}")
    candidate = (capture / name).resolve()
    if not candidate.is_relative_to(capture.resolve()):
        _raise("AO buffer file escapes capture directory")
    if not candidate.is_file():
        _raise(f"AO buffer file is missing: {name}")
    return candidate


def _validate_index(capture: Path, index: Mapping[str, Any], requested_tick: int) -> dict[str, Any]:
    if (set(index) != _INDEX_FIELDS or type(index.get("version")) is not int or
            index.get("version") != 1 or index.get("complete") is not True):
        _raise("AO buffer index schema/version/completion mismatch")
    jobs = index.get("jobs")
    if not isinstance(jobs, list) or len(jobs) != _JOB_COUNT:
        _raise("AO buffer index must contain exactly two jobs")

    frames: list[int] = []
    records: list[dict[str, Any]] = []
    plane_hashes: dict[str, str] = {}
    expected_files = {"index.json"}
    dimensions: tuple[int, int] | None = None
    total_bytes = 0

    for job_index, job in enumerate(jobs):
        if not isinstance(job, Mapping) or set(job) != _JOB_FIELDS:
            _raise(f"AO buffer job {job_index} schema mismatch")
        frame = _positive_int(job.get("frame"), f"job {job_index} frame")
        tick = job.get("tick")
        if type(tick) is not int or tick != requested_tick + job_index:
            _raise("AO buffer jobs must be requested tick and requested tick+1")
        if job.get("camera") != 0 or type(job.get("camera")) is not int:
            _raise("AO buffer jobs must use camera 0")
        width = _positive_int(job.get("width"), f"job {job_index} width")
        height = _positive_int(job.get("height"), f"job {job_index} height")
        if dimensions is None:
            dimensions = (width, height)
        elif dimensions != (width, height):
            _raise("AO buffer job dimensions changed between consecutive ticks")

        metadata: dict[str, Any] = {"frame": frame, "tick": tick, "camera": 0,
                                    "width": width, "height": height}
        for field in _U32_FIELDS:
            metadata[field] = _u32_list(job.get(field), field)

        planes = job.get("planes")
        if not isinstance(planes, list) or len(planes) != _PLANES_PER_JOB:
            _raise(f"AO buffer job {job_index} must contain exactly eleven planes")
        plane_records: list[dict[str, Any]] = []
        for plane_index, plane in enumerate(planes):
            if not isinstance(plane, Mapping) or set(plane) != _PLANE_FIELDS:
                _raise(f"AO buffer plane {job_index}:{plane_index} schema mismatch")
            stage, mip = _STAGES[plane_index]
            if (plane.get("stage") != stage or type(plane.get("mip")) is not int or
                    plane.get("mip") != mip):
                _raise(f"AO buffer plane order mismatch at {job_index}:{plane_index}")
            plane_width = _positive_int(plane.get("width"), "AO buffer plane width")
            plane_height = _positive_int(plane.get("height"), "AO buffer plane height")
            expected_width = width >> mip
            expected_height = height >> mip
            if (plane_width, plane_height) != (expected_width, expected_height):
                _raise(f"AO buffer MIP extent mismatch at {job_index}:{plane_index}")
            expected_format = "r32float" if stage in _FLOAT_STAGES else "r32uint"
            if plane.get("format") != expected_format:
                _raise(f"AO buffer format mismatch at {job_index}:{plane_index}")
            expected_bytes = plane_width * plane_height * 4
            if type(plane.get("bytes")) is not int or plane["bytes"] != expected_bytes:
                _raise(f"AO buffer byte count mismatch at {job_index}:{plane_index}")
            if expected_bytes <= 0:
                _raise("AO buffer byte count must be positive")
            total_bytes += expected_bytes
            if total_bytes > _MAX_CAPTURE_BYTES:
                _raise("AO buffer capture exceeds the 96 MiB native readback budget")
            expected_name = f"j{job_index:03d}_{stage}_m{mip}.bin"
            path = _safe_plane_file(capture, plane.get("file"), expected_name)
            if path.stat().st_size != expected_bytes:
                _raise(f"AO buffer file size mismatch: {expected_name}")
            digest = _sha256(path)
            plane_hashes[path.name] = digest
            expected_files.add(path.name)
            plane_records.append({
                "stage": stage, "mip": mip, "width": plane_width,
                "height": plane_height, "format": expected_format,
                "file": path.name, "bytes": expected_bytes, "sha256": digest,
            })
        frames.append(frame)
        records.append(dict(metadata, planes=plane_records))

    if len(set(frames)) != len(frames):
        _raise("AO buffer job frames must be unique")
    entries = list(capture.iterdir())
    for entry in entries:
        if entry.name not in expected_files or not entry.is_file():
            _raise(f"unexpected file in AO buffer capture directory: {entry.name}")
    if len(plane_hashes) != _JOB_COUNT * _PLANES_PER_JOB:
        _raise("AO buffer plane inventory is incomplete")
    return {
        "jobs": records,
        "frames": frames,
        "dimensions": {"width": dimensions[0], "height": dimensions[1]},
        "plane_sha256": plane_hashes,
        "tight_bytes": total_bytes,
    }


def validate_buffer_capture(
    args: Any,
    root: str | Path,
    directory: str | Path,
    executable: str | Path,
    binary_hash: str,
    exit_code: int,
    reason: str,
    text: str,
) -> dict[str, Any] | None:
    """Validate one completed native capture and return diagnostic metadata."""

    tick = getattr(args, "ao_buffers_tick", None)
    if tick is None:
        return None
    admission = buffer_capture_error(args, root)
    if admission is not None:
        _raise(f"AO buffer capture was not admitted: {admission}")
    if exit_code != 0 or reason != "exited":
        _raise(f"AO buffer capture did not exit cleanly: exit={exit_code} reason={reason}")
    if not isinstance(text, str):
        _raise("AO buffer capture log must be text")
    if "[AO-BUFFERS-ERROR]".casefold() in text.casefold():
        _raise("native AO buffer capture reported [AO-BUFFERS-ERROR]")
    marker_lines = [line.rstrip("\r") for line in text.splitlines()
                    if line.startswith("[AO-BUFFERS]")]
    if len(marker_lines) != 1:
        _raise("AO buffer capture requires exactly one completion marker")
    marker = _COMPLETE_RE.fullmatch(marker_lines[0])
    if marker is None:
        _raise("malformed AO buffer completion marker")
    marker_values = {key: int(value) for key, value in marker.groupdict().items()}
    if (marker.group("jobs"), marker.group("planes"), marker.group("complete")) != ("2", "22", "1"):
        _raise("AO buffer completion marker is incomplete")
    if not 0 < marker_values["bytes"] <= _MAX_CAPTURE_BYTES:
        _raise("AO buffer completion marker exceeds the native readback budget")

    root_path = Path(root).resolve()
    expected_executable = _resolved(root_path, EXPECTED_EXECUTABLE)
    actual_executable = _resolved(root_path, executable)
    if actual_executable != expected_executable:
        _raise("AO buffer executable path is not the exact private QA executable")
    if not actual_executable.is_file():
        _raise("AO buffer executable is missing")
    if not isinstance(binary_hash, str) or _HASH_RE.fullmatch(binary_hash) is None:
        _raise("AO buffer executable hash is malformed")
    actual_binary_hash = _sha256(actual_executable)
    if actual_binary_hash != binary_hash.lower():
        _raise("AO buffer executable hash does not match the executable")

    ready_path = root_path / BUILD_READY
    if not ready_path.is_file():
        _raise("AO buffer build-ready.json is missing")
    ready = _read_json(ready_path, "AO buffer build-ready.json")
    section = ready.get("ao_buffers") if isinstance(ready, Mapping) else None
    if not isinstance(section, Mapping):
        _raise("build-ready.json has no ao_buffers provenance")
    source_hashes, generated_hashes = _validate_receipt_hashes(root_path, ready_path, section)
    _validate_receipt_executable(root_path, ready_path, section, binary_hash.lower())

    capture = _capture_directory(directory)
    index_path = capture / "index.json"
    index_bytes = index_path.read_bytes()
    index = _read_json(index_path, "AO buffer index.json")
    if not isinstance(index, Mapping):
        _raise("AO buffer index.json must be an object")
    evidence = _validate_index(capture, index, tick)
    evidence.update({
        "requested_tick": tick,
        "marker_bytes": marker_values["bytes"],
        "index_sha256": hashlib.sha256(index_bytes).hexdigest(),
        "binary_sha256": actual_binary_hash,
        "build_ready_sha256": _sha256(ready_path),
        "source_sha256": source_hashes,
        "generated_source_sha256": generated_hashes,
        "executable": str(expected_executable),
        "scope": (
            "Diagnostic-only native XeGTAO intermediate texture readback for two "
            "consecutive camera-zero ticks; not pixel-quality, cadence, performance, "
            "or release evidence."
        ),
    })
    return evidence
