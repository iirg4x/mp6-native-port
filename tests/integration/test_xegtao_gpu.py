"""Optional GPU harness for the original Intel XeGTAO generated WGSL.

This module is deliberately independent of the production AO renderer.  It
loads WGSL generated from the pinned Intel XeGTAO headers, creates explicit
WebGPU layouts from the declarations in each module, and runs deterministic
synthetic depth scenes.  Install the optional dependencies with::

    pip install --target build/ao-gpu-test-deps wgpu==0.32.0 numpy

The normal CPU test discovery path does not require either dependency and this
module never requests an adapter while it is being imported.  ``Renderer``
also accepts an optional HxWx3 view-space normal field for isolated shading
comparisons and an optional captured-camera aspect.  Run directly to execute
the checks (and optionally request timestamp queries)::

    python tests/integration/test_xegtao_gpu.py [--timing] [--shader-dir DIR] [--backend Vulkan|D3D12]
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
OPTIONAL_DEPS = ROOT / "build" / "ao-gpu-test-deps"
if OPTIONAL_DEPS.is_dir() and str(OPTIONAL_DEPS) not in sys.path:
    sys.path.insert(0, str(OPTIONAL_DEPS))
if str(ROOT / "tests") not in sys.path:
    sys.path.insert(0, str(ROOT / "tests"))
if str(ROOT / "tests" / "integration") not in sys.path:
    sys.path.insert(0, str(ROOT / "tests" / "integration"))

try:
    import numpy as np
except ImportError:  # pragma: no cover - exercised only without the optional QA dependency
    np = None

try:
    import wgpu
except ImportError:  # pragma: no cover - exercised only without the optional QA dependency
    wgpu = None

if np is not None and wgpu is not None:
    # Fixture import errors are test defects, not missing optional GPU support.
    # These helpers do not create a renderer or request a GPU on import.
    from test_ao_gpu import feet_scene, plane
else:
    feet_scene = plane = None


DEFAULT_SHADER_DIR = ROOT / "build" / "ao-upstream-toolchain" / "xegtao"
RENDERER_SHADER_DIR = DEFAULT_SHADER_DIR
TIMING_REQUESTED = False

TIMING_SCOPE = (
    "synthetic XeGTAO GPU stages only; excludes MP6 copy, foreground, and composite"
)
BATCH_TIMING_LABEL = "XeGTAO batch"


class GpuUnavailable(RuntimeError):
    """The optional harness cannot run on this host."""


class ToolchainUnavailable(GpuUnavailable):
    """Generated WGSL or a required optional compiler artifact is absent."""


class ShaderInterfaceError(RuntimeError):
    """A generated module does not expose the interface the stage requires."""


XE_HILBERT_LEVEL = 6
XE_HILBERT_WIDTH = 1 << XE_HILBERT_LEVEL
XE_HILBERT_AREA = XE_HILBERT_WIDTH * XE_HILBERT_WIDTH
_UINT32_MASK = 0xFFFFFFFF


def hilbert_index(pos_x: int, pos_y: int) -> int:
    """Return XeGTAO's unsigned-32 Hilbert index for one 64x64 tile.

    This is a direct Python reconstruction of ``XeGTAO::HilbertIndex``.  The
    explicit masks preserve C/C++ ``uint`` wraparound for callers that probe
    values outside the LUT's 64x64 domain; the renderer indexes the LUT with
    coordinates reduced modulo :data:`XE_HILBERT_WIDTH`.
    """

    pos_x = int(pos_x) & _UINT32_MASK
    pos_y = int(pos_y) & _UINT32_MASK
    index = 0
    cur_level = XE_HILBERT_WIDTH // 2
    while cur_level > 0:
        region_x = 1 if (pos_x & cur_level) > 0 else 0
        region_y = 1 if (pos_y & cur_level) > 0 else 0
        index = (index + cur_level * cur_level * ((3 * region_x) ^ region_y)) & _UINT32_MASK
        if region_y == 0:
            if region_x == 1:
                pos_x = (XE_HILBERT_WIDTH - 1 - pos_x) & _UINT32_MASK
                pos_y = (XE_HILBERT_WIDTH - 1 - pos_y) & _UINT32_MASK
            pos_x, pos_y = pos_y, pos_x
        cur_level //= 2
    return index


def hilbert_lut_values():
    """Build the row-major ``uint32`` values uploaded to the shared LUT."""

    if np is None:  # pragma: no cover - Renderer already rejects this setup
        raise GpuUnavailable("NumPy is unavailable")
    return np.asarray(
        [hilbert_index(x, y) for y in range(XE_HILBERT_WIDTH) for x in range(XE_HILBERT_WIDTH)],
        dtype=np.uint32,
    ).reshape((XE_HILBERT_WIDTH, XE_HILBERT_WIDTH))


@dataclass(frozen=True)
class ResourceDecl:
    key: str
    name: str
    group: int
    binding: int
    kind: str
    format: str | None = None
    access: str | None = None


@dataclass(frozen=True)
class ShaderModuleInfo:
    name: str
    path: Path
    code: str
    entry_point: str
    workgroup_size: tuple[int, int, int]
    resources: dict[str, ResourceDecl]


@dataclass
class RenderResult:
    """Readbacks from one isolated synthetic XeGTAO execution."""

    decoded_visibility: "np.ndarray"
    # Batch mode intentionally does not snapshot intermediate normal/AO
    # textures: doing so would add copies between dispatches and make the
    # single-pass experiment unlike production.  They are therefore absent
    # (None) only for batch renders.
    packed_normals: "np.ndarray | None"
    mip0: "np.ndarray"
    raw_ao: "np.ndarray | None"
    depth_mips: "list[np.ndarray]"
    timings_ms: dict[str, float] | None = None
    timing_scope: str | None = None
    # Exact packed finalAO readback, retained for bit-for-bit parity checks.
    final_ao: "np.ndarray | None" = None

    @property
    def visibility(self):
        """Compatibility spelling for callers that use ``result.visibility``."""

        return self.decoded_visibility


@dataclass
class _ReusedTextures:
    """Four production-equivalent allocations plus harness-only diagnostics."""

    raw_edges: object
    depth_mips: object
    normals_ao1: object
    ao0: object
    normal_snapshot: object | None
    raw_ao_snapshot: object | None

    @property
    def working_textures(self):
        return (self.raw_edges, self.depth_mips, self.normals_ao1, self.ao0)


_DECL_RE = re.compile(
    r"(?P<attrs>(?:@\s*(?:binding|group)\s*\(\s*\d+\s*\)\s*)+)"
    r"var(?P<storage>\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*:\s*(?P<type>[^;]+);",
    re.MULTILINE,
)
_ENTRY_RE = re.compile(
    r"@workgroup_size\s*\(\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*(\d+)\s*)?\)\s*"
    r"@?\s*(?:compute\s*)?fn\s+(?P<name>[A-Za-z_]\w*)\s*\(",
    re.MULTILINE,
)
_NUMERIC_SUFFIX_RE = re.compile(r"_(\d+)$")


def _resource_key(name: str, expected: set[str]) -> str | None:
    """Map a generated declaration to a stable resource prefix.

    Slang normally adds ``_0`` (or another numeric suffix) to names.  Prefer
    an exact declaration, then accept only a numeric suffix attached to one of
    the known resource prefixes; unrelated globals are not silently bound.
    """

    if name in expected:
        return name
    stripped = _NUMERIC_SUFFIX_RE.sub("", name)
    if stripped in expected:
        return stripped
    for candidate in sorted(expected, key=len, reverse=True):
        if name.startswith(candidate + "_") and name[len(candidate) + 1 :].isdigit():
            return candidate
    return None


def _parse_resource_type(storage: str | None, type_text: str) -> tuple[str, str | None, str | None]:
    if storage and storage.strip("<> ") == "uniform":
        return "uniform", None, None
    if type_text.strip() == "sampler":
        return "sampler", None, None
    storage_match = re.fullmatch(
        r"texture_storage_2d\s*<\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_-]+)\s*>",
        type_text.strip(),
    )
    if storage_match:
        access = storage_match.group(2).replace("_", "-")
        return "storage", storage_match.group(1), access
    sampled_match = re.fullmatch(r"texture_2d\s*<\s*([A-Za-z0-9_]+)\s*>", type_text.strip())
    if sampled_match:
        scalar = sampled_match.group(1)
        if scalar in ("u32", "i32"):
            return "sampled-uint", None, None
        if scalar in ("f32", "f16"):
            return "sampled-f32", None, None
    raise ShaderInterfaceError(f"unsupported generated WGSL resource type: {type_text.strip()!r}")


def parse_shader_module(
    path: Path,
    entry_point: str,
    expected_resources: set[str],
    required_resources: set[str],
) -> ShaderModuleInfo:
    """Read a generated module and derive its actual groups and bindings."""

    if not path.is_file():
        raise ToolchainUnavailable(f"missing generated WGSL module: {path}")
    code = path.read_text(encoding="utf-8")
    entry_match = re.search(rf"\bfn\s+{re.escape(entry_point)}\s*\(", code)
    if entry_match is None:
        raise ShaderInterfaceError(f"{path.name} has no entry point {entry_point!r}")

    workgroup_size = None
    for match in _ENTRY_RE.finditer(code):
        if match.group("name") == entry_point:
            workgroup_size = tuple(int(match.group(i) or 1) for i in (1, 2, 3))
            break
    if workgroup_size is None:
        raise ShaderInterfaceError(f"{path.name} has no workgroup size for {entry_point!r}")

    resources: dict[str, ResourceDecl] = {}
    for match in _DECL_RE.finditer(code):
        attrs = match.group("attrs")
        binding_match = re.search(r"@binding\s*\(\s*(\d+)\s*\)", attrs)
        group_match = re.search(r"@group\s*\(\s*(\d+)\s*\)", attrs)
        if binding_match is None or group_match is None:
            continue
        key = _resource_key(match.group("name"), expected_resources)
        if key is None:
            # An unrecognized declaration may be a compiler-retained global.
            # Keep the failure explicit if it is a bindable resource rather
            # than accidentally creating a mismatched pipeline layout.
            raise ShaderInterfaceError(
                f"{path.name} exposes unknown bindable resource {match.group('name')!r}"
            )
        kind, fmt, access = _parse_resource_type(match.group("storage"), match.group("type"))
        if kind == "storage" and access not in ("write", "write-only"):
            raise ShaderInterfaceError(
                f"{path.name}:{match.group('name')} has storage access {access!r}; "
                "the upstream bridge contract requires write-only storage textures"
            )
        decl = ResourceDecl(
            key=key,
            name=match.group("name"),
            group=int(group_match.group(1)),
            binding=int(binding_match.group(1)),
            kind=kind,
            format=fmt,
            access=access,
        )
        if key in resources and resources[key] != decl:
            raise ShaderInterfaceError(f"{path.name} declares {key!r} more than once")
        resources[key] = decl

    missing = sorted(required_resources - resources.keys())
    if missing:
        found = ", ".join(sorted(resources)) or "<none>"
        raise ShaderInterfaceError(
            f"{path.name} is missing required resources {missing}; parsed declarations: {found}"
        )
    return ShaderModuleInfo(
        name=path.stem,
        path=path,
        code=code,
        entry_point=entry_point,
        workgroup_size=workgroup_size,
        resources=resources,
    )


def _validated_aspect(width: int, height: int, aspect: float | None = None) -> float:
    """Return the projection aspect, validating optional captured-camera input."""

    if int(width) <= 0 or int(height) <= 0:
        raise ValueError("projection dimensions must be positive")
    if aspect is None:
        return float(width) / float(height)
    try:
        aspect = float(aspect)
    except (TypeError, ValueError) as exc:
        raise ValueError("aspect must be a finite positive number") from exc
    if not math.isfinite(aspect) or aspect <= 0.0:
        raise ValueError("aspect must be a finite positive number")
    return aspect


def _validated_radius(radius: float) -> float:
    try:
        radius = float(radius)
    except (TypeError, ValueError) as exc:
        raise ValueError("radius must be finite, positive and representable as float32") from exc
    if not math.isfinite(radius) or not 0 < radius <= 3.402823e38 / 1.457:
        raise ValueError("radius must be finite, positive and representable as float32")
    if struct.unpack("<f", struct.pack("<f", radius))[0] == 0:
        raise ValueError("radius must remain positive after float32 conversion")
    return radius


def _validated_depth_mip_offset(depth_mip_offset: float) -> float:
    """Validate the upstream UI's finite depth-mip offset range."""

    try:
        depth_mip_offset = float(depth_mip_offset)
    except (TypeError, ValueError) as exc:
        raise ValueError("depth_mip_offset must be a finite number in [0, 30]") from exc
    if not math.isfinite(depth_mip_offset) or not 0.0 <= depth_mip_offset <= 30.0:
        raise ValueError("depth_mip_offset must be a finite number in [0, 30]")
    return depth_mip_offset


