"""CPU-only comparison of two native leaf-camera mode captures.

The native QA runner writes ``[LEAF-CAMERA]`` JSON records to ``game.log``.
This utility consumes two already-captured run directories (an unshaded
legacy control and a XeGTAO run), and never starts the game, a GPU
device, or an Android toolchain.  It registers a bounded ground ROI in world
space and reports display-RGB motion statistics.  The report is deliberately
limited to the visible receiver ROI; it is not a whole-plant quality score.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
DEPENDENCIES = ROOT / "build" / "ao-gpu-test-deps"
if str(DEPENDENCIES) not in sys.path:
    sys.path.insert(0, str(DEPENDENCIES))
if str(ROOT / "tests" / "integration") not in sys.path:
    sys.path.insert(0, str(ROOT / "tests" / "integration"))

from analyze_ao_leaf_motion import Camera, bilinear, motion_stats, np


PLANE_Y = 5.132045198
RAW_BOARD_ASPECT = 1.2
DEFAULT_ROI = (300, 385, 980, 490)
ASPECT_TOLERANCE = 5e-4
FOV_TOLERANCE = 2e-4
MATRIX_TOLERANCE = 2e-3
ROUNDTRIP_TOLERANCE = 1e-6
POSE_TOLERANCE = 1e-5
REQUIRED_PHASE_FIELDS = (
    "window_size", "capture_frames", "capture_stride", "capture_trigger",
    "capture_delay", "ticks", "input_script", "widescreen", "leaf_camera",
    "leaf_camera_mode", "geometry_at",
)
PHASE_FIELDS = REQUIRED_PHASE_FIELDS + ("leaf_freeze", "geometry_at_second")

FREEZE_ASSET = 0x00D70006
FREEZE_ASSET_TEXT = f"0x{FREEZE_ASSET:08X}"
FREEZE_ROOT = "b01_m002"
FREEZE_TIME = 7.0
FREEZE_SPEED = 0.0
FREEZE_WORLD_TOLERANCE = 1e-3
FREEZE_LOCAL_TOLERANCE = 1e-7
FREEZE_TARGET_NAMES = ("flower_a", "fa_c2", "a_ff1", "a_ha1", "a_ha2")
FREEZE_FACE_TYPES = (2, 3)
VISIBILITY_EPSILON = 1e-8
REPEATED_POSE_PERIOD_TICKS = 120


@dataclass(frozen=True)
class LeafCameraRecord:
    """One self-contained native ``[LEAF-CAMERA]`` sample."""

    tick: int
    relative_tick: int
    mode: str
    fov: float
    aspect: float
    eye: tuple[float, float, float]
    center: tuple[float, float, float]


@dataclass(frozen=True)
class LeafFreezeRecord:
    """The resolved frozen-plant selection emitted by ``[LEAF-FREEZE]``."""

    asset: str
    root: str
    resolved_model: int
    time: float
    speed: float


def _run_result_path(run: Path) -> Path:
    return run / "result.json"


def _window_size(value) -> tuple[int, int]:
    if not isinstance(value, str) or "x" not in value:
        raise ValueError("result request window_size is missing or malformed")
    width_text, height_text = value.lower().split("x", 1)
    width = _integer(width_text, "window width")
    height = _integer(height_text, "window height")
    if width < 2 or height < 2:
        raise ValueError("result request window_size must be at least 2x2")
    return width, height


def _require_request_fields(request: dict, fields: tuple[str, ...]) -> None:
    missing = [field for field in fields if field not in request]
    if missing:
        raise ValueError(f"result request missing {', '.join(missing)}")


_XE_POLICY_RE = re.compile(
    r"\[MP6-AO\]\s+XeGTAO\s+(?:experimental|reference):\s+"
    r"(?P<resolution>full)-resolution\s+direct\s+uint\s+output,\s+"
    r"(?P<quality>\w+),\s+(?P<denoise>\d+)\s+spatial denoise passes",
    re.IGNORECASE,
)
_XE_DEPTH_RE = re.compile(
    r"\[MP6-AO\]\s+XeGTAO depth storage:\s+requested=(?P<requested>\w+)\s+"
    r"actual=(?P<actual>\w+)",
    re.IGNORECASE,
)


def _normalize_xe_quality(value) -> str:
    quality = str(value).strip().lower()
    if quality not in {"high", "ultra"}:
        raise ValueError("XeGTAO quality must be high or ultra")
    return quality


def _normalize_xe_denoise(value) -> int:
    denoise = _integer(value, "XeGTAO denoise passes")
    if denoise < 0:
        raise ValueError("XeGTAO denoise passes must be non-negative")
    return denoise


def _xe_policy_role(quality, denoise) -> str:
    return f"xe_{_normalize_xe_quality(quality)}{_normalize_xe_denoise(denoise)}"


def _xe_policy_from_role(role: str) -> tuple[str, int]:
    match = re.fullmatch(r"xe_(high|ultra)([0-9]+)", str(role).lower())
    if not match:
        raise ValueError(f"unknown Xe provenance role: {role}")
    return match.group(1), int(match.group(2))


def validate_xe_policy_log(path: Path, *, quality: str = "ultra",
                           denoise: int = 3, resolution: str = "full",
                           depth_storage: str = "r32float") -> dict:
    """Require native log evidence for the requested XeGTAO policy.

    ``result.json`` records the request and the depth-storage handshake, but
    the quality/denoise policy is also asserted from the run's actual log so
    an analyzer cannot label a different Xe configuration as Ultra3.
    """

    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ValueError(f"missing Xe policy log: {path}") from exc
    policies = list(_XE_POLICY_RE.finditer(text))
    if not policies:
        raise ValueError(f"Xe policy log has no quality/denoise record: {path}")
    expected_quality = str(quality).lower()
    expected_resolution = str(resolution).lower()
    for match in policies:
        values = match.groupdict()
        if (values["quality"].lower() != expected_quality or
                values["resolution"].lower() != expected_resolution or
                int(values["denoise"]) != int(denoise)):
            raise ValueError(
                f"Xe policy log mismatch: quality={values['quality']} "
                f"denoise={values['denoise']} resolution={values['resolution']}"
            )
    depths = list(_XE_DEPTH_RE.finditer(text))
    if not depths:
        raise ValueError(f"Xe policy log has no depth-storage record: {path}")
    for match in depths:
        values = match.groupdict()
        if (values["requested"].lower() != str(depth_storage).lower() or
                values["actual"].lower() != str(depth_storage).lower()):
            raise ValueError(
                f"Xe depth-storage log mismatch: requested={values['requested']} "
                f"actual={values['actual']}"
            )
    return {
        "quality": expected_quality,
        "denoise": int(denoise),
        "resolution": expected_resolution,
        "depth_storage_requested": str(depth_storage).lower(),
        "depth_storage_actual": str(depth_storage).lower(),
        "policy_records": len(policies),
        "depth_storage_records": len(depths),
    }


def load_run_provenance(run: Path, role: str, *, xe_quality: str | None = None,
                        xe_denoise: int | None = None) -> dict:
    """Validate and summarize the immutable request/result provenance."""

    path = _run_result_path(Path(run))
    if not path.is_file():
        raise ValueError(f"missing run result.json: {path}")
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot parse run result.json: {path}") from exc
    if not isinstance(result, dict) or not isinstance(result.get("request"), dict):
        raise ValueError(f"run result has no request object: {path}")
    sha = result.get("sha256")
    if not isinstance(sha, str) or not sha:
        raise ValueError(f"run result has no executable sha256: {path}")
    if result.get("exit_code") != 0 or result.get("crash_marker") is not False:
        raise ValueError(f"run did not complete cleanly: {path}")
    request = result["request"]
    logged_policy = None
    _require_request_fields(
        request, REQUIRED_PHASE_FIELDS + ("implementation", "unshaded_ao", "ao"))
    if type(request["ao"]) is not int or request["ao"] != 2:
        raise ValueError(
            "leaf-camera comparison requires request ao to be strict integer 2")
    # These keys were added for the frozen-plant harness.  Missing values are
    # deliberately the animated-run defaults so existing reports cannot be
    # relabeled as frozen merely because they predate the fields.
    leaf_freeze = request.get("leaf_freeze", False)
    if not isinstance(leaf_freeze, bool):
        raise ValueError("result request leaf_freeze must be boolean when present")
    geometry_at_second = request.get("geometry_at_second")
    if geometry_at_second is not None:
        geometry_at_second = _integer(geometry_at_second, "geometry_at_second")
    if leaf_freeze and geometry_at_second is None:
        raise ValueError("frozen result request must contain geometry_at_second")
    width, height = _window_size(request["window_size"])
    if request["leaf_camera"] is not True:
        raise ValueError(f"run is missing leaf-camera capture: {path}")
    expected_quality = expected_denoise = None
    if role == "neutral":
        if request["implementation"] != "legacy" or request["unshaded_ao"] is not True:
            raise ValueError("neutral run must be legacy with unshaded_ao=true")
    elif str(role).lower().startswith("xe_"):
        role_quality, role_denoise = _xe_policy_from_role(role)
        expected_quality = _normalize_xe_quality(
            role_quality if xe_quality is None else xe_quality)
        expected_denoise = _normalize_xe_denoise(
            role_denoise if xe_denoise is None else xe_denoise)
        if _xe_policy_role(expected_quality, expected_denoise) != str(role).lower():
            raise ValueError(
                f"Xe provenance role {role!r} does not match requested policy "
                f"{expected_quality}/{expected_denoise}"
            )
        _require_request_fields(request, (
            "xegtao_quality", "xegtao_denoise", "xegtao_resolution",
            "xegtao_depth_storage",
        ))
        if (request["implementation"] != "xegtao" or
                str(request["xegtao_quality"]).lower() != expected_quality or
                _normalize_xe_denoise(request["xegtao_denoise"]) != expected_denoise or
                request["xegtao_resolution"] != "full" or
                request["xegtao_depth_storage"] != "r32float" or
                request["unshaded_ao"] is not False):
            raise ValueError(
                f"Xe run must be full-resolution XeGTAO {expected_quality}/"
                f"{expected_denoise} with r32float depth"
            )
        storage = result.get("xegtao_depth_storage")
        if (not isinstance(storage, dict) or storage.get("requested") != "r32float" or
                storage.get("actual") != "r32float"):
            raise ValueError("Xe run is missing actual r32float depth provenance")
        logged_policy = validate_xe_policy_log(
            Path(run) / "game.log", quality=expected_quality,
            denoise=expected_denoise)
    else:
        raise ValueError(f"unknown run provenance role: {role}")
    actual_quality = {
        "implementation": request["implementation"],
        "unshaded_ao": request["unshaded_ao"],
        "ao": request["ao"],
    }
    if expected_quality is not None:
        storage = result["xegtao_depth_storage"]
        actual_quality.update(
            quality=expected_quality,
            denoise=expected_denoise,
            resolution=request["xegtao_resolution"],
            depth_storage=request["xegtao_depth_storage"],
            depth_storage_actual=storage["actual"],
            depth_storage_records=storage.get("records"),
            logged_policy=logged_policy,
        )
    return {
        "run_path": str(Path(run).resolve()),
        "result_path": str(path.resolve()),
        "executable_sha256": sha,
        "window_size": [width, height],
        "phase": {
            **{field: request[field] for field in REQUIRED_PHASE_FIELDS},
            "leaf_freeze": leaf_freeze,
            "geometry_at_second": geometry_at_second,
        },
        "freeze": {
            "enabled": leaf_freeze,
            "geometry_at": _integer(request["geometry_at"], "geometry_at"),
            "geometry_at_second": geometry_at_second,
        },
        "actual_quality": actual_quality,
        "logged_policy": logged_policy,
        "request": request,
    }


def validate_provenance_pair(neutral: dict, xe_ultra3: dict) -> None:
    """Require same binary, dimensions, and capture phase for the pair."""

    if neutral["executable_sha256"] != xe_ultra3["executable_sha256"]:
        raise ValueError("neutral and Xe runs use different executable sha256 values")
    if neutral["window_size"] != xe_ultra3["window_size"]:
        raise ValueError("neutral and Xe runs have different requested dimensions")
    if neutral["phase"] != xe_ultra3["phase"]:
        raise ValueError("neutral and Xe runs have different capture phase/request settings")


def _finite_float(value, name: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be numeric") from exc
    if not math.isfinite(result):
        raise ValueError(f"{name} must be finite")
    return result


def _integer(value, name: str) -> int:
    number = _finite_float(value, name)
    if number != math.trunc(number):
        raise ValueError(f"{name} must be integral")
    return int(number)


def _pick_field(data: dict, *names: str):
    """Return the first present alias without treating falsey values as absent."""

    for name in names:
        if name in data:
            return data[name]
    raise KeyError(names[0])


def _normalize_freeze_asset(value) -> str:
    """Normalize decimal/hex asset IDs to the canonical eight-digit spelling."""

    if isinstance(value, bool):
        raise ValueError("frozen leaf asset must be an integer or hexadecimal string")
    if isinstance(value, str):
        text = value.strip()
        if not text:
            raise ValueError("frozen leaf asset is empty")
        try:
            number = int(text, 0)
        except ValueError:
            try:
                number = int(text, 16)
            except ValueError as exc:
                raise ValueError(f"invalid frozen leaf asset: {value!r}") from exc
    else:
        number = _integer(value, "frozen leaf asset")
    if number != FREEZE_ASSET:
        raise ValueError(
            f"unexpected frozen leaf asset: got 0x{number:08X}, "
            f"expected {FREEZE_ASSET_TEXT}"
        )
    return FREEZE_ASSET_TEXT


def parse_leaf_freeze_log(path: Path) -> LeafFreezeRecord | None:
    """Parse the exact resolved selection from a ``[LEAF-FREEZE]`` marker.

    The helper accepts the canonical names (``asset``, ``root``,
    ``resolvedModel``, ``time``, ``speed``) plus the two spelling variants
    used by older harness helpers.  A non-frozen animated run has no marker
    and returns ``None``; a malformed or conflicting marker is an error.
    """

    records: list[LeafFreezeRecord] = []
    marker = "[LEAF-FREEZE]"
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise ValueError(f"cannot read freeze log: {path}") from exc
    for line_number, line in enumerate(lines, 1):
        if marker not in line:
            continue
        payload = line.split(marker, 1)[1].strip()
        try:
            data = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise ValueError(f"invalid [LEAF-FREEZE] JSON at line {line_number}") from exc
        if not isinstance(data, dict):
            raise ValueError(f"[LEAF-FREEZE] line {line_number} is not an object")
        try:
            asset = _normalize_freeze_asset(
                _pick_field(data, "asset", "assetId", "assetID"))
            root = _pick_field(data, "root")
            model = _integer(
                _pick_field(data, "resolvedModel", "resolved_model", "model"),
                "frozen resolved model")
            time = _finite_float(
                _pick_field(data, "time", "motionTime"), "frozen motion time")
            speed = _finite_float(
                _pick_field(data, "speed", "motionSpeed"), "frozen motion speed")
        except KeyError as exc:
            raise ValueError(
                f"[LEAF-FREEZE] line {line_number} missing {exc.args[0]}"
            ) from exc
        if root != FREEZE_ROOT:
            raise ValueError(
                f"unexpected frozen leaf root: got {root!r}, expected {FREEZE_ROOT!r}"
            )
        if not math.isclose(time, FREEZE_TIME, rel_tol=0.0, abs_tol=1e-6):
            raise ValueError(f"unexpected frozen motion time: {time:g}")
        if not math.isclose(speed, FREEZE_SPEED, rel_tol=0.0, abs_tol=1e-6):
            raise ValueError(f"unexpected frozen motion speed: {speed:g}")
        records.append(LeafFreezeRecord(asset, root, model, time, speed))
    if not records:
        return None
    if len(set(records)) != 1:
        raise ValueError(f"freeze log contains conflicting markers: {path}")
    return records[0]


def _float_array(value, name: str) -> np.ndarray:
    try:
        array = np.asarray(value, dtype=np.float64)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a numeric array") from exc
    if array.size == 0 or not np.isfinite(array).all():
        raise ValueError(f"{name} must be a non-empty finite array")
    return array


def _load_geometry_rows(run: Path) -> dict[int, list[dict]]:
    path = Path(run) / "geometry.jsonl"
    if not path.is_file():
        raise ValueError(f"frozen run is missing geometry.jsonl: {path}")
    by_tick: dict[int, list[dict]] = {}
    for line_number, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"invalid geometry JSON at line {line_number}: {path}") from exc
        if not isinstance(row, dict):
            raise ValueError(f"geometry line {line_number} is not an object: {path}")
        tick = _integer(row.get("tick"), "geometry tick")
        by_tick.setdefault(tick, []).append(row)
    if not by_tick:
        raise ValueError(f"geometry.jsonl has no rows: {path}")
    return by_tick


def _geometry_target_rows(by_tick: dict[int, list[dict]], tick: int,
                          freeze: LeafFreezeRecord, run: Path) -> dict[str, dict]:
    rows = by_tick.get(tick)
    if not rows:
        raise ValueError(f"frozen geometry tick {tick} is absent from {run / 'geometry.jsonl'}")
    selected: dict[str, dict] = {}
    for row in rows:
        try:
            model = _integer(row.get("model"), "geometry model")
        except ValueError:
            continue
        if model != freeze.resolved_model:
            continue
        name = row.get("name")
        if name in FREEZE_TARGET_NAMES:
            if name in selected:
                raise ValueError(f"duplicate frozen geometry target {name!r} at tick {tick}")
            selected[name] = row
    missing = [name for name in FREEZE_TARGET_NAMES if name not in selected]
    if missing:
        raise ValueError(
            f"frozen geometry tick {tick} on model {freeze.resolved_model} "
            f"is missing {', '.join(missing)} in {run / 'geometry.jsonl'}"
        )
    return selected


def validate_frozen_geometry_pair(neutral_run: Path, xe_ultra3_run: Path,
                                  *, geometry_at: int,
                                  geometry_at_second: int,
                                  neutral_freeze: LeafFreezeRecord,
                                  xe_freeze: LeafFreezeRecord,
                                  xe_role: str = "xe_ultra3") -> dict:
    """Prove the requested plant rows stayed frozen across both captures.

    This proof is intentionally separate from the RGB motion metrics: it only
    checks the five resolved geometry targets and does not claim whole-plant
    image quality.
    """

    xe_quality, xe_denoise = _xe_policy_from_role(xe_role)
    xe_role = _xe_policy_role(xe_quality, xe_denoise)
    if neutral_freeze != xe_freeze:
        raise ValueError("neutral and Xe runs have different [LEAF-FREEZE] selections")
    ticks = (_integer(geometry_at, "geometry_at"),
             _integer(geometry_at_second, "geometry_at_second"))
    if ticks[0] == ticks[1]:
        raise ValueError("frozen geometry_at and geometry_at_second must differ")
    neutral_rows = _load_geometry_rows(Path(neutral_run))
    xe_rows = _load_geometry_rows(Path(xe_ultra3_run))
    selected = {
        "neutral": {
            tick: _geometry_target_rows(neutral_rows, tick, neutral_freeze,
                                        Path(neutral_run))
            for tick in ticks
        },
        xe_role: {
            tick: _geometry_target_rows(xe_rows, tick, xe_freeze,
                                        Path(xe_ultra3_run))
            for tick in ticks
        },
    }
    base = selected["neutral"][ticks[0]]
    max_world_delta = 0.0
    for role in ("neutral", xe_role):
        for tick in ticks:
            for name in FREEZE_TARGET_NAMES:
                row = selected[role][tick][name]
                time = _finite_float(row.get("motionTime"), "geometry motionTime")
                speed = _finite_float(row.get("motionSpeed"), "geometry motionSpeed")
                if not math.isclose(time, FREEZE_TIME, rel_tol=0.0, abs_tol=1e-6):
                    raise ValueError(
                        f"frozen target {name} at tick {tick} has motionTime={time:g}, "
                        f"expected {FREEZE_TIME:g}"
                    )
                if not math.isclose(speed, FREEZE_SPEED, rel_tol=0.0, abs_tol=1e-6):
                    raise ValueError(
                        f"frozen target {name} at tick {tick} has motionSpeed={speed:g}, "
                        f"expected {FREEZE_SPEED:g}"
                    )
                reference = base[name]
                local = _float_array(row.get("localVertices"),
                                     f"{role} {name} localVertices")
                reference_local = _float_array(
                    reference.get("localVertices"), f"reference {name} localVertices")
                if local.shape != reference_local.shape or not np.allclose(
                        local, reference_local, rtol=0.0, atol=FREEZE_LOCAL_TOLERANCE):
                    raise ValueError(f"frozen target {name} localVertices changed at tick {tick}")
                if row.get("faces") != reference.get("faces"):
                    raise ValueError(f"frozen target {name} faces changed at tick {tick}")
                transform = row.get("localTransform")
                reference_transform = reference.get("localTransform")
                if not isinstance(transform, dict) or not isinstance(reference_transform, dict):
                    raise ValueError(f"frozen target {name} is missing localTransform")
                for component in ("pos", "rot", "scale"):
                    current = _float_array(transform.get(component),
                                           f"{role} {name} localTransform.{component}")
                    expected = _float_array(
                        reference_transform.get(component),
                        f"reference {name} localTransform.{component}")
                    if current.shape != expected.shape or not np.allclose(
                            current, expected, rtol=0.0, atol=FREEZE_LOCAL_TOLERANCE):
                        raise ValueError(
                            f"frozen target {name} localTransform.{component} changed "
                            f"at tick {tick}"
                        )
                world = _float_array(row.get("vertices"),
                                     f"{role} {name} world vertices")
                reference_world = _float_array(
                    reference.get("vertices"), f"reference {name} world vertices")
                if world.shape != reference_world.shape:
                    raise ValueError(f"frozen target {name} world vertex shape changed at tick {tick}")
                delta = float(np.max(np.abs(world - reference_world)))
                max_world_delta = max(max_world_delta, delta)
    if max_world_delta > FREEZE_WORLD_TOLERANCE:
        raise ValueError(
            f"frozen geometry world max abs delta {max_world_delta:g} exceeds "
            f"{FREEZE_WORLD_TOLERANCE:g}"
        )
    return {
        "enabled": True,
        "asset": neutral_freeze.asset,
        "root": neutral_freeze.root,
        "resolved_model": neutral_freeze.resolved_model,
        "motion_time": neutral_freeze.time,
        "motion_speed": neutral_freeze.speed,
        "geometry_ticks": list(ticks),
        "targets": list(FREEZE_TARGET_NAMES),
        "local_vertices_unchanged": True,
        "faces_unchanged": True,
        "local_transform_unchanged": True,
        "world_max_abs_delta": max_world_delta,
        "world_delta_tolerance": FREEZE_WORLD_TOLERANCE,
        "records_per_run": {role: {str(tick): len(selected[role][tick])
                                     for tick in ticks}
                             for role in ("neutral", xe_role)},
    }


def _visibility_triangles_for_row(row: dict, *, source: str = "geometry") -> tuple[tuple[np.ndarray, np.ndarray, np.ndarray], ...]:
    """Triangulate one captured target row for conservative visibility tests."""

    name = row.get("name", "<unnamed>")
    try:
        vertices = np.asarray(row["vertices"], dtype=np.float64)
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"{source} target {name} has malformed vertices") from exc
    if (vertices.ndim != 2 or vertices.shape[1] != 3 or vertices.shape[0] < 3 or
            not np.isfinite(vertices).all()):
        raise ValueError(f"{source} target {name} vertices must be a finite Nx3 array")
    faces = row.get("faces")
    if not isinstance(faces, (list, tuple)) or not faces:
        raise ValueError(f"{source} target {name} has no faces")
    triangles = []
    for face_index, face in enumerate(faces):
        if not isinstance(face, (list, tuple)) or len(face) < 4:
            raise ValueError(f"{source} target {name} face {face_index} is malformed")
        primitive = _integer(face[0], f"{source} target {name} face type")
        if primitive not in FREEZE_FACE_TYPES:
            raise ValueError(
                f"{source} target {name} face {face_index} has unsupported type {primitive}; "
                f"expected one of {FREEZE_FACE_TYPES}"
            )
        indices = []
        for index in face[1:]:
            indices.append(_integer(
                index, f"{source} target {name} face {face_index} index"))
        expected = 3 if primitive == 2 else 4
        if len(indices) != expected or len(set(indices)) != expected:
            raise ValueError(
                f"{source} target {name} face {face_index} type {primitive} "
                f"requires {expected} distinct indices"
            )
        if any(index < 0 or index >= vertices.shape[0] for index in indices):
            raise ValueError(f"{source} target {name} face {face_index} index is out of range")
        if primitive == 2:
            triangles.append(tuple(vertices[index] for index in indices))
        else:
            # HSF stores quad corners in crossed strip order [TL, BL, TR, BR],
            # while FaceDraw emits GX_QUADS as [0, 2, 3, 1].  Split along
            # that emitted diagonal so visibility follows the native surface.
            triangles.append((vertices[indices[0]], vertices[indices[2]], vertices[indices[3]]))
            triangles.append((vertices[indices[0]], vertices[indices[3]], vertices[indices[1]]))
    return tuple(triangles)


def load_frozen_visibility_triangles(run: Path, geometry_tick: int,
                                     freeze: LeafFreezeRecord) -> tuple[tuple[np.ndarray, np.ndarray, np.ndarray], ...]:
    """Load the five resolved target meshes at the validated geometry tick."""

    rows = _load_geometry_rows(Path(run))
    selected = _geometry_target_rows(rows, _integer(geometry_tick, "geometry tick"),
                                     freeze, Path(run))
    triangles = []
    for name in FREEZE_TARGET_NAMES:
        triangles.extend(_visibility_triangles_for_row(
            selected[name], source=f"{Path(run) / 'geometry.jsonl'}:{geometry_tick}"))
    if not triangles:
        raise ValueError(f"no target triangles found in frozen geometry: {run}")
    return tuple(triangles)


def _segment_target_hits(origin, endpoints, triangles: tuple[tuple[np.ndarray, np.ndarray, np.ndarray], ...]) -> np.ndarray:
    """Return two-sided ray-segment hits before the receiver endpoint."""

    origin = np.asarray(origin, dtype=np.float64)
    endpoints = np.asarray(endpoints, dtype=np.float64)
    if origin.shape != (3,) or not np.isfinite(origin).all():
        raise ValueError("visibility ray origin must be a finite 3-vector")
    if endpoints.ndim < 2 or endpoints.shape[-1] != 3 or not np.isfinite(endpoints).all():
        raise ValueError("visibility ray endpoints must be finite arrays ending in 3")
    flat_direction = (endpoints - origin).reshape(-1, 3)
    hits = np.zeros(flat_direction.shape[0], dtype=bool)
    for first, second, third in triangles:
        first = np.asarray(first, dtype=np.float64)
        second = np.asarray(second, dtype=np.float64)
        third = np.asarray(third, dtype=np.float64)
        if (first.shape != (3,) or second.shape != (3,) or third.shape != (3,) or
                not np.isfinite(np.stack((first, second, third))).all()):
            raise ValueError("visibility triangle must contain three finite 3-vectors")
        edge1 = second - first
        edge2 = third - first
        pvec = np.cross(flat_direction, edge2)
        determinant = pvec @ edge1
        nonparallel = np.abs(determinant) > VISIBILITY_EPSILON
        inverse = np.zeros_like(determinant)
        inverse[nonparallel] = 1.0 / determinant[nonparallel]
        to_origin = origin - first
        bary_u = (pvec @ to_origin) * inverse
        qvec = np.cross(to_origin, edge1)
        bary_v = (flat_direction @ qvec) * inverse
        distance = (qvec @ edge2) * inverse
        hits |= (
            nonparallel &
            (bary_u >= -VISIBILITY_EPSILON) &
            (bary_v >= -VISIBILITY_EPSILON) &
            (bary_u + bary_v <= 1.0 + VISIBILITY_EPSILON) &
            (distance > VISIBILITY_EPSILON) &
            (distance < 1.0 - VISIBILITY_EPSILON)
        )
    return hits.reshape(endpoints.shape[:-1])


def target_covered_by_bilinear_footprint(camera: Camera, u, v, valid,
                                         plane_y: float, triangles) -> np.ndarray:
    """Conservatively mark samples covered at any bilinear footprint corner."""

    u = np.asarray(u, dtype=np.float64)
    v = np.asarray(v, dtype=np.float64)
    valid = np.asarray(valid, dtype=bool)
    if u.shape != v.shape or u.shape != valid.shape:
        raise ValueError("visibility pixel coordinates and valid mask must have equal shapes")
    finite = np.isfinite(u) & np.isfinite(v)
    floor_u = np.where(finite, np.floor(u), 0).astype(np.int64)
    floor_v = np.where(finite, np.floor(v), 0).astype(np.int64)
    covered = np.zeros(u.shape, dtype=bool)
    for offset_x, offset_y in ((0, 0), (1, 0), (0, 1), (1, 1)):
        corner_valid = valid & finite
        if not corner_valid.any():
            continue
        corner_u = floor_u + offset_x
        corner_v = floor_v + offset_y
        points = camera.world_on_plane(
            corner_u[corner_valid], corner_v[corner_valid], plane_y)
        origin = -camera.inverse @ camera.matrix[:, 3]
        corner_hits = _segment_target_hits(origin, points, tuple(triangles))
        covered[corner_valid] |= corner_hits
    return covered


def _vector3(value, name: str) -> tuple[float, float, float]:
    if not isinstance(value, (list, tuple)) or len(value) != 3:
        raise ValueError(f"{name} must contain exactly three values")
    return tuple(_finite_float(part, f"{name}[{index}]")
                 for index, part in enumerate(value))


def _leaf_record(data: dict, line_number: int) -> LeafCameraRecord:
    required = ("tick", "relativeTick", "mode", "fov", "aspect", "eye", "center")
    missing = [key for key in required if key not in data]
    if missing:
        raise ValueError(f"[LEAF-CAMERA] line {line_number} missing {', '.join(missing)}")
    mode = data["mode"]
    if not isinstance(mode, str) or not mode:
        raise ValueError(f"[LEAF-CAMERA] line {line_number} has invalid mode")
    fov = _finite_float(data["fov"], "fov")
    aspect = _finite_float(data["aspect"], "aspect")
    if not 0.0 < fov < 179.0 or aspect <= 0.0:
        raise ValueError(f"[LEAF-CAMERA] line {line_number} has invalid fov/aspect")
    return LeafCameraRecord(
        tick=_integer(data["tick"], "tick"),
        relative_tick=_integer(data["relativeTick"], "relativeTick"),
        mode=mode,
        fov=fov,
        aspect=aspect,
        eye=_vector3(data["eye"], "eye"),
        center=_vector3(data["center"], "center"),
    )


def parse_leaf_camera_log(path: Path) -> tuple[LeafCameraRecord, ...]:
    """Parse self-contained camera records from a native ``game.log``.

    Older harnesses printed one mode-only marker before the sampled records;
    that marker is harmless and is ignored.  Any sampled marker with a
    missing field is rejected rather than silently pairing the wrong phase.
    """

    records: list[LeafCameraRecord] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        marker = "[LEAF-CAMERA]"
        if marker not in line:
            continue
        payload = line.split(marker, 1)[1].strip()
        try:
            data = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise ValueError(f"invalid [LEAF-CAMERA] JSON at line {line_number}") from exc
        if not isinstance(data, dict):
            raise ValueError(f"[LEAF-CAMERA] line {line_number} is not an object")
        if "tick" not in data and set(data).issubset({"mode"}):
            continue
        records.append(_leaf_record(data, line_number))
    if not records:
        raise ValueError(f"no self-contained [LEAF-CAMERA] records in {path}")
    for previous, current in zip(records, records[1:]):
        if current.tick <= previous.tick:
            raise ValueError(f"camera ticks are not strictly increasing in {path}")
        if current.relative_tick <= previous.relative_tick:
            raise ValueError(f"camera relativeTick values are not strictly increasing in {path}")
    return tuple(records)


def load_camera_record(path: Path, tick: int | None = None) -> dict:
    """Load the installed camera record required for aspect and pose checks."""

    rows = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"invalid camera JSON at line {line_number}") from exc
        if not isinstance(row, dict):
            raise ValueError(f"camera record line {line_number} is not an object")
        rows.append(row)
    if not rows:
        raise ValueError(f"no installed camera records in {path}")
    if tick is not None:
        rows = [row for row in rows if _integer(row.get("tick"), "camera tick") == tick]
        if not rows:
            raise ValueError(f"camera record tick {tick} is absent from {path}")
    elif len(rows) != 1:
        raise ValueError("--camera-record must contain one row or use --camera-record-tick")
    record = dict(rows[0])
    if "viewMatrix" not in record or "viewport" not in record:
        raise ValueError("installed camera record must contain viewMatrix and viewport")
    matrix = np.asarray(record["viewMatrix"], dtype=np.float64)
    viewport = np.asarray(record["viewport"], dtype=np.float64)
    fov = _finite_float(record.get("fov"), "camera fov")
    aspect = _finite_float(record.get("aspect"), "camera aspect")
    if matrix.size != 12 or not np.isfinite(matrix).all():
        raise ValueError("installed camera viewMatrix must contain 12 finite values")
    if viewport.size != 4 or not np.isfinite(viewport).all():
        raise ValueError("installed camera viewport must contain four finite values")
    if viewport[0] != 0 or viewport[1] != 0 or viewport[2] <= 0 or viewport[3] <= 0:
        raise ValueError("only full installed camera viewports are supported")
    if not 0.0 < fov < 179.0 or aspect <= 0.0:
        raise ValueError("installed camera fov/aspect is invalid")
    record["fov"] = fov
    record["aspect"] = aspect
    record["viewMatrix"] = matrix.reshape(3, 4).reshape(-1).tolist()
    record["viewport"] = viewport.tolist()
    return record


def normalize_log_aspect(log_aspect: float, installed_aspect: float,
                         raw_aspect: float = RAW_BOARD_ASPECT,
                         tolerance: float = ASPECT_TOLERANCE) -> str:
    """Accept either raw-board or already-widened log aspect.

    The returned label is useful in diagnostics; all camera construction uses
    ``installed_aspect``.  A third value is evidence of a mismatched harness
    and must not be silently folded into registration.
    """

    log_aspect = _finite_float(log_aspect, "log aspect")
    installed_aspect = _finite_float(installed_aspect, "installed aspect")
    raw_aspect = _finite_float(raw_aspect, "raw aspect")
    if math.isclose(log_aspect, installed_aspect, rel_tol=0.0, abs_tol=tolerance):
        return "installed"
    if math.isclose(log_aspect, raw_aspect, rel_tol=0.0, abs_tol=tolerance):
        return "raw-board"
    raise ValueError(
        f"camera log aspect {log_aspect:g} is neither raw board {raw_aspect:g} "
        f"nor installed {installed_aspect:g}"
    )


def look_at_view_matrix(eye, center, up=(0.0, 1.0, 0.0)) -> np.ndarray:
    """Build the row-major Hu3D view matrix for a Y-up look-at camera."""

    eye = np.asarray(eye, dtype=np.float64)
    center = np.asarray(center, dtype=np.float64)
    up = np.asarray(up, dtype=np.float64)
    if eye.shape != (3,) or center.shape != (3,) or up.shape != (3,):
        raise ValueError("look-at vectors must have shape (3,)")
    if not np.isfinite(eye).all() or not np.isfinite(center).all() or not np.isfinite(up).all():
        raise ValueError("look-at vectors must be finite")
    forward = center - eye
    forward_norm = np.linalg.norm(forward)
    if forward_norm <= 1e-12:
        raise ValueError("look-at eye and center must differ")
    forward /= forward_norm
    right = np.cross(forward, up)
    right_norm = np.linalg.norm(right)
    if right_norm <= 1e-12:
        raise ValueError("look-at up must not be parallel to view direction")
    right /= right_norm
    true_up = np.cross(right, forward)
    matrix = np.empty((3, 4), dtype=np.float64)
    matrix[:, :3] = np.stack((right, true_up, -forward))
    matrix[:, 3] = -matrix[:, :3] @ eye
    return matrix


def _camera_for_log(record: LeafCameraRecord, installed_record: dict,
                    output_size: tuple[int, int], raw_aspect: float,
                    aspect_tolerance: float) -> Camera:
    normalize_log_aspect(record.aspect, installed_record["aspect"], raw_aspect,
                         aspect_tolerance)
    matrix = look_at_view_matrix(record.eye, record.center)
    camera_record = dict(installed_record, fov=record.fov,
                         aspect=installed_record["aspect"],
                         viewMatrix=matrix.reshape(-1).tolist())
    return Camera(camera_record, output_size)


def validate_camera_record_against_logs(installed_record: dict,
                                        records: tuple[LeafCameraRecord, ...] | list[LeafCameraRecord],
                                        *, raw_aspect: float = RAW_BOARD_ASPECT,
                                        aspect_tolerance: float = ASPECT_TOLERANCE,
                                        fov_tolerance: float = FOV_TOLERANCE,
                                        matrix_tolerance: float = MATRIX_TOLERANCE) -> None:
    """Verify the geometry probe's pose/FOV against the same-tick log row."""

    tick = _integer(installed_record.get("tick"), "camera tick")
    matches = [row for row in records if row.tick == tick]
    if len(matches) != 1:
        raise ValueError(f"camera log must contain exactly one row at installed tick {tick}")
    row = matches[0]
    normalize_log_aspect(row.aspect, installed_record["aspect"], raw_aspect,
                         aspect_tolerance)
    if not math.isclose(row.fov, installed_record["fov"], rel_tol=0.0,
                        abs_tol=fov_tolerance):
        raise ValueError(
            f"camera FOV mismatch at tick {tick}: log={row.fov:g}, "
            f"installed={installed_record['fov']:g}"
        )
    expected = look_at_view_matrix(row.eye, row.center)
    actual = np.asarray(installed_record["viewMatrix"], dtype=np.float64).reshape(3, 4)
    if not np.allclose(expected, actual, rtol=0.0, atol=matrix_tolerance):
        raise ValueError(f"camera pose mismatch at installed tick {tick}")


