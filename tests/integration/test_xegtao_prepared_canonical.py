"""QA-only equivalence experiment for prepared-depth/canonical-depth fusion.

The baseline runs the exact production ordered depth-preparation raster, then
the exact native XeGTAO canonical-depth raster from
``src/gx/xegtao/adapter_shader.hpp``.  The candidate is the canonical variant
emitted by the same compiled production factory as the baseline; no test-local
WGSL reconstruction is substituted.

This module never requests a GPU at import time.  Optional dependencies are
``numpy`` and ``wgpu==0.32.0`` in ``build/ao-gpu-test-deps``.  Normal CPU
unittest discovery can import the source transformation and packing helpers
without either dependency.  Run GPU parity checks directly with::

    python tests/integration/test_xegtao_prepared_canonical.py --backend Vulkan -v

The opt-in timing experiment measures only resident preparation/canonical
passes.  It excludes the rest of XeGTAO, the native frame, Android, and all
CPU uploads/readbacks from timestamped intervals.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import sys
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
OPTIONAL_DEPS = ROOT / "build" / "ao-gpu-test-deps"
if OPTIONAL_DEPS.is_dir() and str(OPTIONAL_DEPS) not in sys.path:
    sys.path.insert(0, str(OPTIONAL_DEPS))
TESTS = ROOT / "tests"
INTEGRATION = TESTS / "integration"
for _path in (TESTS, INTEGRATION):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

try:  # Optional GPU-only dependency; CPU source checks remain importable.
    import numpy as np
except ImportError:  # pragma: no cover - exercised on CPU-only hosts
    np = None

try:  # Optional GPU-only dependency; importing never requests an adapter.
    import wgpu
except ImportError:  # pragma: no cover - exercised on CPU-only hosts
    wgpu = None

try:
    from ao_shader_source import shader_source as load_shader_source
except ImportError:  # pragma: no cover - malformed checkout
    load_shader_source = None

try:
    import test_xegtao_adapter as adapter_harness
except ImportError:  # pragma: no cover - malformed checkout
    adapter_harness = None


HAS_GPU_DEPS = np is not None and wgpu is not None


class ShaderTransformError(RuntimeError):
    """The bounded in-memory candidate transform no longer has its anchors."""


class GpuUnavailable(RuntimeError):
    """The optional WebGPU experiment cannot run on this host."""


def _function(source: str, name: str) -> str:
    """Return one WGSL function, failing closed on duplicate/unclosed bodies."""

    matches = list(re.finditer(rf"\bfn\s+{re.escape(name)}\s*\(", source))
    if len(matches) != 1:
        raise ShaderTransformError(f"expected one WGSL function {name!r}, found {len(matches)}")
    opening = source.find("{", matches[0].start())
    if opening < 0:
        raise ShaderTransformError(f"WGSL function {name!r} has no body")
    depth = 1
    for index in range(opening + 1, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[matches[0].start() : index + 1]
    raise ShaderTransformError(f"WGSL function {name!r} is unclosed")


def _adapter_source() -> str:
    if adapter_harness is None:
        raise ShaderTransformError("test_xegtao_adapter.py is unavailable")
    return adapter_harness._adapter_source()


def production_source(*, compatibility: bool, depth_samples: int, decals: bool,
                      canonical_depth: bool = False) -> str:
    """Load the exact C++-selected source for one factory configuration."""

    if load_shader_source is None:
        raise ShaderTransformError("tests/ao_shader_source.py is unavailable")
    if depth_samples not in (1, 4):
        raise ValueError(f"depth_samples must be 1 or 4, got {depth_samples}")
    return load_shader_source(
        compatibility=compatibility,
        prepared_depth=True,
        attachment_depth=True,
        multisampled_depth=depth_samples > 1,
        decals=decals,
        canonical_depth=canonical_depth,
    )


def _validate_canonical_source(source: str) -> str:
    """Validate the actual factory's canonical entry and binding contract."""

    required = (
        "struct Params {",
        "@group(0) @binding(0) var<uniform> p: Params;",
        "@group(0) @binding(1) var sceneDepth:",
        "struct AdapterParams {",
    )
    for anchor in required:
        if anchor not in source:
            raise ShaderTransformError(f"canonical production source anchor missing: {anchor!r}")
    if len(re.findall(r"@fragment\s+fn\s+fs_canonical_depth\s*\(", source)) != 1:
        raise ShaderTransformError("canonical production entry point is not unique")
    binding = re.findall(
        r"@group\(0\)\s*@binding\(7\)\s+var<uniform>\s+\w+\s*:\s*AdapterParams\s*;",
        source,
    )
    if len(binding) != 1:
        raise ShaderTransformError(
            f"canonical AdapterParams binding 7 must be unique, found {len(binding)}"
        )
    if re.search(
        r"@group\(0\)\s*@binding\(6\)\s+var<uniform>\s+\w+\s*:\s*AdapterParams\s*;",
        source,
    ):
        raise ShaderTransformError("canonical AdapterParams must use binding 7")
    if "@fragment fn fs_prepared_canonical" in source:
        raise ShaderTransformError("manual prepared canonical entry survived factory selection")
    params = re.search(r"struct\s+AdapterParams\s*\{(.*?)\};", source, re.DOTALL)
    if params is None or not all(
        field in params.group(1) for field in ("depth: vec4f", "viewport: vec4f", "scissor: vec4f")
    ):
        raise ShaderTransformError("canonical AdapterParams must expose depth, viewport, and scissor vec4f fields")
    return source


def candidate_source(production: str | None = None, adapter: str | None = None, *,
                     compatibility: bool = False, depth_samples: int = 1,
                     decals: bool = True) -> str:
    """Select and validate the canonical shader emitted by production C++."""

    if adapter is not None:
        raise ShaderTransformError(
            "manual adapter source injection is unsupported; use the production canonical factory"
        )
    if production is None:
        production = production_source(
            compatibility=compatibility,
            depth_samples=depth_samples,
            decals=decals,
            canonical_depth=True,
        )
    return _validate_canonical_source(production)