def _pack_constants(width: int, height: int, aspect: float | None = None,
                    radius: float = 32.0, depth_mip_offset: float = 3.3) -> bytes:
    """Pack the independent 96-byte GTAOConstants block from XeGTAO.h."""

    near, far = 20.0, 10000.0
    tan_half_y = math.tan(math.radians(45.0) * 0.5)
    tan_half_x = tan_half_y * _validated_aspect(width, height, aspect)
    depth_unpack = (near * far / (far - near), far / (far - near))
    values = (
        int(width),
        int(height),
        1.0 / width,
        1.0 / height,
        *depth_unpack,
        tan_half_x,
        tan_half_y,
        2.0 * tan_half_x,
        -2.0 * tan_half_y,
        -tan_half_x,
        tan_half_y,
        2.0 * tan_half_x / width,
        -2.0 * tan_half_y / height,
        _validated_radius(radius),
        0.615,
        1.457,
        0.0,
        2.2,
        1.2,
        2.0,
        0.0,
        _validated_depth_mip_offset(depth_mip_offset),
        0,
    )
    # int2 + 12 float scalars from the six Vector2 fields + 9 float scalars
    # + int NoiseIndex = exactly 96 bytes, matching the upstream reflection.
    packed = struct.pack("<2i21f i", *values)
    if len(packed) != 96:
        raise AssertionError(f"GTAOConstants packed to {len(packed)} bytes, expected 96")
    return packed