def pair_camera_records(neutral: tuple[LeafCameraRecord, ...] | list[LeafCameraRecord],
                        xe_ultra3: tuple[LeafCameraRecord, ...] | list[LeafCameraRecord],
                        *, installed_aspect: float,
                        raw_aspect: float = RAW_BOARD_ASPECT,
                        aspect_tolerance: float = ASPECT_TOLERANCE,
                        fov_tolerance: float = FOV_TOLERANCE) -> tuple[tuple[LeafCameraRecord, LeafCameraRecord], ...]:
    """Pair logs only when tick, phase, mode, FOV, and cardinality agree."""

    if len(neutral) != len(xe_ultra3) or not neutral:
        raise ValueError("camera logs have different or empty sample counts")
    pairs = []
    for index, (left, right) in enumerate(zip(neutral, xe_ultra3)):
        left_key = (left.tick, left.relative_tick, left.mode)
        right_key = (right.tick, right.relative_tick, right.mode)
        if left_key != right_key:
            raise ValueError(f"camera phase/mode mismatch at sample {index}: {left_key} != {right_key}")
        normalize_log_aspect(left.aspect, installed_aspect, raw_aspect, aspect_tolerance)
        normalize_log_aspect(right.aspect, installed_aspect, raw_aspect, aspect_tolerance)
        if not math.isclose(left.fov, right.fov, rel_tol=0.0, abs_tol=fov_tolerance):
            raise ValueError(f"camera FOV mismatch at tick {left.tick}")
        if (not np.allclose(left.eye, right.eye, rtol=0.0, atol=POSE_TOLERANCE) or
                not np.allclose(left.center, right.center, rtol=0.0, atol=POSE_TOLERANCE)):
            raise ValueError(f"camera pose mismatch at tick {left.tick}")
        pairs.append((left, right))
    return tuple(pairs)