def source_hash(source: str) -> str:
    return hashlib.sha256(source.encode("utf-8")).hexdigest()


def _test_ao_gpu_fixture_hash() -> str:
    """Hash the deterministic ``test_ao_gpu.py`` fixture module's source.

    Read from disk rather than imported: importing it needs the optional
    wgpu QA dependency, which the hash does not."""

    return hashlib.sha256((Path(__file__).resolve().parent / "test_ao_gpu.py").read_bytes()).hexdigest()


def bit_diagnostics(expected, actual) -> dict[str, int]:
    """Report exact-buffer differences without widening the parity criterion."""

    expected_bits = np.ascontiguousarray(expected, dtype=np.float32).view(np.uint32)
    actual_bits = np.ascontiguousarray(actual, dtype=np.float32).view(np.uint32)
    if expected_bits.shape != actual_bits.shape:
        return {"mismatch_count": -1, "max_ulp": -1}
    delta = np.abs(expected_bits.astype(np.int64) - actual_bits.astype(np.int64))
    return {
        "mismatch_count": int(np.count_nonzero(expected_bits != actual_bits)),
        "max_ulp": int(delta.max()) if delta.size else 0,
    }


def pack_adapter_params(depth, viewport, scissor) -> bytes:
    values = (*depth, *viewport, *scissor)
    if len(values) != 12:
        raise ValueError(f"AdapterParams requires 12 floats, got {len(values)}")
    packed = struct.pack("<12f", *(float(value) for value in values))
    if len(packed) != 48:
        raise AssertionError(f"AdapterParams packed to {len(packed)} bytes")
    return packed


def pack_production_params(
    width: int,
    height: int,
    viewport,
    scissor,
    depth_range,
    reversed_z: bool,
    *,
    strength: float = 0.35,
) -> bytes:
    values = (
        math.tan(math.radians(45.0) / 2.0),
        width / height,
        20.0,
        10000.0,
        *viewport,
        *scissor,
        *depth_range,
        float(reversed_z),
        strength,
        float(width),
        float(height),
        float(width),
        float(height),
    )
    if len(values) != 20:
        raise ValueError(f"production Params requires 20 floats, got {len(values)}")
    packed = struct.pack("<20f", *(float(value) for value in values))
    if len(packed) != 80:
        raise AssertionError(f"production Params packed to {len(packed)} bytes")
    return packed


def adapter_depth_constants() -> tuple[float, float, float, float]:
    near, far = 20.0, 10000.0
    return (
        near * far / (far - near),
        far / (far - near),
        0.4,
        0.0,
    )


def _raw_depth(values, depth_range, reversed_z):
    if np is None:
        raise GpuUnavailable("NumPy is required for synthetic depth generation")
    values = np.ascontiguousarray(values, dtype=np.float32)
    near, far = 20.0, 10000.0
    safe_z = np.maximum(values, np.float32(near))
    raw = (np.float32(far) - np.float32(near * far) / safe_z) / np.float32(far - near)
    raw = np.where(values > np.float32(0), raw, np.float32(1.0))
    lo, hi = (np.float32(depth_range[0]), np.float32(depth_range[1]))
    raw = lo + raw * (hi - lo)
    if reversed_z:
        raw = np.float32(1.0) - raw
    return np.ascontiguousarray(raw, dtype=np.float32)


def _expected_prepared_viewz(values, depth_range, reversed_z):
    """CPU oracle for semantic decal/water assertions (not parity tolerance)."""

    raw = _raw_depth(values, depth_range, reversed_z)
    lo = np.float32(depth_range[0])
    hi = np.float32(depth_range[1])
    forward = np.float32(1.0) - raw if reversed_z else raw
    d = (forward - lo) / (hi - lo)
    near = np.float32(20.0)
    far = np.float32(10000.0)
    result = (near * far) / (far - d * (far - near))
    return np.where((d >= np.float32(0.0)) & (d < np.float32(0.999999)), result, np.float32(0.0))


def _transparent_foliage(width: int, height: int):
    return np.zeros((height, width, 4), dtype=np.float32)


@dataclass
class CanonicalParity:
    """Readbacks exposed for extending parity to AO/composite stages."""

    baseline_prepared: object
    baseline_canonical: object
    candidate_canonical: object

    @property
    def exact(self) -> bool:
        return bool(
            np.array_equal(
                self.baseline_canonical.view(np.uint32),
                self.candidate_canonical.view(np.uint32),
            )
        )


@dataclass
class _CaseResources:
    width: int
    height: int
    depth_attachment: object
    baseline_prepared: object
    baseline_canonical: object
    candidate_canonical: object
    baseline_prepare_group: object
    baseline_adapter_group: object
    candidate_group: object
    keep_alive: tuple[object, ...]

    def release(self) -> None:
        """Destroy per-case allocations after synchronous readback.

        Pipelines, layouts, the device, and the adapter are renderer-owned and
        intentionally remain resident.  Textures, uniform buffers, bind groups,
        and the seed group belong only to this case and must not accumulate
        across the high-resolution timing matrix.
        """

        for resource in (
            self.depth_attachment, self.baseline_prepared,
            self.baseline_canonical, self.candidate_canonical,
            *self.keep_alive,
        ):
            destroy = getattr(resource, "destroy", None)
            if destroy is not None:
                destroy()
        self.keep_alive = ()
        self.depth_attachment = None
        self.baseline_prepared = None
        self.baseline_canonical = None
        self.candidate_canonical = None
        self.baseline_prepare_group = None
        self.baseline_adapter_group = None
        self.candidate_group = None