def _raw_depth(values):
    near, far = np.float32(20.0), np.float32(10000.0)
    z = np.asarray(values, dtype=np.float32)
    safe_z = np.maximum(z, near)
    raw = (far - near * far / safe_z) / (far - near)
    return np.ascontiguousarray(np.where(z > 0.0, raw, 1.0), dtype=np.float32)


def _view_depth_from_raw(raw):
    near, far = np.float32(20.0), np.float32(10000.0)
    mul = near * far / (far - near)
    add = far / (far - near)
    return np.ascontiguousarray(mul / (add - np.asarray(raw, dtype=np.float32)), dtype=np.float32)


def _decode_packed_normals(packed):
    packed = np.asarray(packed, dtype=np.uint32)
    encoded = np.stack(
        (
            (packed & np.uint32(0x7FF)).astype(np.float32) / np.float32(2047.0),
            ((packed >> np.uint32(11)) & np.uint32(0x7FF)).astype(np.float32) / np.float32(2047.0),
            ((packed >> np.uint32(22)) & np.uint32(0x3FF)).astype(np.float32) / np.float32(1023.0),
        ),
        axis=-1,
    )
    return encoded * np.float32(2.0) - np.float32(1.0)


def pack_viewspace_normals(normals, expected_shape: tuple[int, int] | None = None):
    """Validate and pack HxWx3 view-space normals for XeGTAO input.

    XeGTAO's normal texture stores signed normals as R11G11B10 UNORM: the
    first two components use 11 bits and Z uses 10 bits.  The upstream helper
    rounds by truncating ``saturate(value) * bits + 0.5`` after mapping
    ``[-1, 1]`` to ``[0, 1]``.  Keep that exact order here so a supplied
    normal field reaches ``g_srcNormalmapMain`` identically to generated
    normals.
    """

    if np is None:  # pragma: no cover - guarded by Renderer.__init__
        raise GpuUnavailable("NumPy is unavailable")
    try:
        values = np.asarray(normals, dtype=np.float32)
    except (TypeError, ValueError) as exc:
        raise ValueError("normals must be a numeric HxWx3 array") from exc
    if values.ndim != 3 or values.shape[-1] != 3:
        raise ValueError(f"normals must have shape HxWx3, got {values.shape!r}")
    if expected_shape is not None:
        expected = (int(expected_shape[0]), int(expected_shape[1]), 3)
        if values.shape != expected:
            raise ValueError(f"normals must have shape {expected!r}, got {values.shape!r}")
    if not bool(np.isfinite(values).all()):
        raise ValueError("normals must contain only finite values")
    # Measure in float64 so very small finite input vectors do not underflow
    # while checking their non-zero length, and large float32 vectors do not
    # spuriously overflow before normalization.
    length = np.linalg.norm(values.astype(np.float64), axis=-1)
    if not bool(np.isfinite(length).all()) or bool(np.any(length <= 0.0)):
        raise ValueError("normals must have a finite, non-zero length at every pixel")
    normalized = np.ascontiguousarray(values / length[..., None], dtype=np.float32)
    encoded = np.clip(normalized * np.float32(0.5) + np.float32(0.5), 0.0, 1.0)
    x = np.floor(encoded[..., 0] * np.float32(2047.0) + np.float32(0.5)).astype(np.uint32)
    y = np.floor(encoded[..., 1] * np.float32(2047.0) + np.float32(0.5)).astype(np.uint32)
    z = np.floor(encoded[..., 2] * np.float32(1023.0) + np.float32(0.5)).astype(np.uint32)
    return np.ascontiguousarray(x | (y << np.uint32(11)) | (z << np.uint32(22)), dtype=np.uint32)


@dataclass(frozen=True)
class _StageSpec:
    name: str
    entry: str
    expected: frozenset[str]
    required: frozenset[str]


COMMON = frozenset({"GTAOConstantBuffer", "g_samplerPointClamp"})
PREFILTER = _StageSpec(
    "CSPrefilterDepths16x16",
    "CSPrefilterDepths16x16",
    COMMON
    | frozenset(
        {"g_srcRawDepthPrefilter", *(f"g_outWorkingDepthMIP{i}" for i in range(5))}
    ),
    COMMON
    | frozenset(
        {"g_srcRawDepthPrefilter", *(f"g_outWorkingDepthMIP{i}" for i in range(5))}
    ),
)
NORMALS = _StageSpec(
    "CSGenerateNormals",
    "CSGenerateNormals",
    COMMON | frozenset({"g_srcRawDepthNormals", "g_outNormalmap"}),
    COMMON | frozenset({"g_srcRawDepthNormals", "g_outNormalmap"}),
)
MAIN_EXPECTED = COMMON | frozenset(
    {
        "g_srcWorkingDepthMain",
        "g_srcNormalmapMain",
        "g_srcHilbertLUTMain",
        "g_outWorkingAOTerm",
        "g_outWorkingEdges",
    }
)
MAIN_REQUIRED = COMMON | frozenset(
    {
        "g_srcWorkingDepthMain",
        "g_srcNormalmapMain",
        "g_srcHilbertLUTMain",
        "g_outWorkingAOTerm",
        "g_outWorkingEdges",
    }
)
DENOISE_EXPECTED = COMMON | frozenset(
    {"g_srcWorkingAOTerm", "g_srcWorkingEdges", "g_outFinalAOTerm"}
)
DENOISE_REQUIRED = DENOISE_EXPECTED


def _render_stage_order(quality_name: str, denoise_passes: int,
                        supplied_normals: bool) -> list[str]:
    """Return the compute order, omitting normal reconstruction only for input normals."""

    order = [PREFILTER.name]
    if not supplied_normals:
        order.append(NORMALS.name)
    order.append(quality_name)
    if denoise_passes > 1:
        order.extend(["CSDenoisePass"] * (denoise_passes - 1))
    order.append("CSDenoiseLastPass")
    return order