def _load_frames(run: Path) -> dict[int, np.ndarray]:
    from compare_ao_captures import frame

    frame_paths = sorted((run / "frames").glob("*.mfd"))
    if not frame_paths:
        raise ValueError(f"no .mfd frames in {run}")
    output = {}
    for path in frame_paths:
        tick, image = frame(path)
        if tick in output:
            raise ValueError(f"duplicate frame tick {tick} in {run}")
        if image.ndim != 3 or image.shape[2] != 3:
            raise ValueError(f"frame {path} is not an RGB image")
        output[int(tick)] = image
    return output


def _validate_roi(roi, size: tuple[int, int], stride: int) -> tuple[int, int, int, int]:
    if len(roi) != 4:
        raise ValueError("ROI must contain x0 y0 x1 y1")
    x0, y0, x1, y1 = (int(value) for value in roi)
    width, height = size
    if stride <= 0 or not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        raise ValueError(f"ROI {tuple(roi)} is outside image size {(width, height)}")
    return x0, y0, x1, y1


def _repeated_pose_pairs(
        pairs: tuple[tuple[LeafCameraRecord, LeafCameraRecord], ...],
        ticks: list[int] | tuple[int, ...],
        *, period_ticks: int = REPEATED_POSE_PERIOD_TICKS) -> tuple[tuple[int, int], ...]:
    """Find sampled frame pairs that revisit the same logged camera pose."""

    if period_ticks <= 0:
        raise ValueError("repeated-pose period must be positive")
    by_tick = {left.tick: index for index, (left, _) in enumerate(pairs)}
    repeated = []
    for tick in ticks:
        other_index = by_tick.get(tick + period_ticks)
        if other_index is None:
            continue
        index = by_tick[tick]
        left, left_xe = pairs[index]
        right, right_xe = pairs[other_index]
        if (left.mode != right.mode or
                not math.isclose(left.fov, right.fov, rel_tol=0.0,
                                 abs_tol=FOV_TOLERANCE) or
                not np.allclose(left.eye, right.eye, rtol=0.0,
                                atol=POSE_TOLERANCE) or
                not np.allclose(left.center, right.center, rtol=0.0,
                                atol=POSE_TOLERANCE)):
            continue
        # pair_camera_records already compares the two run poses at each
        # tick; retaining this assertion documents that the metric is based
        # on a pose shared by neutral and Xe, not one run alone.
        if (left_xe.mode != left.mode or right_xe.mode != right.mode or
                not math.isclose(left_xe.fov, left.fov, rel_tol=0.0,
                                 abs_tol=FOV_TOLERANCE) or
                not math.isclose(right_xe.fov, right.fov, rel_tol=0.0,
                                 abs_tol=FOV_TOLERANCE)):
            continue
        repeated.append((index, other_index))
    return tuple(repeated)