class PreparedCanonicalRenderer:
    """Small local renderer containing only the two parity paths."""

    def __init__(
        self,
        *,
        compatibility: bool,
        depth_samples: int,
        decals: bool,
        timing: bool = False,
        mask_format: str = "rgba8unorm",
    ):
        if not HAS_GPU_DEPS:
            raise GpuUnavailable("numpy and wgpu==0.32.0 are required")
        if depth_samples not in (1, 4):
            raise ValueError(f"depth_samples must be 1 or 4, got {depth_samples}")
        self.compatibility = bool(compatibility)
        self.depth_samples = int(depth_samples)
        self.decals = bool(decals)
        if mask_format not in ("rgba8unorm", "rgba32float"):
            raise ValueError("mask_format must be rgba8unorm or rgba32float")
        self.mask_format = mask_format
        self.production = production_source(
            compatibility=self.compatibility,
            depth_samples=self.depth_samples,
            decals=self.decals,
            canonical_depth=False,
        )
        self.adapter_source = _adapter_source()
        self.candidate = candidate_source(
            compatibility=self.compatibility,
            depth_samples=self.depth_samples,
            decals=self.decals,
        )
        self.adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if self.adapter is None:
            raise GpuUnavailable("wgpu could not find a high-performance adapter")
        required = ["timestamp-query"] if timing else []
        self.device = self.adapter.request_device_sync(required_features=required)
        self._create_pipelines()

    def _create_pipelines(self):
        device = self.device
        production_module = device.create_shader_module(code=self.production)
        candidate_module = device.create_shader_module(code=self.candidate)
        adapter_module = device.create_shader_module(code=self.adapter_source)
        depth_texture_entry = dict(
            binding=1,
            visibility=wgpu.ShaderStage.FRAGMENT,
            texture=dict(
                sample_type="depth",
                view_dimension="2d",
                multisampled=self.depth_samples > 1,
            ),
        )
        ordinary_texture_entries = [
            dict(
                binding=index,
                visibility=wgpu.ShaderStage.FRAGMENT,
                texture=dict(sample_type="unfilterable-float", view_dimension="2d"),
            )
            for index in (2, 3, 4, 5)
        ]
        production_entries = [
            dict(binding=0, visibility=wgpu.ShaderStage.FRAGMENT, buffer=dict(type="uniform", min_binding_size=80)),
            depth_texture_entry,
            *ordinary_texture_entries,
        ]
        self.production_layout = device.create_bind_group_layout(entries=production_entries)
        candidate_entries = [
            *production_entries,
            dict(binding=7, visibility=wgpu.ShaderStage.FRAGMENT, buffer=dict(type="uniform", min_binding_size=48)),
        ]
        self.candidate_layout = device.create_bind_group_layout(entries=candidate_entries)
        self.adapter_layout = device.create_bind_group_layout(
            entries=[
                dict(binding=0, visibility=wgpu.ShaderStage.FRAGMENT, buffer=dict(type="uniform", min_binding_size=48)),
                dict(
                    binding=1,
                    visibility=wgpu.ShaderStage.FRAGMENT,
                    texture=dict(sample_type="unfilterable-float", view_dimension="2d"),
                ),
            ]
        )
        self.production_pipeline = device.create_render_pipeline(
            layout=device.create_pipeline_layout(bind_group_layouts=[self.production_layout]),
            vertex=dict(module=production_module, entry_point="vs_main"),
            fragment=dict(module=production_module, entry_point="fs_decal_depth", targets=[dict(format="r32float")]),
            primitive=dict(topology="triangle-list"),
        )
        self.candidate_pipeline = device.create_render_pipeline(
            layout=device.create_pipeline_layout(bind_group_layouts=[self.candidate_layout]),
            vertex=dict(module=candidate_module, entry_point="vs_main"),
            fragment=dict(module=candidate_module, entry_point="fs_canonical_depth", targets=[dict(format="r32float")]),
            primitive=dict(topology="triangle-list"),
        )
        self.adapter_pipeline = device.create_render_pipeline(
            layout=device.create_pipeline_layout(bind_group_layouts=[self.adapter_layout]),
            vertex=dict(module=adapter_module, entry_point="vs_main"),
            fragment=dict(module=adapter_module, entry_point="fs_canonical_depth", targets=[dict(format="r32float")]),
            primitive=dict(topology="triangle-list"),
        )
        seed_code = """
@group(0) @binding(0) var raw: texture_2d<f32>;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
    let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
    return vec4f(p[i],0,1);
}
@fragment fn fs(@builtin(position) pixel:vec4f, @builtin(sample_index) sample:u32)
    -> @builtin(frag_depth) f32 {
    return clamp(textureLoad(raw, vec2i(pixel.xy), 0).r + f32(sample) * 0.0001, 0.0, 1.0);
}
"""
        seed_module = device.create_shader_module(code=seed_code)
        self.seed_layout = device.create_bind_group_layout(
            entries=[
                dict(
                    binding=0,
                    visibility=wgpu.ShaderStage.FRAGMENT,
                    texture=dict(sample_type="unfilterable-float", view_dimension="2d"),
                )
            ]
        )
        self.seed_pipeline = device.create_render_pipeline(
            layout=device.create_pipeline_layout(bind_group_layouts=[self.seed_layout]),
            vertex=dict(module=seed_module, entry_point="vs"),
            fragment=dict(module=seed_module, entry_point="fs", targets=[]),
            primitive=dict(topology="triangle-list"),
            depth_stencil=dict(format="depth32float", depth_write_enabled=True, depth_compare="always"),
            multisample=dict(count=self.depth_samples),
        )

    @staticmethod
    def _texture(device, width, height, format, usage, sample_count=1):
        return device.create_texture(
            size=(width, height, 1),
            format=format,
            usage=usage,
            sample_count=sample_count,
        )

    def _write(self, texture, values, width: int, height: int):
        values = np.ascontiguousarray(values)
        channels = values.shape[-1] if values.ndim == 3 else 1
        self.device.queue.write_texture(
            {"texture": texture},
            values,
            {
                "bytes_per_row": width * values.dtype.itemsize * channels,
                "rows_per_image": height,
            },
            (width, height, 1),
        )

    def _read(self, texture, width: int, height: int, dtype, channels: int = 1):
        raw = self.device.queue.read_texture(
            {"texture": texture},
            {
                "bytes_per_row": width * np.dtype(dtype).itemsize * channels,
                "rows_per_image": height,
            },
            (width, height, 1),
        )
        shape = (height, width, channels) if channels != 1 else (height, width)
        return np.frombuffer(raw, dtype=dtype).reshape(shape).copy()

    def _allocate_case(
        self,
        view_z,
        *,
        viewport,
        scissor,
        depth_range,
        reversed_z,
        decal_before,
        decal_after,
        foliage,
    ) -> _CaseResources:
        height, width = view_z.shape
        raw = _raw_depth(view_z, depth_range, reversed_z)
        before_raw = _raw_depth(decal_before, depth_range, reversed_z)
        after_raw = _raw_depth(decal_after, depth_range, reversed_z)
        sample_usage = wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST
        output_usage = wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC
        depth_attachment = self._texture(
            self.device,
            width,
            height,
            "depth32float",
            wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING,
            sample_count=self.depth_samples,
        )
        raw_texture = self._texture(self.device, width, height, "r32float", sample_usage)
        before_texture = self._texture(self.device, width, height, "r32float", sample_usage)
        after_texture = self._texture(self.device, width, height, "r32float", sample_usage)
        foliage_height, foliage_width, foliage_channels = foliage.shape
        if foliage_channels != 4:
            raise ValueError("foliage must be HxWx4 RGBA32F")
        foliage_texture = self._texture(
            self.device,
            foliage_width,
            foliage_height,
            self.mask_format,
            sample_usage,
        )
        baseline_prepared = self._texture(
            self.device, width, height, "r32float", output_usage | wgpu.TextureUsage.TEXTURE_BINDING
        )
        baseline_canonical = self._texture(self.device, width, height, "r32float", output_usage)
        candidate_canonical = self._texture(self.device, width, height, "r32float", output_usage)
        self._write(raw_texture, raw, width, height)
        self._write(before_texture, before_raw, width, height)
        self._write(after_texture, after_raw, width, height)
        # Native foreground coverage uses the window's 8-bit UNORM color
        # format (and RGBA8 for its 1x1 empty fallback). Do not benchmark a
        # four-times-larger float mask as if it were the native allocation.
        mask_pixels = foliage if self.mask_format == "rgba32float" else (
            np.rint(np.clip(foliage, 0.0, 1.0) * 255.0).astype(np.uint8)
        )
        self._write(foliage_texture, mask_pixels, foliage_width, foliage_height)

        production_uniform = self.device.create_buffer_with_data(
            data=pack_production_params(width, height, viewport, scissor, depth_range, reversed_z),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        adapter_uniform = self.device.create_buffer_with_data(
            data=pack_adapter_params(adapter_depth_constants(), viewport, scissor),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        seed_group = self.device.create_bind_group(
            layout=self.seed_layout,
            entries=[dict(binding=0, resource=raw_texture.create_view())],
        )
        production_entries = [
            dict(binding=0, resource=dict(buffer=production_uniform, size=80)),
            dict(binding=1, resource=depth_attachment.create_view()),
            dict(binding=2, resource=foliage_texture.create_view()),
            dict(binding=3, resource=before_texture.create_view()),
            dict(binding=4, resource=after_texture.create_view()),
            dict(binding=5, resource=foliage_texture.create_view()),
        ]
        baseline_prepare_group = self.device.create_bind_group(
            layout=self.production_layout,
            entries=production_entries,
        )
        candidate_group = self.device.create_bind_group(
            layout=self.candidate_layout,
            entries=[*production_entries, dict(binding=7, resource=dict(buffer=adapter_uniform, size=48))],
        )
        baseline_adapter_group = self.device.create_bind_group(
            layout=self.adapter_layout,
            entries=[
                dict(binding=0, resource=dict(buffer=adapter_uniform, size=48)),
                dict(binding=1, resource=baseline_prepared.create_view()),
            ],
        )
        return _CaseResources(
            width,
            height,
            depth_attachment,
            baseline_prepared,
            baseline_canonical,
            candidate_canonical,
            baseline_prepare_group,
            baseline_adapter_group,
            candidate_group,
            (
                raw_texture,
                before_texture,
                after_texture,
                foliage_texture,
                production_uniform,
                adapter_uniform,
                seed_group,
            ),
        )

    @staticmethod
    def _timestamps(query, index):
        if query is None:
            return {}
        return {
            "timestamp_writes": {
                "query_set": query,
                "beginning_of_pass_write_index": index,
                "end_of_pass_write_index": index + 1,
            }
        }

    def _encode_seed(self, encoder, resources: _CaseResources):
        raw_view = resources.keep_alive[0].create_view()
        render = encoder.begin_render_pass(
            label="seed depth attachment",
            color_attachments=[],
            depth_stencil_attachment=dict(
                view=resources.depth_attachment.create_view(),
                depth_load_op="clear",
                depth_store_op="store",
                depth_clear_value=0.0,
            ),
        )
        render.set_pipeline(self.seed_pipeline)
        render.set_bind_group(0, resources.keep_alive[-1])
        render.draw(3)
        render.end()

    def _encode_baseline(self, encoder, resources, query=None, index=0):
        render = encoder.begin_render_pass(
            label="baseline prepared view-z",
            **self._timestamps(query, index),
            color_attachments=[
                dict(
                    view=resources.baseline_prepared.create_view(),
                    clear_value=(0, 0, 0, 0),
                    load_op="clear",
                    store_op="store",
                )
            ],
        )
        render.set_pipeline(self.production_pipeline)
        render.set_bind_group(0, resources.baseline_prepare_group)
        render.draw(3)
        render.end()
        index += 2 if query is not None else 0
        render = encoder.begin_render_pass(
            label="baseline canonical depth",
            **self._timestamps(query, index),
            color_attachments=[
                dict(
                    view=resources.baseline_canonical.create_view(),
                    clear_value=(1, 0, 0, 0),
                    load_op="clear",
                    store_op="store",
                )
            ],
        )
        render.set_pipeline(self.adapter_pipeline)
        render.set_bind_group(0, resources.baseline_adapter_group)
        render.draw(3)
        render.end()
        return index + (2 if query is not None else 0)

    def _encode_candidate(self, encoder, resources, query=None, index=0):
        render = encoder.begin_render_pass(
            label="candidate prepared canonical depth",
            **self._timestamps(query, index),
            color_attachments=[
                dict(
                    view=resources.candidate_canonical.create_view(),
                    clear_value=(1, 0, 0, 0),
                    load_op="clear",
                    store_op="store",
                )
            ],
        )
        render.set_pipeline(self.candidate_pipeline)
        render.set_bind_group(0, resources.candidate_group)
        render.draw(3)
        render.end()
        return index + (2 if query is not None else 0)

    def compare(
        self,
        view_z,
        *,
        viewport=(0.0, 0.0, 1.0, 1.0),
        scissor=(0.0, 0.0, 1.0, 1.0),
        depth_range=(0.0, 1.0),
        reversed_z=True,
        decal_before=None,
        decal_after=None,
        foliage=None,
    ) -> CanonicalParity:
        view_z = np.ascontiguousarray(view_z, dtype=np.float32)
        if view_z.ndim != 2:
            raise ValueError("view_z must be a 2D HxW array")
        height, width = view_z.shape
        if decal_before is None:
            decal_before = view_z
        if decal_after is None:
            decal_after = view_z
        decal_before = np.ascontiguousarray(decal_before, dtype=np.float32)
        decal_after = np.ascontiguousarray(decal_after, dtype=np.float32)
        if decal_before.shape != view_z.shape or decal_after.shape != view_z.shape:
            raise ValueError("decal depth arrays must match view_z shape")
        if foliage is None:
            foliage = _transparent_foliage(width, height)
        foliage = np.ascontiguousarray(foliage, dtype=np.float32)
        if foliage.ndim != 3 or foliage.shape[-1] != 4:
            raise ValueError("foliage must be HxWx4")
        resources = self._allocate_case(
            view_z,
            viewport=viewport,
            scissor=scissor,
            depth_range=depth_range,
            reversed_z=reversed_z,
            decal_before=decal_before,
            decal_after=decal_after,
            foliage=foliage,
        )
        encoder = self.device.create_command_encoder(label="prepared canonical parity")
        self._encode_seed(encoder, resources)
        self._encode_baseline(encoder, resources)
        self._encode_candidate(encoder, resources)
        self.device.queue.submit([encoder.finish()])
        try:
            # queue.read_texture is synchronous here; only after all three
            # readbacks have completed is it safe to release case resources.
            result = CanonicalParity(
                baseline_prepared=self._read(resources.baseline_prepared, width, height, np.float32),
                baseline_canonical=self._read(resources.baseline_canonical, width, height, np.float32),
                candidate_canonical=self._read(resources.candidate_canonical, width, height, np.float32),
            )
        finally:
            resources.release()
        return result

    def timing_size(self, width: int, height: int) -> dict:
        if "timestamp-query" not in self.adapter.features:
            raise GpuUnavailable("adapter does not support timestamp-query")
        if np is None:
            raise GpuUnavailable("NumPy is required for timing")
        # Use the same deterministic contact-shoe fixture as the existing AO
        # GPU QA.  This explicit import avoids silently timing a fallback scene.
        import test_ao_gpu as upstream

        scene, _ = upstream.feet_scene(width, height)
        fixture_hash = _test_ao_gpu_fixture_hash()
        resources = self._allocate_case(
            np.ascontiguousarray(scene, dtype=np.float32),
            viewport=(0.0, 0.0, 1.0, 1.0),
            scissor=(0.0, 0.0, 1.0, 1.0),
            depth_range=(0.0, 1.0),
            reversed_z=True,
            decal_before=np.ascontiguousarray(scene, dtype=np.float32),
            decal_after=np.ascontiguousarray(scene, dtype=np.float32),
            foliage=_transparent_foliage(width, height),
        )
        # Seed and all uploads happen before warmup/timestamped work.
        seed_encoder = self.device.create_command_encoder(label="timing depth seed")
        self._encode_seed(seed_encoder, resources)
        self.device.queue.submit([seed_encoder.finish()])
        warmup = self.device.create_command_encoder(label="prepared canonical warmup")
        for _ in range(10):
            for condition in ("baseline", "candidate", "candidate", "baseline"):
                if condition == "baseline":
                    self._encode_baseline(warmup, resources)
                else:
                    self._encode_candidate(warmup, resources)
        self.device.queue.submit([warmup.finish()])

        samples_per_condition = 120
        blocks = samples_per_condition // 2
        query = self.device.create_query_set(type="timestamp", count=samples_per_condition * 4 + samples_per_condition * 2)
        measured = self.device.create_command_encoder(label="prepared canonical ABBA timestamps")
        records = []
        index = 0
        for _ in range(blocks):
            for condition in ("baseline", "candidate", "candidate", "baseline"):
                start = index
                if condition == "baseline":
                    index = self._encode_baseline(measured, resources, query, index)
                else:
                    index = self._encode_candidate(measured, resources, query, index)
                records.append((condition, start))
        if index != samples_per_condition * 6:
            raise AssertionError(f"timestamp allocation mismatch: {index}")
        resolved = self.device.create_buffer(
            size=index * 8,
            usage=wgpu.BufferUsage.QUERY_RESOLVE | wgpu.BufferUsage.COPY_SRC,
        )
        measured.resolve_query_set(query, 0, index, resolved, 0)
        self.device.queue.submit([measured.finish()])
        try:
            # read_buffer synchronizes completion of the measured submission;
            # all per-case textures and buffers can now be released.
            timestamps = np.frombuffer(self.device.queue.read_buffer(resolved), dtype=np.uint64)
        finally:
            resources.release()
            query.destroy()
            resolved.destroy()
            query = None
            resolved = None
            seed_encoder = None
            warmup = None
            measured = None
        stage_sum = {"baseline": [], "candidate": []}
        first_last = {"baseline": [], "candidate": []}
        for condition, start in records:
            first = int(timestamps[start])
            second = int(timestamps[start + 1])
            if condition == "baseline":
                third = int(timestamps[start + 2])
                fourth = int(timestamps[start + 3])
                if second < first or fourth < third or fourth < first:
                    raise AssertionError("non-monotonic baseline timestamp interval")
                stage_sum[condition].append((second - first + fourth - third) / 1e6)
                first_last[condition].append((fourth - first) / 1e6)
            else:
                if second < first:
                    raise AssertionError("non-monotonic candidate timestamp interval")
                stage_sum[condition].append((second - first) / 1e6)
                first_last[condition].append((second - first) / 1e6)

        def distribution(values):
            values = np.asarray(values, dtype=np.float64)
            return {
                "count": int(values.size),
                "min": float(values.min()),
                "median": float(np.percentile(values, 50)),
                "p95": float(np.percentile(values, 95)),
                "max": float(values.max()),
                "mean": float(values.mean()),
            }

        return {
            "size": [width, height],
            "config": {
                "compatibility": self.compatibility,
                "depth_samples": self.depth_samples,
                "decals": self.decals,
                "mask_format": self.mask_format,
                "fixture": "test_ao_gpu.feet_scene, full-size transparent mask",
            },
            "warmup_samples_per_condition": 20,
            "measured_samples_per_condition": samples_per_condition,
            "order": "ABBA (baseline preparation+canonical, candidate, candidate, baseline), repeated 60 times",
            "gpu_timestamp_ms": {
                "baseline_stage_sum": distribution(stage_sum["baseline"]),
                "baseline_first_to_last_span": distribution(first_last["baseline"]),
                "candidate_stage_sum": distribution(stage_sum["candidate"]),
                "candidate_first_to_last_span": distribution(first_last["candidate"]),
            },
            "source_sha256": {
                "production_prepared": source_hash(self.production),
                "adapter_canonical": source_hash(self.adapter_source),
                "candidate_prepared_canonical": source_hash(self.candidate),
                "test_ao_gpu_fixture": fixture_hash,
            },
        }


def _jsonable(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, dict):
        return {str(key): _jsonable(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_jsonable(item) for item in value]
    return str(value)


def _validate_output_path(path: Path) -> Path:
    build = (ROOT / "build").resolve()
    target = path if path.is_absolute() else (Path.cwd() / path)
    target = target.resolve()
    try:
        target.relative_to(build)
    except ValueError as exc:
        raise ValueError(f"timing report must be a new path inside {build}") from exc
    if target == build or target.suffix.lower() != ".json":
        raise ValueError("timing report output must be a .json file inside build")
    if target.exists():
        raise ValueError(f"timing report path already exists; choose a new path: {target}")
    return target


def run_timing(*, output: Path, include_msaa4_decals: bool = False, backend: str | None = None) -> dict:
    if not HAS_GPU_DEPS:
        raise GpuUnavailable("numpy and wgpu==0.32.0 are required")
    target = _validate_output_path(output)
    if backend:
        os.environ["WGPU_BACKEND_TYPE"] = backend
    probe = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
    if probe is None:
        raise GpuUnavailable("wgpu could not find a high-performance adapter")
    if "timestamp-query" not in probe.features:
        raise GpuUnavailable("adapter does not support timestamp-query")
    entries = []
    fixture_hash = _test_ao_gpu_fixture_hash()
    modes = [(1, False)]
    if include_msaa4_decals:
        modes.append((4, True))
    for depth_samples, decals in modes:
        renderer = PreparedCanonicalRenderer(
            compatibility=False,
            depth_samples=depth_samples,
            decals=decals,
            timing=True,
        )
        for width, height in ((848, 480), (1280, 720), (2340, 1080), (2560, 1440)):
            entries.append(renderer.timing_size(width, height))
    report = {
        "schema": "xegtao-prepared-canonical-timing-v1",
        "backend_requested": backend,
        "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "fixture_sha256": fixture_hash,
        "device": _jsonable(dict(probe.info)),
        "measurement_scope": {
            "included": "Resident production depth preparation plus canonical R32 raster only",
            "excluded": [
                "raw-depth generation and all uniform/texture uploads",
                "GPU readbacks and report serialization",
                "AO generation, denoise, composite, and all other XeGTAO stages",
                "MP6/native frame timing, Android, and game integration",
            ],
        },
        "configs": entries,
    }
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        # Exclusive creation closes the check-then-write race and guarantees
        # that an existing report is never silently overwritten.
        with target.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except FileExistsError as exc:
        raise ValueError(f"timing report path was created concurrently: {target}") from exc
    return report


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class PreparedCanonicalGPU(unittest.TestCase):
    """Focused exact-buffer parity checks; no full AO/composite claims."""

    @classmethod
    def setUpClass(cls):
        adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if adapter is None:
            raise unittest.SkipTest("wgpu could not find a high-performance adapter")
        cls._renderers = {}

    @classmethod
    def renderer(cls, compatibility=False, depth_samples=1, decals=False):
        key = (bool(compatibility), int(depth_samples), bool(decals))
        if key not in cls._renderers:
            cls._renderers[key] = PreparedCanonicalRenderer(
                compatibility=key[0], depth_samples=key[1], decals=key[2]
            )
        return cls._renderers[key]

    def assert_parity(self, values, **kwargs):
        result = self.renderer(**kwargs.pop("renderer", {})).compare(values, **kwargs)
        self.assertTrue(np.isfinite(result.baseline_canonical).all())
        np.testing.assert_array_equal(
            result.candidate_canonical.view(np.uint32),
            result.baseline_canonical.view(np.uint32),
            err_msg=(
                "candidate full-resolution canonical depth differs from baseline; "
                f"{bit_diagnostics(result.baseline_canonical, result.candidate_canonical)}"
            ),
        )
        return result

    def test_clear_depth_boundaries_and_odd_resolution(self):
        values = np.array(
            [
                [0.0, -0.0, -1.0, 19.99, 20.0, 20.01, 400.0, 9999.0, 10000.0, 20000.0],
                [np.nan, -np.inf, np.inf, 1e-30, 1.0, 45.0, 400.0, 1800.0, 6000.0, 0.0],
            ],
            dtype=np.float32,
        )
        self.assert_parity(values)
        y, x = np.mgrid[:65, :129]
        odd = (80.0 + x * 11.0 + y * 17.0).astype(np.float32)
        odd[(x + 3 * y) % 13 == 0] = 0.0
        self.assert_parity(odd)

    def test_contact_shoes_plane_and_discontinuities(self):
        import test_ao_gpu as upstream

        for width, height in ((33, 19), (129, 65)):
            plane, _ = upstream.plane(width, height)
            contact, _ = upstream.feet_scene(width, height)
            step = np.full((height, width), 400.0, dtype=np.float32)
            step[:, width // 2 :] = 1800.0
            step[height // 3 : 2 * height // 3, width // 3 : 2 * width // 3] = 90.0
            step[::3, ::5] = 0.0
            for name, scene in (("plane", plane), ("contact-shoes", contact), ("discontinuity", step)):
                with self.subTest(size=(width, height), scene=name):
                    self.assert_parity(scene)

    def test_reversed_and_forward_restricted_depth_range(self):
        y, x = np.mgrid[:19, :33]
        scene = (25.0 + x * 37.0 + y * 19.0).astype(np.float32)
        scene[::4, ::5] = 0.0
        for reversed_z in (False, True):
            with self.subTest(reversed_z=reversed_z):
                self.assert_parity(
                    scene,
                    depth_range=(0.1, 0.8),
                    reversed_z=reversed_z,
                )

    def test_viewport_scissor_crops_and_edges(self):
        width, height = 33, 19
        y, x = np.mgrid[:height, :width]
        scene = (60.0 + x * 17.0 + y * 31.0).astype(np.float32)
        scene[::4, ::6] = 0.0
        cases = (
            ((0.0, 0.0, 1.0, 1.0), (0.0, 0.0, 1.0, 1.0)),
            ((5 / width, 3 / height, 19 / width, 11 / height), (8 / width, 2 / height, 18 / width, 13 / height)),
            ((-0.4, -0.2, 1.5, 1.4), (2 / width, 1 / height, 5 / width, 3 / height)),
            ((0.0, 0.0, 1.0, 1.0), (27 / width, 14 / height, 6 / width, 5 / height)),
        )
        for viewport, scissor in cases:
            with self.subTest(viewport=viewport, scissor=scissor):
                self.assert_parity(scene, viewport=viewport, scissor=scissor)

    def test_decals_later_actor_occlusion_and_water_masks(self):
        width, height = 33, 19
        base = np.full((height, width), 500.0, dtype=np.float32)
        before = base.copy()
        after = base.copy()
        after[5:14, 8:25] = 180.0
        # A later actor changes the post-decal depth: this region must not be
        # erased by the ordered decal selection.
        current = after.copy()
        current[8:11, 14:19] = 90.0
        foliage = _transparent_foliage(width, height)
        foliage[2:6, 3:8, 3] = 1.0
        foliage[12:15, 20:26, 3] = 1.0 / 255.0
        result = self.assert_parity(
            current,
            decal_before=before,
            decal_after=after,
            foliage=foliage,
            renderer={"depth_samples": 4, "decals": True},
        )
        self.assertTrue(np.all(result.baseline_canonical[foliage[..., 3] > 0] == np.float32(1.0)))
        # The actor exists only in the current post-decal depth; production's
        # equality guard therefore retains it instead of restoring the decal's
        # before-depth.  Keep this semantic assertion independent of candidate
        # parity and check both selected values below.
        selection = (current == after) & (before != after)
        self.assertTrue(np.all(selection[5:8, 8:12]))
        self.assertTrue(np.all(~selection[8:11, 14:19]))
        np.testing.assert_allclose(
            result.baseline_prepared[8:11, 14:19],
            _expected_prepared_viewz(np.full((3, 5), 90.0, dtype=np.float32), (0.0, 1.0), True),
            rtol=2e-5,
            atol=2e-4,
        )
        np.testing.assert_allclose(
            result.baseline_prepared[5:8, 8:12],
            _expected_prepared_viewz(np.full((3, 4), 500.0, dtype=np.float32), (0.0, 1.0), True),
            rtol=2e-5,
            atol=2e-4,
        )

    def test_rgb_foliage_without_alpha_does_not_change_depth(self):
        width, height = 17, 11
        scene = np.full((height, width), 400.0, dtype=np.float32)
        transparent_rgb = _transparent_foliage(width, height)
        transparent_rgb[..., :3] = (0.15, 0.35, 0.8)
        renderer = self.renderer(depth_samples=1, decals=False)
        plain = renderer.compare(scene)
        colored = renderer.compare(scene, foliage=transparent_rgb)
        np.testing.assert_array_equal(
            plain.baseline_prepared.view(np.uint32), colored.baseline_prepared.view(np.uint32)
        )
        np.testing.assert_array_equal(
            plain.candidate_canonical.view(np.uint32), colored.candidate_canonical.view(np.uint32)
        )

    def test_compatibility_and_depth_sample_matrix(self):
        scene = np.full((11, 17), 400.0, dtype=np.float32)
        scene[::3, ::4] = 0.0
        for compatibility in (False, True):
            for depth_samples in (1, 4):
                for decals in (False, True):
                    with self.subTest(
                        compatibility=compatibility,
                        depth_samples=depth_samples,
                        decals=decals,
                    ):
                        self.assert_parity(
                            scene,
                            renderer={
                                "compatibility": compatibility,
                                "depth_samples": depth_samples,
                                "decals": decals,
                            },
                        )

    def test_combined_adversarial_fixture_all_modes_and_water_shapes(self):
        """Bounded 48-case semantic/parity matrix (8 configs x 2 Z x 3 masks)."""

        width, height = 129, 65
        y, x = np.mgrid[:height, :width]
        ground = (320.0 + x * 1.7 + y * 2.1).astype(np.float32)
        ground[:, width // 2 :] += 900.0
        ground[::7, ::9] = 0.0
        before = ground.copy()
        after = before.copy()
        decal = np.zeros_like(ground, dtype=bool)
        decal[20:46, 28:98] = True
        after[decal] = 180.0
        actor = np.zeros_like(ground, dtype=bool)
        actor[30:38, 60:72] = True
        current = after.copy()
        current[actor] = 90.0
        viewport = (0.07, 0.11, 0.83, 0.75)
        scissor = (0.13, 0.17, 0.72, 0.60)
        for water_shape in ("full", "one-by-one", "one-by-one-transparent"):
            if water_shape == "full":
                foliage = _transparent_foliage(width, height)
                water = np.zeros((height, width), dtype=bool)
                water[5:12, 8:25] = True
                water[48:54, 92:116] = True
                foliage[..., 3] = np.where(water, np.float32(1.0), np.float32(0.0))
                foliage[48:54, 92:116, 3] = np.float32(1.0 / 255.0)
            elif water_shape == "one-by-one":
                foliage = np.zeros((1, 1, 4), dtype=np.float32)
                water = np.ones((height, width), dtype=bool)
                foliage[0, 0, 3] = 1.0
            elif water_shape == "one-by-one-transparent":
                # A transparent 1x1 fallback mask must not turn every pixel
                # into water; this is the no-water companion to the 1x1 case.
                foliage = np.zeros((1, 1, 4), dtype=np.float32)
                water = np.zeros((height, width), dtype=bool)
            for compatibility in (False, True):
                for depth_samples in (1, 4):
                    for decals in (False, True):
                        for reversed_z in (False, True):
                            with self.subTest(
                                water_shape=water_shape,
                                compatibility=compatibility,
                                depth_samples=depth_samples,
                                decals=decals,
                                reversed_z=reversed_z,
                            ):
                                result = self.assert_parity(
                                    current,
                                    viewport=viewport,
                                    scissor=scissor,
                                    depth_range=(0.1, 0.8),
                                    reversed_z=reversed_z,
                                    decal_before=before,
                                    decal_after=after,
                                    foliage=foliage,
                                    renderer={
                                        "compatibility": compatibility,
                                        "depth_samples": depth_samples,
                                        "decals": decals,
                                    },
                                )
                                self.assertTrue(
                                    np.all(result.baseline_canonical[water] == np.float32(1.0)),
                                    "water mask must force canonical depth to clear",
                                )
                                if water_shape == "one-by-one-transparent":
                                    self.assertTrue(
                                        np.any(result.baseline_canonical < np.float32(1.0)),
                                        "transparent 1x1 fallback mask must not clear all canonical depth",
                                    )
                                if water_shape == "full":
                                    semantic = current.copy()
                                    if decals:
                                        semantic = np.where(
                                            (current == after) & (before != after), before, current
                                        )
                                    expected = _expected_prepared_viewz(semantic, (0.1, 0.8), reversed_z)
                                    check = (~water) & (~actor) & decal
                                    np.testing.assert_allclose(
                                        result.baseline_prepared[check],
                                        expected[check],
                                        rtol=3e-5,
                                        atol=3e-3,
                                    )
                                    actor_expected = _expected_prepared_viewz(
                                        np.full((1,), 90.0, dtype=np.float32), (0.1, 0.8), reversed_z
                                    )[0]
                                    np.testing.assert_allclose(
                                        result.baseline_prepared[actor],
                                        actor_expected,
                                        rtol=3e-5,
                                        atol=3e-3,
                                    )


def _run_timing(parsed) -> int:
    try:
        target = _validate_output_path(parsed.output)
        report = run_timing(
            output=target,
            include_msaa4_decals=parsed.include_msaa4_decals,
            backend=parsed.backend,
        )
    except GpuUnavailable as exc:
        print(f"GPU timing unavailable: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"output": str(target), "configs": len(report["configs"])}, sort_keys=True))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("Vulkan", "D3D12"))
    parser.add_argument("--timing-experiment", action="store_true")
    parser.add_argument(
        "--include-msaa4-decals",
        action="store_true",
        help="also time the optional MSAA4 + decal configuration at all four sizes",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "build" / "xegtao-prepared-canonical-timing.json",
        help="timing report path; must be a .json path inside build/",
    )
    parsed, unittest_args = parser.parse_known_args(argv)
    if parsed.backend:
        # Set this before the first adapter request; import remains GPU-free.
        os.environ["WGPU_BACKEND_TYPE"] = parsed.backend
    if parsed.timing_experiment:
        return _run_timing(parsed)
    result = unittest.main(module=__name__, argv=[sys.argv[0], *unittest_args], exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":  # pragma: no cover - direct integration entry point
    raise SystemExit(main())