class Renderer:
    """Explicit-layout renderer for the generated XeGTAO compute stages."""

    def __init__(self, shader_dir: Path | str = DEFAULT_SHADER_DIR, timing: bool = False):
        if np is None or wgpu is None:
            raise GpuUnavailable("numpy and wgpu==0.32.0 are required for this optional harness")
        self.shader_dir = Path(shader_dir)
        self.timing = bool(timing)
        self.stages: dict[str, ShaderModuleInfo] = {}
        specs = [PREFILTER, NORMALS]
        for quality in ("Low", "Medium", "High"):
            specs.append(_StageSpec(f"CSGTAO{quality}", f"CSGTAO{quality}", MAIN_EXPECTED, MAIN_REQUIRED))
        if (self.shader_dir / "CSGTAOUltra.wgsl").is_file():
            specs.append(_StageSpec("CSGTAOUltra", "CSGTAOUltra", MAIN_EXPECTED, MAIN_REQUIRED))
        specs.append(_StageSpec("CSDenoiseLastPass", "CSDenoiseLastPass", DENOISE_EXPECTED, DENOISE_REQUIRED))
        optional_denoise = self.shader_dir / "CSDenoisePass.wgsl"
        if optional_denoise.is_file():
            specs.append(_StageSpec("CSDenoisePass", "CSDenoisePass", DENOISE_EXPECTED, DENOISE_REQUIRED))
        for spec in specs:
            self.stages[spec.name] = parse_shader_module(
                self.shader_dir / f"{spec.name}.wgsl", spec.entry, set(spec.expected), set(spec.required)
            )

        self.adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if self.adapter is None:
            raise GpuUnavailable("wgpu could not find a high-performance adapter")
        limit = self.adapter.limits.get(
            "max_storage_textures_per_shader_stage",
            self.adapter.limits.get("max-storage-textures-per-shader-stage", 0),
        )
        if int(limit or 0) < 5:
            raise GpuUnavailable(
                f"adapter max_storage_textures_per_shader_stage={limit!r}, need at least 5"
            )
        required_features = ["timestamp-query"] if self.timing else []
        if self.timing and "timestamp-query" not in self.adapter.features:
            raise GpuUnavailable("timestamp-query was requested but is not supported by the adapter")
        self.device = self.adapter.request_device_sync(
            required_features=required_features,
            required_limits={"max_storage_textures_per_shader_stage": 5},
        )
        self.sampler = self.device.create_sampler(
            address_mode_u="clamp-to-edge",
            address_mode_v="clamp-to-edge",
            address_mode_w="clamp-to-edge",
            mag_filter="nearest",
            min_filter="nearest",
            mipmap_filter="nearest",
        )
        # The upstream Hilbert sequence is invariant across viewport sizes.
        # Keep one immutable sampled LUT resident for the renderer instead of
        # allocating and clearing a viewport-sized texture for every render.
        self._hilbert_lut = self._texture(
            (XE_HILBERT_WIDTH, XE_HILBERT_WIDTH),
            "r32uint",
            wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST,
        )
        self._write(
            self._hilbert_lut,
            hilbert_lut_values(),
            (XE_HILBERT_WIDTH, XE_HILBERT_WIDTH),
        )
        self._hilbert_lut_view = self._hilbert_lut.create_view()
        self.pipelines = {
            name: self._create_pipeline(info)
            for name, info in self.stages.items()
        }
        self.last_timings_ms: dict[str, float] | None = None
        self.last_compute_pass_count = 0
        self._reused_textures: dict[tuple[int, int], _ReusedTextures] = {}

    def _layout_entry(self, resource: ResourceDecl) -> dict:
        base = dict(binding=resource.binding, visibility=wgpu.ShaderStage.COMPUTE)
        if resource.kind == "uniform":
            base["buffer"] = dict(type="uniform", min_binding_size=96)
        elif resource.kind == "sampler":
            # Point sampling intentionally uses a non-filtering sampler so all
            # R32F views remain unfilterable-float, as required by WebGPU.
            base["sampler"] = dict(type="non-filtering")
        elif resource.kind == "sampled-f32":
            base["texture"] = dict(sample_type="unfilterable-float", view_dimension="2d")
        elif resource.kind == "sampled-uint":
            base["texture"] = dict(sample_type="uint", view_dimension="2d")
        elif resource.kind == "storage":
            base["storage_texture"] = dict(
                access="write-only",
                format=resource.format,
                view_dimension="2d",
            )
        else:  # pragma: no cover - parser exhaustiveness guard
            raise ShaderInterfaceError(f"unsupported resource kind {resource.kind!r}")
        return base

    def _create_pipeline(self, info: ShaderModuleInfo):
        module = self.device.create_shader_module(code=info.code)
        max_group = max(resource.group for resource in info.resources.values())
        layouts = []
        for group in range(max_group + 1):
            entries = [
                self._layout_entry(resource)
                for resource in sorted(info.resources.values(), key=lambda item: (item.group, item.binding))
                if resource.group == group
            ]
            layouts.append(self.device.create_bind_group_layout(entries=entries))
        pipeline_layout = self.device.create_pipeline_layout(bind_group_layouts=layouts)
        return self.device.create_compute_pipeline(
            layout=pipeline_layout,
            compute=dict(module=module, entry_point=info.entry_point),
        )

    def _texture(self, size: tuple[int, int], format: str, usage: int, mip_levels: int = 1):
        return self.device.create_texture(
            size=(int(size[0]), int(size[1]), 1),
            mip_level_count=mip_levels,
            format=format,
            usage=usage,
        )

    def _ensure_diagnostic_snapshots(self, reused: _ReusedTextures,
                                     size: tuple[int, int]) -> None:
        """Lazily allocate the harness-only snapshots used by split passes.

        Batch mode never needs these textures.  Keeping them lazy means a
        first batch render with resource reuse has exactly the production
        working allocations; a later default render can still request the
        historical diagnostics without changing the alias plan.
        """

        if reused.normal_snapshot is None or reused.raw_ao_snapshot is None:
            snapshot_usage = wgpu.TextureUsage.COPY_SRC | wgpu.TextureUsage.COPY_DST
            if reused.normal_snapshot is None:
                reused.normal_snapshot = self._texture(size, "r32uint", snapshot_usage)
            if reused.raw_ao_snapshot is None:
                reused.raw_ao_snapshot = self._texture(size, "r32uint", snapshot_usage)

    def _write(self, texture, values, size: tuple[int, int], mip_level: int = 0):
        values = np.ascontiguousarray(values)
        self.device.queue.write_texture(
            dict(texture=texture, mip_level=mip_level),
            values,
            dict(bytes_per_row=int(size[0]) * values.dtype.itemsize, rows_per_image=int(size[1])),
            (int(size[0]), int(size[1]), 1),
        )

    def _read(self, texture, size: tuple[int, int], dtype, mip_level: int = 0):
        width, height = map(int, size)
        raw = self.device.queue.read_texture(
            dict(texture=texture, mip_level=mip_level),
            dict(bytes_per_row=width * np.dtype(dtype).itemsize, rows_per_image=height),
            (width, height, 1),
        )
        return np.frombuffer(raw, dtype=dtype).reshape(height, width).copy()

    def _view(self, texture, mip_level: int = 0, mip_count: int | None = None):
        if mip_count is None:
            return texture.create_view()
        return texture.create_view(base_mip_level=mip_level, mip_level_count=mip_count)

    def _bind_group(self, info: ShaderModuleInfo, pipeline, uniform, objects: dict[str, object]):
        max_group = max(resource.group for resource in info.resources.values())
        groups = []
        for group in range(max_group + 1):
            entries = []
            for resource in sorted(info.resources.values(), key=lambda item: item.binding):
                if resource.group != group:
                    continue
                if resource.key == "GTAOConstantBuffer":
                    resource_obj = dict(buffer=uniform, size=96)
                else:
                    try:
                        resource_obj = objects[resource.key]
                    except KeyError as exc:
                        raise ShaderInterfaceError(
                            f"no runtime object supplied for {info.name}:{resource.key}"
                        ) from exc
                entries.append(dict(binding=resource.binding, resource=resource_obj))
            groups.append(
                self.device.create_bind_group(
                    layout=pipeline.get_bind_group_layout(group), entries=entries
                )
            )
        return groups

    @staticmethod
    def _dispatch(info: ShaderModuleInfo, width: int, height: int) -> tuple[int, int, int]:
        # Match Intel's ceil dispatch. WGSL 17.7.15 defines invalid-address
        # textureStore as a no-op. Do not early-return the prefilter's edge
        # invocations: every lane must reach its workgroup barriers.
        # https://www.w3.org/TR/WGSL/#texturestore
        wx, wy, _ = info.workgroup_size
        if info.name == PREFILTER.name:
            tile_x, tile_y = 2, 2
        elif info.name.startswith("CSDenoise"):
            tile_x, tile_y = 2, 1
        else:
            tile_x, tile_y = 1, 1
        return (
            max(1, math.ceil(width / (wx * tile_x))),
            max(1, math.ceil(height / (wy * tile_y))),
            1,
        )

    def render(self, z, quality: str = "High", denoise_passes: int = 1,
               *, reuse_resources: bool = False, normals=None,
               aspect: float | None = None, radius: float = 32.0,
               depth_mip_offset: float = 3.3,
               batch_compute: bool = False) -> RenderResult:
        """Run XeGTAO, optionally encoding every stage in one compute pass.

        ``batch_compute`` preserves the upstream stage order, bindings, and
        dispatch dimensions while recording all dispatches in one WebGPU
        compute pass.  Because intermediate normal/raw-AO snapshots would
        require copies between those dispatches, batch results expose
        ``packed_normals=None`` and ``raw_ao=None``.  Final AO and every depth
        mip remain available for parity checks.  The default remains the
        historical one-pass-per-stage harness behavior.
        """

        if np is None:  # pragma: no cover - guarded by __init__
            raise GpuUnavailable("NumPy is unavailable")
        z = np.ascontiguousarray(np.asarray(z, dtype=np.float32))
        if z.ndim != 2 or min(z.shape) <= 0:
            raise ValueError(f"z must be a non-empty HxW array, got {z.shape!r}")
        height, width = map(int, z.shape)
        aspect = _validated_aspect(width, height, aspect)
        radius = _validated_radius(radius)
        depth_mip_offset = _validated_depth_mip_offset(depth_mip_offset)
        supplied_packed_normals = (
            None if normals is None else pack_viewspace_normals(normals, (height, width))
        )
        quality_name = f"CSGTAO{quality.capitalize()}"
        if quality_name not in self.pipelines:
            raise ValueError(f"quality shader is unavailable: {quality!r}")
        denoise_passes = int(denoise_passes)
        if denoise_passes not in (1, 2, 3):
            raise ValueError("denoise_passes must be one, two, or three")
        if denoise_passes > 1 and "CSDenoisePass" not in self.pipelines:
            raise ToolchainUnavailable("CSDenoisePass.wgsl is unavailable for a second denoise pass")
        batch_compute = bool(batch_compute)

        usage_sample = wgpu.TextureUsage.TEXTURE_BINDING
        usage_storage = usage_sample | wgpu.TextureUsage.STORAGE_BINDING
        usage_readback = usage_storage | wgpu.TextureUsage.COPY_SRC | wgpu.TextureUsage.COPY_DST
        reused = None
        if reuse_resources:
            size = (width, height)
            reused = self._reused_textures.get(size)
            if reused is None:
                reused = _ReusedTextures(
                    # Production renders canonical depth into this allocation;
                    # include its render usage even though this harness uploads it.
                    raw_edges=self._texture(size, "r32float", usage_readback | wgpu.TextureUsage.RENDER_ATTACHMENT),
                    depth_mips=self._texture(size, "r32float", usage_readback, mip_levels=5),
                    normals_ao1=self._texture(size, "r32uint", usage_readback),
                    ao0=self._texture(size, "r32uint", usage_readback),
                    normal_snapshot=None,
                    raw_ao_snapshot=None,
                )
                self._reused_textures[size] = reused
            raw_depth = main_edges = reused.raw_edges
            depth_working = reused.depth_mips
            normalmap = reused.normals_ao1
            main_ao = reused.ao0
            denoise_outputs = [reused.normals_ao1 if i % 2 == 0 else reused.ao0
                               for i in range(denoise_passes)]
            final_ao = denoise_outputs[-1]
        else:
            raw_depth = self._texture((width, height), "r32float", usage_sample | wgpu.TextureUsage.COPY_DST)
            depth_working = self._texture((width, height), "r32float", usage_readback, mip_levels=5)
            normalmap = self._texture((width, height), "r32uint", usage_readback)
            main_ao = self._texture((width, height), "r32uint", usage_readback)
            main_edges = self._texture((width, height), "r32float", usage_readback)
            final_ao = self._texture((width, height), "r32uint", usage_readback)
            denoise_outputs = [self._texture((width, height), "r32uint", usage_readback)
                               for _ in range(denoise_passes - 1)] + [final_ao]

        if reused is not None and not batch_compute:
            # Diagnostic copies are a split-pass compatibility aid only.
            self._ensure_diagnostic_snapshots(reused, (width, height))

        raw = _raw_depth(z)
        self._write(raw_depth, raw, (width, height))
        if not reuse_resources:
            self._write(normalmap, np.zeros((height, width), dtype=np.uint32), (width, height))
            self._write(main_ao, np.zeros((height, width), dtype=np.uint32), (width, height))
            self._write(main_edges, np.zeros((height, width), dtype=np.float32), (width, height))
            self._write(final_ao, np.zeros((height, width), dtype=np.uint32), (width, height))
            for intermediate in denoise_outputs[:-1]:
                self._write(intermediate, np.zeros((height, width), dtype=np.uint32), (width, height))
            for level in range(5):
                level_size = (max(1, width >> level), max(1, height >> level))
                self._write(
                    depth_working,
                    np.zeros((level_size[1], level_size[0]), dtype=np.float32),
                    level_size,
                    mip_level=level,
                )
        # Reused resources retain previous-frame contents until their producer
        # runs, as in production. In particular, clearing main_edges here would
        # destroy the canonical raw-depth upload in the aliased allocation.
        if supplied_packed_normals is not None:
            # This upload is the only producer for the optional normal path;
            # normal reconstruction below is deliberately skipped.
            self._write(normalmap, supplied_packed_normals, (width, height))

        uniform = self.device.create_buffer_with_data(
            data=_pack_constants(width, height, aspect, radius, depth_mip_offset),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        depth_full_view = self._view(depth_working, 0, 5)
        stage_objects = {
            "g_samplerPointClamp": self.sampler,
            "g_srcRawDepthPrefilter": self._view(raw_depth),
            "g_srcRawDepthNormals": self._view(raw_depth),
            "g_srcWorkingDepthMain": depth_full_view,
            "g_srcNormalmapMain": self._view(normalmap),
            "g_srcHilbertLUTMain": self._hilbert_lut_view,
            "g_outNormalmap": self._view(normalmap),
            "g_outWorkingAOTerm": self._view(main_ao),
            "g_outWorkingEdges": self._view(main_edges),
            "g_srcWorkingAOTerm": self._view(main_ao),
            "g_srcWorkingEdges": self._view(main_edges),
            "g_outFinalAOTerm": self._view(final_ao),
        }
        for level in range(5):
            stage_objects[f"g_outWorkingDepthMIP{level}"] = self._view(depth_working, level, 1)

        encoder = self.device.create_command_encoder()
        query = None
        stage_order = _render_stage_order(
            quality_name, denoise_passes, supplied_packed_normals is not None
        )
        self.last_compute_pass_count = 1 if batch_compute else len(stage_order)
        query_count = 2 if batch_compute else 2 * len(stage_order)
        if self.timing:
            query = self.device.create_query_set(type="timestamp", count=query_count)

        if reused is not None and supplied_packed_normals is not None and not batch_compute:
            # The first denoise pass aliases normals_ao1. Snapshot the supplied
            # input before any split compute pass can overwrite that shared
            # texture. Batch mode intentionally has no diagnostic snapshot.
            encoder.copy_texture_to_texture(
                dict(texture=normalmap), dict(texture=reused.normal_snapshot),
                (width, height, 1),
            )

        denoise_index = 0
        previous_ao = main_ao
        if batch_compute:
            # Keep every stage's pipeline/bind groups/dispatch in one pass.
            # In particular, do not insert diagnostic copies between these
            # dispatches: production relies on the existing alias plan and
            # only the final AO/depth-MIP outputs are read back below.
            timestamps = {}
            if query is not None:
                timestamps = dict(
                    timestamp_writes=dict(
                        query_set=query,
                        beginning_of_pass_write_index=0,
                        end_of_pass_write_index=1,
                    )
                )
            compute = encoder.begin_compute_pass(label=BATCH_TIMING_LABEL, **timestamps)
            for stage_name in stage_order:
                info = self.stages[stage_name]
                objects = dict(stage_objects)
                if stage_name.startswith("CSDenoise"):
                    destination = denoise_outputs[denoise_index]
                    objects["g_srcWorkingAOTerm"] = self._view(previous_ao)
                    objects["g_outFinalAOTerm"] = self._view(destination)
                    previous_ao = destination
                    denoise_index += 1
                bind_groups = self._bind_group(info, self.pipelines[stage_name], uniform, objects)
                compute.set_pipeline(self.pipelines[stage_name])
                # Set every declared group slot, including empty-gap layouts,
                # for each stage before dispatching in the shared pass.
                for group_index, bind_group in enumerate(bind_groups):
                    compute.set_bind_group(group_index, bind_group)
                compute.dispatch_workgroups(*self._dispatch(info, width, height))
            compute.end()
        else:
            for index, stage_name in enumerate(stage_order):
                info = self.stages[stage_name]
                objects = dict(stage_objects)
                if stage_name.startswith("CSDenoise"):
                    destination = denoise_outputs[denoise_index]
                    objects["g_srcWorkingAOTerm"] = self._view(previous_ao)
                    objects["g_outFinalAOTerm"] = self._view(destination)
                    previous_ao = destination
                    denoise_index += 1
                bind_groups = self._bind_group(info, self.pipelines[stage_name], uniform, objects)
                timestamps = {}
                if query is not None:
                    timestamps = dict(
                        timestamp_writes=dict(
                            query_set=query,
                            beginning_of_pass_write_index=2 * index,
                            end_of_pass_write_index=2 * index + 1,
                        )
                    )
                compute = encoder.begin_compute_pass(label=stage_name, **timestamps)
                compute.set_pipeline(self.pipelines[stage_name])
                for group_index, bind_group in enumerate(bind_groups):
                    compute.set_bind_group(group_index, bind_group)
                compute.dispatch_workgroups(*self._dispatch(info, width, height))
                compute.end()
                if reused is not None and stage_name in (NORMALS.name, quality_name):
                    # Harness-only copies preserve diagnostics immediately
                    # before denoise overwrites normals/AO0. They are outside
                    # every pass and do not belong in batch mode/production.
                    source, destination = ((normalmap, reused.normal_snapshot)
                                           if stage_name == NORMALS.name
                                           else (main_ao, reused.raw_ao_snapshot))
                    encoder.copy_texture_to_texture(
                        dict(texture=source), dict(texture=destination),
                        (width, height, 1),
                    )

        timing_buffer = None
        if query is not None:
            timing_buffer = self.device.create_buffer(
                size=8 * query_count,
                usage=wgpu.BufferUsage.QUERY_RESOLVE | wgpu.BufferUsage.COPY_SRC,
            )
            encoder.resolve_query_set(query, 0, query_count, timing_buffer, 0)
        self.device.queue.submit([encoder.finish()])

        timings = None
        if timing_buffer is not None:
            values = np.frombuffer(self.device.queue.read_buffer(timing_buffer), dtype=np.uint64)
            if batch_compute:
                timings = {
                    BATCH_TIMING_LABEL: (int(values[1]) - int(values[0])) / 1e6,
                }
            else:
                timings = {
                    (stage_name if stage_order.count(stage_name) == 1 else f"{stage_name}[{index}]"):
                        (int(values[index * 2 + 1]) - int(values[index * 2])) / 1e6
                    for index, stage_name in enumerate(stage_order)
                }
            self.last_timings_ms = timings
            if TIMING_REQUESTED:
                print(json.dumps({"scope": TIMING_SCOPE, "passes_ms": timings}, sort_keys=True), flush=True)

        # Each readback is deliberately isolated and uses the actual texture
        # mip/origin; no readback includes production copy/composite work.
        depth_mips = [self._read(depth_working, (max(1, width >> level), max(1, height >> level)),
                                np.float32, mip_level=level) for level in range(5)]
        mip0 = depth_mips[0]
        if batch_compute:
            # Intermediate snapshots are intentionally invalid in the
            # one-pass path; reading them would either observe an alias that
            # denoise has already overwritten or require a copy between
            # dispatches.  Final AO and depth mips remain authoritative.
            packed_normals = None
            raw_ao = None
        else:
            packed_normals = self._read(reused.normal_snapshot if reused else normalmap,
                                        (width, height), np.uint32)
            raw_ao = self._read(reused.raw_ao_snapshot if reused else main_ao,
                               (width, height), np.uint32)
        decoded_ao = self._read(final_ao, (width, height), np.uint32)
        decoded_visibility = decoded_ao.astype(np.float32) / np.float32(255.0)
        return RenderResult(
            decoded_visibility=decoded_visibility,
            packed_normals=packed_normals,
            mip0=mip0,
            raw_ao=raw_ao,
            depth_mips=depth_mips,
            timings_ms=timings,
            timing_scope=TIMING_SCOPE if timings is not None else None,
            final_ao=decoded_ao,
        )


HAS_GPU_DEPS = np is not None and wgpu is not None and plane is not None and feet_scene is not None


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class XeGTAOGPU(unittest.TestCase):
    renderer: Renderer

    @classmethod
    def setUpClass(cls):
        try:
            cls.renderer = Renderer(RENDERER_SHADER_DIR, timing=TIMING_REQUESTED)
        except (GpuUnavailable, ToolchainUnavailable) as exc:
            raise unittest.SkipTest(str(exc)) from exc
        print(
            "XeGTAO GPU adapter:",
            json.dumps(dict(cls.renderer.adapter.info), sort_keys=True, default=str),
            flush=True,
        )

    def _render(self, z, label: str) -> RenderResult:
        try:
            return self.renderer.render(z)
        except Exception as exc:
            # Keep padded-dispatch failures visible with the exact scene size;
            # broad optional-GPU skips would hide shader bounds regressions.
            raise AssertionError(
                f"upstream XeGTAO GPU stage failed for {label} shape={np.asarray(z).shape}: "
                f"{type(exc).__name__}: {exc}"
            ) from exc

    def test_flat_front_plane_is_mostly_near_one(self):
        width, height = 256, 144
        z = np.full((height, width), 400.0, dtype=np.float32)
        result = self._render(z, "flat-front-plane")
        visibility = result.decoded_visibility[2:-2, 2:-2]
        self.assertTrue(np.isfinite(visibility).all())
        self.assertGreater(float(np.quantile(visibility, 0.05)), 0.90)
        self.assertGreater(float(np.median(visibility)), 0.97)
        self.assertGreaterEqual(float(visibility.min()), 0.0)
        self.assertLessEqual(float(visibility.max()), 1.0)
        normal_length = np.linalg.norm(_decode_packed_normals(result.packed_normals), axis=-1)[2:-2, 2:-2]
        self.assertTrue(np.isfinite(normal_length).all())
        self.assertGreater(float(np.median(normal_length)), 0.97)
        self.assertLess(float(np.median(normal_length)), 1.03)

    def test_supplied_normals_roundtrip_and_reuse_without_stale_input(self):
        scene = np.full((73, 129), 1000.0, dtype=np.float32)
        scene[22:52, 40:85] = 980
        for vector in ((0., 0., -1.), (.2, .1, -1.), (0., 0., -1.)):
            normals = np.empty((*scene.shape, 3), dtype=np.float32)
            normals[:] = vector
            expected_packed = pack_viewspace_normals(normals, scene.shape)
            for passes in (1, 2, 3):
                expected = self.renderer.render(scene, denoise_passes=passes,
                                                normals=normals, aspect=1.59)
                actual = self.renderer.render(scene, denoise_passes=passes,
                                              normals=normals, aspect=1.59,
                                              reuse_resources=True)
                np.testing.assert_array_equal(actual.packed_normals, expected_packed)
                for name in ("decoded_visibility", "packed_normals", "raw_ao"):
                    np.testing.assert_array_equal(getattr(actual, name), getattr(expected, name))
                self.assertTrue(np.isfinite(actual.visibility).all())

    def test_upstream_mip0_roundtrips_positive_z(self):
        width, height = 256, 144
        y, x = np.mgrid[:height, :width]
        z = (120.0 + 0.75 * x + 1.25 * y).astype(np.float32)
        result = self._render(z, "mip0-roundtrip")
        expected = _view_depth_from_raw(_raw_depth(z))
        interior = (slice(2, height - 2), slice(2, width - 2))
        np.testing.assert_allclose(result.mip0[interior], expected[interior], rtol=3e-3, atol=0.2)
        self.assertTrue(np.isfinite(result.mip0).all())

    def test_all_depth_mips_and_normal_direction_on_flat_surface(self):
        result = self._render(np.full((145, 257), 400.0, dtype=np.float32), "flat-mip-chain")
        for level, mip in enumerate(result.depth_mips):
            with self.subTest(mip=level):
                self.assertEqual(mip.shape, (max(1, 145 >> level), max(1, 257 >> level)))
                np.testing.assert_allclose(mip, 400.0, rtol=1e-4, atol=1e-3)
        packed = result.packed_normals[2:-2, 2:-2]
        normal = np.stack(((packed & 2047) / 2047.0,
                           ((packed >> 11) & 2047) / 2047.0,
                           ((packed >> 22) & 1023) / 1023.0), axis=-1) * 2.0 - 1.0
        # The upstream coordinate system points forward along positive Z;
        # a front-facing flat receiver must point back toward the camera.
        self.assertLess(float(np.max(np.abs(normal[..., :2]))), 0.002)
        self.assertLess(float(np.max(normal[..., 2])), -0.99)

    def test_all_clear_depth_is_finite_and_visibility_bounded(self):
        width, height = 33, 19
        result = self._render(np.zeros((height, width), dtype=np.float32), "all-clear")
        self.assertTrue(np.isfinite(result.mip0).all())
        self.assertTrue(np.isfinite(result.decoded_visibility).all())
        self.assertTrue(np.isfinite(result.packed_normals.astype(np.float32)).all())
        self.assertGreaterEqual(float(result.decoded_visibility.min()), 0.0)
        self.assertLessEqual(float(result.decoded_visibility.max()), 1.0)

    def test_contact_scene_is_deterministic_and_occludes(self):
        width, height = 256, 144
        ground, _ = plane(width, height)
        scene, contact = feet_scene(width, height)
        baseline = self._render(ground, "contact-ground")
        first = self._render(scene, "contact-feet-first")
        second = self._render(scene, "contact-feet-second")
        np.testing.assert_array_equal(first.decoded_visibility, second.decoded_visibility)
        self.assertTrue(np.isfinite(first.decoded_visibility).all())
        self.assertGreater(float(np.max(np.abs(first.decoded_visibility - baseline.decoded_visibility))), 1e-4)
        self.assertGreater(float(np.quantile(baseline.decoded_visibility[contact] - first.decoded_visibility[contact], 0.75)), 1e-3)

    def test_even_odd_and_small_sizes_survive_padded_dispatch(self):
        for width, height in ((256, 144), (257, 145), (33, 19)):
            with self.subTest(size=(width, height)):
                z, _ = plane(width, height)
                result = self._render(z, f"size-{width}x{height}")
                self.assertEqual(result.decoded_visibility.shape, (height, width))
                self.assertEqual(result.packed_normals.shape, (height, width))
                self.assertTrue(np.isfinite(result.decoded_visibility).all())
                self.assertGreaterEqual(float(result.decoded_visibility.min()), 0.0)
                self.assertLessEqual(float(result.decoded_visibility.max()), 1.0)

    def test_all_quality_levels_and_spatial_denoise_counts(self):
        scene, _ = feet_scene(129, 73)
        for quality in ("Low", "Medium", "High", "Ultra"):
            results = []
            for passes in (1, 2, 3):
                with self.subTest(quality=quality, denoise_passes=passes):
                    result = self.renderer.render(scene, quality=quality, denoise_passes=passes)
                    self.assertEqual(result.visibility.shape, scene.shape)
                    self.assertGreaterEqual(float(result.visibility.min()), 0.0)
                    self.assertLessEqual(float(result.visibility.max()), 1.0)
                    self.assertLess(float(result.visibility.min()), 0.95)
                    results.append(result.visibility)
            # A second spatial denoise pass must actually consume the first
            # pass, not repeat a stale input or return the main-pass texture.
            self.assertGreater(float(np.max(np.abs(results[0] - results[1]))), 1.0 / 255.0)

    def test_resource_reuse_is_bit_identical_for_every_policy_and_consecutive_frames(self):
        def identical(expected, actual):
            # Compare bits, including all diagnostic outputs. Visibility is an
            # exact deterministic decode of the final packed uint AO term.
            for name in ("decoded_visibility", "packed_normals", "raw_ao"):
                np.testing.assert_array_equal(getattr(actual, name).view(np.uint32),
                                              getattr(expected, name).view(np.uint32), err_msg=name)
            for level, (before, after) in enumerate(zip(expected.depth_mips, actual.depth_mips)):
                np.testing.assert_array_equal(after.view(np.uint32), before.view(np.uint32),
                                              err_msg=f"depth mip {level}")

        for width, height in ((128, 72), (129, 73), (33, 19)):
            ground, _ = plane(width, height)
            contact, _ = feet_scene(width, height)
            clear = np.zeros((height, width), dtype=np.float32)
            # Returning to clear also catches previous-frame contact data that
            # survived a producer pass or an incorrect alias transition.
            scenes = (("clear", clear), ("plane", ground), ("contacts", contact), ("clear-again", clear))
            allocation_ids = None
            for quality in ("Low", "Medium", "High", "Ultra"):
                for passes in (1, 2, 3):
                    for name, scene in scenes:
                        with self.subTest(size=(width, height), quality=quality, denoise=passes, scene=name):
                            baseline = self.renderer.render(scene, quality, passes)
                            first = self.renderer.render(scene, quality, passes, reuse_resources=True)
                            repeated = self.renderer.render(scene, quality, passes, reuse_resources=True)
                            identical(baseline, first)
                            identical(baseline, repeated)
                            self.assertTrue(np.isfinite(first.visibility).all())
                            self.assertGreaterEqual(float(first.visibility.min()), 0.0)
                            self.assertLessEqual(float(first.visibility.max()), 1.0)
                            reused = self.renderer._reused_textures[(width, height)]
                            ids = tuple(id(texture) for texture in reused.working_textures)
                            self.assertEqual(len(set(ids)), 4)
                            if allocation_ids is None:
                                allocation_ids = ids
                            self.assertEqual(ids, allocation_ids, "consecutive frames must reuse the same allocations")

    def test_upstream_ultra_and_soft_denoise_reuse(self):
        if 'CSGTAOUltra' not in self.renderer.pipelines:
            self.skipTest('regenerate optional upstream Ultra shader')
        scene, _ = feet_scene(129, 73)
        for quality in ('High', 'Ultra'):
            previous = self.renderer.render(scene, quality, 2)
            expected = self.renderer.render(scene, quality, 3)
            # Third pass must consume second-pass output, not repeat a pass
            # from raw AO. Both aliasing strategies preserve raw diagnostics.
            self.assertGreater(float(np.max(np.abs(previous.visibility-expected.visibility))), 1./255.)
            for _ in range(2):
                actual = self.renderer.render(scene, quality, 3, reuse_resources=True)
                for name in ('decoded_visibility','packed_normals','raw_ao'):
                    np.testing.assert_array_equal(getattr(actual,name),getattr(expected,name))


def main(argv: list[str] | None = None) -> int:
    global RENDERER_SHADER_DIR, TIMING_REQUESTED
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--shader-dir", type=Path, default=DEFAULT_SHADER_DIR)
    parser.add_argument("--timing", action="store_true")
    parser.add_argument("--backend", choices=("Vulkan", "D3D12"))
    parsed, unittest_args = parser.parse_known_args(argv)
    RENDERER_SHADER_DIR = parsed.shader_dir
    TIMING_REQUESTED = bool(parsed.timing)
    if parsed.backend:
        os.environ["WGPU_BACKEND_TYPE"] = parsed.backend
    unittest_argv = [sys.argv[0], *unittest_args]
    result = unittest.main(module=__name__, argv=unittest_argv, exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":  # pragma: no cover - direct optional harness entry point
    raise SystemExit(main())