def _repeated_pose_stats(
        repeated_pairs: tuple[tuple[int, int], ...],
        signed_darkening: list[np.ndarray],
        common: np.ndarray,
        ticks: list[int] | tuple[int, ...],
        *, period_ticks: int = REPEATED_POSE_PERIOD_TICKS) -> dict:
    """Summarize absolute Xe signed-darkening differences at repeated poses."""

    differences = []
    pair_reports = []
    for left_index, right_index in repeated_pairs:
        left = signed_darkening[left_index]
        right = signed_darkening[right_index]
        mask = common & np.isfinite(left) & np.isfinite(right)
        count = int(mask.sum())
        pair_reports.append({
            "first_tick": int(ticks[left_index]),
            "second_tick": int(ticks[right_index]),
            "samples": count,
        })
        if count:
            differences.append(np.abs(left[mask] - right[mask]))
    if differences:
        values = np.concatenate(differences)
        mean = float(np.mean(values))
        p95 = float(np.percentile(values, 95))
        maximum = float(np.max(values))
        sample_count = int(values.size)
    else:
        mean = p95 = maximum = None
        sample_count = 0
    return {
        "period_ticks": int(period_ticks),
        "pair_count": len(pair_reports),
        "sample_count": sample_count,
        "mean_abs_difference": mean,
        "p95_abs_difference": p95,
        "max_abs_difference": maximum,
        "units": "absolute signed neutral-minus-Xe luma difference, 0..255 display units",
        "pairs": pair_reports,
        "limitation": "Diagnostic repeated-pose reproducibility only; no acceptance threshold",
    }


def analyze(neutral_run: Path, xe_ultra3_run: Path, camera_record_path: Path,
            *, roi=DEFAULT_ROI, plane_y: float = PLANE_Y, stride: int = 2,
            min_samples: int = 100, camera_record_tick: int | None = None,
            raw_aspect: float = RAW_BOARD_ASPECT,
            aspect_tolerance: float = ASPECT_TOLERANCE,
            fov_tolerance: float = FOV_TOLERANCE,
            matrix_tolerance: float = MATRIX_TOLERANCE,
            xe_quality: str = "ultra", xe_denoise: int = 3) -> dict:
    """Return paired ground-ROI RGB motion metrics for two captured runs."""

    neutral_run = Path(neutral_run)
    xe_ultra3_run = Path(xe_ultra3_run)
    if not neutral_run.is_dir() or not xe_ultra3_run.is_dir():
        raise ValueError("both run arguments must be existing directories")
    if min_samples <= 0:
        raise ValueError("min_samples must be positive")
    xe_quality = _normalize_xe_quality(xe_quality)
    xe_denoise = _normalize_xe_denoise(xe_denoise)
    xe_role = _xe_policy_role(xe_quality, xe_denoise)
    neutral_provenance = load_run_provenance(neutral_run, "neutral")
    xe_provenance = load_run_provenance(
        xe_ultra3_run, xe_role, xe_quality=xe_quality, xe_denoise=xe_denoise)
    validate_provenance_pair(neutral_provenance, xe_provenance)
    freeze_enabled = neutral_provenance["freeze"]["enabled"]
    if freeze_enabled != xe_provenance["freeze"]["enabled"]:
        raise ValueError("neutral and Xe runs disagree on leaf_freeze")
    neutral_freeze = None
    xe_freeze = None
    geometry_proof = {
        "enabled": False,
        "reason": "leaf_freeze=false",
        "geometry_at": neutral_provenance["freeze"]["geometry_at"],
        "geometry_at_second": neutral_provenance["freeze"]["geometry_at_second"],
    }
    if freeze_enabled:
        neutral_freeze = parse_leaf_freeze_log(neutral_run / "game.log")
        xe_freeze = parse_leaf_freeze_log(xe_ultra3_run / "game.log")
        if neutral_freeze is None or xe_freeze is None:
            raise ValueError("frozen runs must contain one [LEAF-FREEZE] marker each")
        if neutral_provenance["freeze"]["geometry_at_second"] is None:
            raise ValueError("frozen runs must provide geometry_at_second")
        geometry_proof = validate_frozen_geometry_pair(
            neutral_run, xe_ultra3_run,
            geometry_at=neutral_provenance["freeze"]["geometry_at"],
            geometry_at_second=neutral_provenance["freeze"]["geometry_at_second"],
            neutral_freeze=neutral_freeze,
            xe_freeze=xe_freeze,
            xe_role=xe_role,
        )
    installed = load_camera_record(camera_record_path, camera_record_tick)
    neutral_records = parse_leaf_camera_log(neutral_run / "game.log")
    xe_records = parse_leaf_camera_log(xe_ultra3_run / "game.log")
    validate_camera_record_against_logs(
        installed, neutral_records, raw_aspect=raw_aspect,
        aspect_tolerance=aspect_tolerance, fov_tolerance=fov_tolerance,
        matrix_tolerance=matrix_tolerance)
    validate_camera_record_against_logs(
        installed, xe_records, raw_aspect=raw_aspect,
        aspect_tolerance=aspect_tolerance, fov_tolerance=fov_tolerance,
        matrix_tolerance=matrix_tolerance)
    pairs = pair_camera_records(
        neutral_records, xe_records, installed_aspect=installed["aspect"],
        raw_aspect=raw_aspect, aspect_tolerance=aspect_tolerance,
        fov_tolerance=fov_tolerance)
    neutral_frames = _load_frames(neutral_run)
    xe_frames = _load_frames(xe_ultra3_run)
    if set(neutral_frames) != set(xe_frames):
        raise ValueError("neutral and XeGTAO runs have different frame ticks")
    neutral_by_tick = {row.tick: row for row, _ in pairs}
    xe_by_tick = {row.tick: row for _, row in pairs}
    ticks = sorted(neutral_frames)
    if any(tick not in neutral_by_tick for tick in ticks):
        raise ValueError("every captured frame tick must have a camera log record")
    if len(ticks) < 2:
        raise ValueError("at least two paired frames are required")
    first_image = neutral_frames[ticks[0]]
    size = (int(first_image.shape[1]), int(first_image.shape[0]))
    if list(size) != neutral_provenance["window_size"]:
        raise ValueError(
            f"neutral frame dimensions {size} differ from result request "
            f"{tuple(neutral_provenance['window_size'])}"
        )
    for tick in ticks:
        if neutral_frames[tick].shape != first_image.shape or xe_frames[tick].shape != first_image.shape:
            raise ValueError("paired runs have inconsistent frame dimensions")
    roi = _validate_roi(roi, size, stride)
    x0, y0, x1, y1 = roi
    yy, xx = np.mgrid[y0:y1:stride, x0:x1:stride]
    first_neutral = _camera_for_log(
        neutral_by_tick[ticks[0]], installed, size, raw_aspect, aspect_tolerance)
    world = first_neutral.world_on_plane(xx, yy, plane_y)
    roundtrip_u, roundtrip_v, roundtrip_depth = first_neutral.project(world)
    roundtrip = float(max(np.max(np.abs(roundtrip_u - xx)),
                          np.max(np.abs(roundtrip_v - yy))))
    if not np.isfinite(roundtrip_depth).all() or roundtrip > ROUNDTRIP_TOLERANCE:
        raise ValueError(f"camera projection roundtrip failed: {roundtrip:g} pixels")
    if not np.allclose(world[..., 1], plane_y, rtol=0.0, atol=1e-9):
        raise ValueError("world registration did not land on the receiver plane")

    visibility_triangles = ()
    if freeze_enabled:
        geometry_tick = neutral_provenance["freeze"]["geometry_at"]
        # The frozen geometry proof has already compared both runs and both
        # geometry ticks.  Include both same-selection meshes here so a tiny
        # capture-side world delta cannot turn a covered sample into a false
        # retained sample.
        visibility_triangles = (
            load_frozen_visibility_triangles(neutral_run, geometry_tick, neutral_freeze) +
            load_frozen_visibility_triangles(xe_ultra3_run, geometry_tick, xe_freeze)
        )

    neutral_samples = []
    xe_samples = []
    validity = []
    target_covered_frames = []
    for tick in ticks:
        neutral_camera = _camera_for_log(
            neutral_by_tick[tick], installed, size, raw_aspect, aspect_tolerance)
        xe_camera = _camera_for_log(
            xe_by_tick[tick], installed, size, raw_aspect, aspect_tolerance)
        neutral_u, neutral_v, neutral_depth = neutral_camera.project(world)
        xe_u, xe_v, xe_depth = xe_camera.project(world)
        neutral_sample, neutral_valid = bilinear(
            neutral_frames[tick], neutral_u, neutral_v)
        xe_sample, xe_valid = bilinear(xe_frames[tick], xe_u, xe_v)
        valid = (neutral_valid & xe_valid &
                 np.isfinite(neutral_depth) & np.isfinite(xe_depth) &
                 (neutral_depth > 0) & (xe_depth > 0))
        neutral_samples.append(neutral_sample)
        xe_samples.append(xe_sample)
        validity.append(valid)
        if freeze_enabled:
            neutral_covered = target_covered_by_bilinear_footprint(
                neutral_camera, neutral_u, neutral_v, valid, plane_y,
                visibility_triangles)
            xe_covered = target_covered_by_bilinear_footprint(
                xe_camera, xe_u, xe_v, valid, plane_y, visibility_triangles)
            target_covered_frames.append(neutral_covered | xe_covered)
    raster_common = np.logical_and.reduce(validity)
    neutral_stack = np.stack(neutral_samples)
    xe_stack = np.stack(xe_samples)
    stable = np.max(np.ptp(neutral_stack, axis=0), axis=-1) <= 2.0
    neutral_stable = raster_common & stable
    if freeze_enabled:
        target_covered = np.logical_or.reduce(target_covered_frames)
    else:
        target_covered = np.zeros(neutral_stable.shape, dtype=bool)
    common = neutral_stable & ~target_covered
    retained = int(common.sum())
    if retained < min_samples:
        raise ValueError(f"insufficient neutral-stable receiver samples: {retained} < {min_samples}")

    luminance = np.array([0.2126, 0.7152, 0.0722])
    neutral_luma = [sample @ luminance for sample in neutral_samples]
    signed_darkening = [(neutral - xe) @ luminance
                        for neutral, xe in zip(neutral_samples, xe_samples)]
    frame_pairs = tuple((neutral_by_tick[tick], xe_by_tick[tick]) for tick in ticks)
    repeated_pairs = _repeated_pose_pairs(frame_pairs, ticks)
    neutral_mask_report = {
        "rule": "Each neutral RGB channel range <= 2 display codes",
        "raster_common": int(raster_common.sum()),
        "retained": retained,
        "limitation": "May exclude genuine moving geometry; not a whole-plant metric",
    }
    visibility_report = None
    if freeze_enabled:
        neutral_stable_count = int(neutral_stable.sum())
        target_covered_count = int((neutral_stable & target_covered).sum())
        neutral_mask_report.update(
            neutral_stable=neutral_stable_count,
            target_covered_excluded=target_covered_count,
            final_samples=retained,
        )
        visibility_report = {
            "rule": "Exclude a sample when any of its four bilinear pixel-center rays "
                    "hits a target mesh before the receiver plane in any frame",
            "target_meshes": list(FREEZE_TARGET_NAMES),
            "geometry_tick": neutral_provenance["freeze"]["geometry_at"],
            "triangle_count": len(visibility_triangles),
            "union_covered": int(target_covered.sum()),
            "excluded_from_neutral_stable": target_covered_count,
            "final_samples": retained,
            "limitation": "Visibility proof covers only these five target meshes; "
                           "it is not full-scene occlusion or whole-plant acceptance",
        }
    return {
        "scope": "paired visible-ground ROI only; not whole-plant acceptance",
        "roi": list(roi),
        "frames": ticks,
        "samples": retained,
        "receiver_y": float(plane_y),
        "roundtrip_max_pixels": roundtrip,
        "geometry_proof": geometry_proof,
        "camera": {
            "installed_record_tick": installed.get("tick"),
            "installed_aspect": installed["aspect"],
            "raw_board_aspect": raw_aspect,
            "viewport": installed["viewport"],
        },
        "provenance": {
            "neutral": {
                "run_path": neutral_provenance["run_path"],
                "result_path": neutral_provenance["result_path"],
                "executable_sha256": neutral_provenance["executable_sha256"],
                "actual_quality": neutral_provenance["actual_quality"],
            },
            xe_role: {
                "run_path": xe_provenance["run_path"],
                "result_path": xe_provenance["result_path"],
                "executable_sha256": xe_provenance["executable_sha256"],
                "actual_quality": xe_provenance["actual_quality"],
            },
        },
        "neutral_mask": neutral_mask_report,
        **({"visibility_mask": visibility_report} if visibility_report is not None else {}),
        "neutral_world_motion_rgb": motion_stats(neutral_luma, common),
        f"{xe_role}_signed_darkening": {
            **motion_stats(signed_darkening, common),
            "units": "signed neutral-minus-Xe luma, 0..255 display units",
        },
        "repeated_pose_signed_darkening": _repeated_pose_stats(
            repeated_pairs, signed_darkening, common, ticks),
        "modes": {
            "neutral": pairs[0][0].mode,
            xe_role: pairs[0][1].mode,
        },
    }


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neutral-run", type=Path, required=True,
                        help="run directory containing unshaded legacy frames/game.log")
    parser.add_argument("--xe-run", type=Path, required=True,
                        help="run directory containing XeGTAO frames/game.log")
    parser.add_argument("--xe-quality", default="ultra", choices=("high", "ultra"),
                        help="expected native XeGTAO quality (default: ultra)")
    parser.add_argument("--xe-denoise", type=int, default=3,
                        help="expected native XeGTAO spatial denoise passes (default: 3)")
    parser.add_argument("--camera-record", type=Path, required=True,
                        help="installed geometry-camera.jsonl from the same QA run")
    parser.add_argument("--camera-record-tick", type=int,
                        help="select this tick when geometry-camera.jsonl has multiple rows")
    parser.add_argument("--roi", type=int, nargs=4, default=DEFAULT_ROI,
                        metavar=("X0", "Y0", "X1", "Y1"))
    parser.add_argument("--plane-y", type=float, default=PLANE_Y)
    parser.add_argument("--stride", type=int, default=2)
    parser.add_argument("--min-samples", type=int, default=100)
    parser.add_argument("--raw-aspect", type=float, default=RAW_BOARD_ASPECT)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    result = analyze(
        args.neutral_run, args.xe_run, args.camera_record,
        roi=args.roi, plane_y=args.plane_y, stride=args.stride,
        min_samples=args.min_samples, camera_record_tick=args.camera_record_tick,
        raw_aspect=args.raw_aspect, xe_quality=args.xe_quality,
        xe_denoise=args.xe_denoise)
    formatted = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(formatted, encoding="utf-8")
    print(formatted, end="")


if __name__ == "__main__":
    main()
