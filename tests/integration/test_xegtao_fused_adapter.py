"""Test-only experiment: fuse canonical depth and upstream normal generation.

The baseline executes the actual adapter raster shader, copies its R32F
output without conversion, and executes the unchanged generated normal
shader. The candidate replaces only that shader's five consumed depth
fetches with canonical clean-depth fetches and also writes canonical depth.
Normal reconstruction and packing remain generated upstream code.

No production, vendored, or generated shader files are modified. This is an
equivalence experiment, not evidence of a performance improvement. Run with::

    python tests/integration/test_xegtao_fused_adapter.py --backend Vulkan -v

An explicit ``--timing-experiment`` instead runs a bounded, synthetic GPU
timestamp experiment. It is not part of normal test discovery and makes no
native-frame, full-AO, or Android performance claim.

The optional dependencies and shader generation are the same as
test_xegtao_gpu.py. Importing this module never requests a GPU.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import json
import os
from pathlib import Path
import re
import sys
from types import SimpleNamespace
import unittest


INTEGRATION = Path(__file__).resolve().parent
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

import test_xegtao_adapter as adapter_harness
import test_xegtao_gpu as upstream


np = upstream.np
wgpu = upstream.wgpu
SHADER_DIR = upstream.DEFAULT_SHADER_DIR
HAS_GPU_DEPS = np is not None and wgpu is not None


def _replace_once(source: str, old: str, new: str) -> str:
    """Fail closed if generation changed the bounded substitution sites."""
    count = source.count(old)
    if count != 1:
        raise upstream.ShaderInterfaceError(
            f"fused experiment expected one substitution site, found {count}: {old!r}"
        )
    return source.replace(old, new, 1)


def _function(source: str, name: str) -> str:
    match = re.search(rf"\bfn\s+{re.escape(name)}\s*\(", source)
    if match is None:
        raise upstream.ShaderInterfaceError(f"missing shader function {name}")
    opening = source.index("{", match.start())
    depth = 1
    for index in range(opening + 1, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[match.start() : index + 1]
    raise upstream.ShaderInterfaceError(f"unclosed shader function {name}")


def _normal_info(shader_dir: Path = SHADER_DIR):
    spec = upstream.NORMALS
    return upstream.parse_shader_module(
        shader_dir / f"{spec.name}.wgsl", spec.entry, set(spec.expected), set(spec.required)
    )


def _fused_info(baseline):
    """Derive an in-memory candidate; preserve generated arithmetic verbatim."""
    adapter = adapter_harness._adapter_source()
    params = re.search(r"struct AdapterParams\s*\{.*?\};", adapter, re.DOTALL)
    if params is None:
        raise upstream.ShaderInterfaceError("adapter has no AdapterParams declaration")
    # These two function bodies come directly from the production adapter.
    receiver = _function(adapter, "receiver")
    canonical = _function(adapter, "fs_canonical_depth")
    canonical_body = canonical[canonical.index("{") + 1 : -1]
    additions = "\n" + params.group(0) + """
@group(0) @binding(2) var<uniform> p: AdapterParams;
@group(3) @binding(2) var fusedRawDepth: texture_storage_2d<r32float, write>;
""" + receiver + """

fn canonicalDepth(inputCoord: vec2<i32>) -> f32 {
    // Point-clamp the integer coordinate BEFORE testing receiver coverage.
    // A gather at the image edge repeats the edge texel, including its mask.
    let at = clamp(inputCoord, vec2i(0), vec2i(textureDimensions(cleanDepth)) - vec2i(1));
    let pixel = vec4f(vec2f(at) + vec2f(0.5), 0, 1);
""" + canonical_body + "\n}\n"

    code = baseline.code
    code = _replace_once(
        code,
        "@binding(0) @group(3) var g_srcRawDepthNormals_0 : texture_2d<f32>;",
        "@binding(0) @group(3) var cleanDepth : texture_2d<f32>;",
    )
    code = _replace_once(
        code, "@binding(1) @group(0) var g_samplerPointClamp_0 : sampler;", ""
    )
    code = _replace_once(
        code,
        ",  sourceNDCDepth_0 : texture_2d<f32>,  depthSampler_0 : sampler",
        "",
    )
    code = _replace_once(
        code,
        "var _S13 : vec4<f32> = (textureGather((i32(0)), (sourceNDCDepth_0), (depthSampler_0), (_S12)));",
        """var _S13 : vec4<f32> = vec4<f32>(
        canonicalDepth(vec2i(pixCoord_0) + vec2i(-1, 0)),
        canonicalDepth(vec2i(pixCoord_0)),
        canonicalDepth(vec2i(pixCoord_0) + vec2i(0, -1)), 0.0f);""",
    )
    code = _replace_once(
        code,
        "var _S14 : vec4<f32> = (textureGather((i32(0)), (sourceNDCDepth_0), (depthSampler_0), (_S12), (vec2<i32>(i32(1), i32(1)))));",
        """var _S14 : vec4<f32> = vec4<f32>(
        canonicalDepth(vec2i(pixCoord_0) + vec2i(0, 1)), 0.0f,
        canonicalDepth(vec2i(pixCoord_0) + vec2i(1, 0)), 0.0f);
    textureStore(fusedRawDepth, pixCoord_0, vec4<f32>(_S13.y, 0, 0, 1));""",
    )
    code = _replace_once(
        code, ", g_srcRawDepthNormals_0, g_samplerPointClamp_0);", ");"
    )
    code = _replace_once(code, "fn CSGenerateNormals(", "fn CSCanonicalDepthAndNormals(")
    code = _replace_once(
        code,
        "    var pixCoord_2 : vec2<u32> = pixCoord_1.xy;",
        """    if (any(pixCoord_1.xy >= textureDimensions(cleanDepth))) { return; }
    var pixCoord_2 : vec2<u32> = pixCoord_1.xy;""",
    )
    # The original banner must not suggest the derived experiment is generated
    # or is a replacement for the generated baseline on disk.
    code = "// In-memory TEST-ONLY canonical-depth/normal fusion experiment.\n" + additions + code
    resources = {
        key: resource for key, resource in baseline.resources.items()
        if key not in ("g_srcRawDepthNormals", "g_samplerPointClamp")
    }
    for resource in (
        upstream.ResourceDecl("cleanDepth", "cleanDepth", 3, 0, "sampled-f32"),
        upstream.ResourceDecl("p", "p", 0, 2, "uniform"),
        upstream.ResourceDecl("fusedRawDepth", "fusedRawDepth", 3, 2, "storage", "r32float", "write"),
    ):
        resources[resource.key] = resource
    return replace(
        baseline, name="CSCanonicalDepthAndNormals", code=code,
        entry_point="CSCanonicalDepthAndNormals", resources=resources,
    )


class _NormalStages(upstream.Renderer):
    """Reuse the upstream layout/bind/dispatch helpers on the raster's device."""

    def __init__(self, raster, shader_dir):
        self.raster = raster
        self.device = raster.device
        self.baseline = _normal_info(shader_dir)
        self.fused = _fused_info(self.baseline)
        self.sampler = self.device.create_sampler(
            address_mode_u="clamp-to-edge", address_mode_v="clamp-to-edge",
            address_mode_w="clamp-to-edge", mag_filter="nearest",
            min_filter="nearest", mipmap_filter="nearest",
        )
        self.baseline_pipeline = self._create_pipeline(self.baseline)
        self.fused_pipeline = self._create_pipeline(self.fused)

    def _layout_entry(self, resource):
        entry = super()._layout_entry(resource)
        if resource.key == "p":
            entry["buffer"]["min_binding_size"] = 48
        return entry

    def compare(self, values, viewport=(0, 0, 1, 1), scissor=(0, 0, 1, 1)):
        clean = np.ascontiguousarray(values, dtype=np.float32)
        height, width = clean.shape
        size = (width, height)
        depth = (20.0 * 10000.0 / (10000.0 - 20.0), 10000.0 / (10000.0 - 20.0), 0.4, 0.0)
        raster_depth = self.raster._render(
            self.raster.depth_pipeline, self.raster.depth_layout,
            width, height, (depth, viewport, scissor), clean,
        )
        sampled_usage = wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST
        result_usage = wgpu.TextureUsage.STORAGE_BINDING | wgpu.TextureUsage.COPY_SRC
        baseline_depth = self._texture(size, "r32float", sampled_usage)
        baseline_normals = self._texture(size, "r32uint", result_usage)
        fused_depth = self._texture(size, "r32float", result_usage)
        fused_normals = self._texture(size, "r32uint", result_usage)
        clean_texture = self._texture(size, "r32float", sampled_usage)
        self._write(clean_texture, clean, size)
        constants = self.device.create_buffer_with_data(
            data=upstream._pack_constants(width, height), usage=wgpu.BufferUsage.UNIFORM
        )
        params = self.device.create_buffer_with_data(
            data=adapter_harness._pack_uniform(depth, viewport, scissor),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        baseline_groups = self._bind_group(
            self.baseline, self.baseline_pipeline, constants,
            {"g_samplerPointClamp": self.sampler,
             "g_srcRawDepthNormals": baseline_depth.create_view(),
             "g_outNormalmap": baseline_normals.create_view()},
        )
        fused_groups = self._bind_group(
            self.fused, self.fused_pipeline, constants,
            {"p": {"buffer": params, "size": 48},
             "cleanDepth": clean_texture.create_view(),
             "fusedRawDepth": fused_depth.create_view(),
             "g_outNormalmap": fused_normals.create_view()},
        )
        encoder = self.device.create_command_encoder()
        # The shared raster helper intentionally makes a readback-only output.
        # A texture copy adds sampling usage without any CPU/GPU arithmetic.
        encoder.copy_texture_to_texture(
            {"texture": raster_depth}, {"texture": baseline_depth}, (width, height, 1)
        )
        for info, pipeline, groups in (
            (self.baseline, self.baseline_pipeline, baseline_groups),
            (self.fused, self.fused_pipeline, fused_groups),
        ):
            compute = encoder.begin_compute_pass()
            compute.set_pipeline(pipeline)
            for index, group in enumerate(groups):
                compute.set_bind_group(index, group)
            compute.dispatch_workgroups(*self._dispatch(info, width, height))
            compute.end()
        self.device.queue.submit([encoder.finish()])
        return (
            self._read(raster_depth, size, np.float32),
            self._read(baseline_normals, size, np.uint32),
            self._read(fused_depth, size, np.float32),
            self._read(fused_normals, size, np.uint32),
        )


class FusedAdapterSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.baseline = _normal_info(SHADER_DIR)
        except upstream.ToolchainUnavailable as exc:
            raise unittest.SkipTest(str(exc)) from exc
        cls.fused = _fused_info(cls.baseline)

    def test_generated_normal_arithmetic_and_packing_are_verbatim(self):
        for name in (
            "XeGTAO_ScreenSpaceToViewSpaceDepth_0", "XeGTAO_CalculateEdges_0",
            "XeGTAO_ComputeViewspacePosition_0", "XeGTAO_CalculateNormal_0",
            "XeGTAO_FLOAT3_to_R11G11B10_UNORM_0",
        ):
            self.assertEqual(_function(self.baseline.code, name), _function(self.fused.code, name))
        start = "    var _S15 : f32"
        baseline = _function(self.baseline.code, "XeGTAO_ComputeViewspaceNormal_0")
        candidate = _function(self.fused.code, "XeGTAO_ComputeViewspaceNormal_0")
        self.assertEqual(baseline[baseline.index(start):], candidate[candidate.index(start):])

    def test_adapter_expression_five_fetches_and_dispatch_guard(self):
        code = self.fused.code
        self.assertEqual(code.count("canonicalDepth(vec2i(pixCoord_0)"), 5)
        self.assertNotIn("textureGather(", code)
        self.assertIn("if (any(pixCoord_1.xy >= textureDimensions(cleanDepth))) { return; }", code)
        canonical = _function(adapter_harness._adapter_source(), "fs_canonical_depth")
        self.assertIn(canonical[canonical.index("{") + 1:-1], code)
        self.assertLess(code.index("let at = clamp(inputCoord"), code.index("!receiver(pixel.xy)"))


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class XeGTAOFusedAdapterGPU(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            _normal_info(SHADER_DIR)
        except upstream.ToolchainUnavailable as exc:
            raise unittest.SkipTest(str(exc)) from exc
        # Reuse the actual adapter extraction, raster pipeline, upload and
        # render helpers. Imported test cases are not rediscovered here.
        adapter_harness.XeGTAOAdapterGPU.setUpClass()
        cls.renderer = _NormalStages(adapter_harness.XeGTAOAdapterGPU, SHADER_DIR)

    def _assert_identical(self, clean, **coverage):
        baseline_depth, baseline_normals, fused_depth, fused_normals = self.renderer.compare(clean, **coverage)
        self.assertTrue(np.isfinite(baseline_depth).all())
        for name, baseline, fused in (
            ("canonical raw-depth bits", baseline_depth.view(np.uint32), fused_depth.view(np.uint32)),
            ("packed normal bits", baseline_normals, fused_normals),
        ):
            np.testing.assert_array_equal(fused, baseline, err_msg=name)

    def test_planes_contacts_and_depth_discontinuities(self):
        for width, height in ((128, 72), (129, 73), (33, 19)):
            plane, _ = upstream.plane(width, height)
            contact, _ = upstream.feet_scene(width, height)
            step = np.full((height, width), 400.0, dtype=np.float32)
            step[:, width // 2:] = 1800.0
            step[height // 3:2 * height // 3, width // 3:2 * width // 3] = 90.0
            step[::3, ::5] = 0.0
            for name, clean in (("plane", plane), ("contacts", contact), ("discontinuities", step)):
                with self.subTest(size=(width, height), scene=name):
                    self._assert_identical(clean)

    def test_clear_nan_nonpositive_and_depth_clamping(self):
        values = np.array(
            [0.0, -0.0, -1.0, -1000.0, np.nan, -np.inf, np.inf,
             1e-30, 1.0, 19.99, 20.0, 20.01, 400.0, 9999.0, 10000.0, 20000.0],
            dtype=np.float32,
        )
        for name, clean in (
            ("clear", np.zeros((11, 17), dtype=np.float32)),
            ("all-nan", np.full((11, 17), np.nan, dtype=np.float32)),
            ("mixed", np.resize(values, (11, 17))),
        ):
            with self.subTest(scene=name):
                self._assert_identical(clean)

    def test_viewport_scissor_offsets_crops_and_image_edges(self):
        width, height = 33, 19
        y, x = np.mgrid[:height, :width]
        clean = (60.0 + x * 17.0 + y * 31.0).astype(np.float32)
        clean[::4, ::6] = 0.0
        cases = (
            ((0, 0, 1, 1), (0, 0, 1, 1)),
            ((5 / width, 3 / height, 19 / width, 11 / height), (8 / width, 2 / height, 18 / width, 13 / height)),
            ((-0.4, -0.2, 1.5, 1.4), (2 / width, 1 / height, 5 / width, 3 / height)),
            ((0, 0, 1, 1), (27 / width, 14 / height, 6 / width, 5 / height)),
            ((0.5 / width, 0.5 / height, (width - 1) / width, (height - 1) / height), (0, 0, 1, 1)),
            ((0, 0, 0.25, 0.25), (0.75, 0.75, 0.25, 0.25)),
        )
        for viewport, scissor in cases:
            with self.subTest(viewport=viewport, scissor=scissor):
                self._assert_identical(clean, viewport=viewport, scissor=scissor)

    def test_small_and_odd_dispatch_sizes(self):
        for width, height in ((1, 1), (1, 7), (9, 1), (2, 2), (3, 5), (7, 9), (17, 11)):
            y, x = np.mgrid[:height, :width]
            clean = (80.0 + x * 23.0 + y * 13.0).astype(np.float32)
            clean[(x + y) % 4 == 3] = 0.0
            with self.subTest(size=(width, height)):
                self._assert_identical(clean)


@dataclass
class _PreparedTiming:
    width: int
    height: int
    # Keep inputs and outputs alive for the entire warmup/measured sequence.
    textures: tuple
    raw_depth_view: object
    raster_group: object
    baseline_groups: list
    fused_groups: list


class _TimingExperiment(_NormalStages):
    """Preallocated synthetic preparation with pass-boundary GPU queries."""

    def __init__(self, shader_dir):
        if not HAS_GPU_DEPS:
            raise upstream.GpuUnavailable("numpy and wgpu==0.32.0 are required")
        self.adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if self.adapter is None:
            raise upstream.GpuUnavailable("no high-performance adapter")
        if "timestamp-query" not in self.adapter.features:
            raise upstream.GpuUnavailable("adapter does not support timestamp-query")
        device = self.adapter.request_device_sync(required_features=["timestamp-query"])
        source = adapter_harness._adapter_source()
        raster = SimpleNamespace(
            device=device, source=source, resources=adapter_harness._resources(source)
        )
        super().__init__(raster, shader_dir)
        # Same actual adapter module/layout as the correctness fixture, on the
        # timestamp-enabled device. Only its canonical entry point is needed.
        self.depth_layout = adapter_harness.XeGTAOAdapterGPU._make_layout.__func__(raster, (0, 1))
        module = device.create_shader_module(code=source)
        self.depth_pipeline = device.create_render_pipeline(
            layout=device.create_pipeline_layout(bind_group_layouts=[self.depth_layout]),
            vertex={"module": module, "entry_point": "vs_main"},
            fragment={"module": module, "entry_point": "fs_canonical_depth", "targets": [{"format": "r32float"}]},
            primitive={"topology": "triangle-list"},
        )

    def prepare(self, width, height):
        size = (width, height)
        clean, _ = upstream.feet_scene(width, height)
        clean = np.ascontiguousarray(clean, dtype=np.float32)
        sample_usage = wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST
        output_usage = wgpu.TextureUsage.STORAGE_BINDING | wgpu.TextureUsage.COPY_SRC
        clean_texture = self._texture(size, "r32float", sample_usage)
        # Unlike the general raster helper, this allocation is sampled
        # directly by normals. No copy or readback exists between the passes.
        raw_depth = self._texture(
            size, "r32float", wgpu.TextureUsage.RENDER_ATTACHMENT
            | wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_SRC,
        )
        baseline_normals = self._texture(size, "r32uint", output_usage)
        fused_depth = self._texture(size, "r32float", output_usage)
        fused_normals = self._texture(size, "r32uint", output_usage)
        self._write(clean_texture, clean, size)
        depth = (20.0 * 10000.0 / (10000.0 - 20.0), 10000.0 / (10000.0 - 20.0), 0.4, 0.0)
        constants = self.device.create_buffer_with_data(
            data=upstream._pack_constants(width, height), usage=wgpu.BufferUsage.UNIFORM
        )
        params = self.device.create_buffer_with_data(
            data=adapter_harness._pack_uniform(depth, (0, 0, 1, 1), (0, 0, 1, 1)),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        clean_view = clean_texture.create_view()
        raw_view = raw_depth.create_view()
        raster_group = self.device.create_bind_group(
            layout=self.depth_layout,
            entries=[{"binding": 0, "resource": {"buffer": params, "size": 48}},
                     {"binding": 1, "resource": clean_view}],
        )
        baseline_groups = self._bind_group(
            self.baseline, self.baseline_pipeline, constants,
            {"g_samplerPointClamp": self.sampler, "g_srcRawDepthNormals": raw_view,
             "g_outNormalmap": baseline_normals.create_view()},
        )
        fused_groups = self._bind_group(
            self.fused, self.fused_pipeline, constants,
            {"p": {"buffer": params, "size": 48}, "cleanDepth": clean_view,
             "fusedRawDepth": fused_depth.create_view(),
             "g_outNormalmap": fused_normals.create_view()},
        )
        return _PreparedTiming(
            width, height,
            (clean_texture, raw_depth, baseline_normals, fused_depth, fused_normals),
            raw_view, raster_group, baseline_groups, fused_groups,
        )

    def encode(self, encoder, prepared, condition, query=None, index=0):
        def timestamps(at):
            if query is None:
                return {}
            return {"timestamp_writes": {
                "query_set": query, "beginning_of_pass_write_index": at,
                "end_of_pass_write_index": at + 1,
            }}

        if condition == "baseline":
            render = encoder.begin_render_pass(
                label="baseline canonical depth", **timestamps(index),
                color_attachments=[{
                    "view": prepared.raw_depth_view, "clear_value": (1, 0, 0, 0),
                    "load_op": "clear", "store_op": "store",
                }],
            )
            render.set_pipeline(self.depth_pipeline)
            render.set_bind_group(0, prepared.raster_group)
            render.draw(3)
            render.end()
            index += 2
            info, pipeline, groups = self.baseline, self.baseline_pipeline, prepared.baseline_groups
        else:
            info, pipeline, groups = self.fused, self.fused_pipeline, prepared.fused_groups
        compute = encoder.begin_compute_pass(label=info.name, **timestamps(index))
        compute.set_pipeline(pipeline)
        for group_index, group in enumerate(groups):
            compute.set_bind_group(group_index, group)
        compute.dispatch_workgroups(*self._dispatch(info, prepared.width, prepared.height))
        compute.end()
        return index + 2

    def run_size(self, width, height):
        prepared = self.prepare(width, height)
        order = ("baseline", "fused", "fused", "baseline")
        # Ten ABBA blocks warm each condition twenty times. Queue order puts
        # every warmup before measurement, without a synchronizing readback.
        warmup = self.device.create_command_encoder(label="preparation warmup")
        for _ in range(10):
            for condition in order:
                self.encode(warmup, prepared, condition)
        self.device.queue.submit([warmup.finish()])

        frames_per_condition = 120
        query_count = frames_per_condition * (4 + 2)
        query = self.device.create_query_set(type="timestamp", count=query_count)
        resolved = self.device.create_buffer(
            size=query_count * 8,
            usage=wgpu.BufferUsage.QUERY_RESOLVE | wgpu.BufferUsage.COPY_SRC,
        )
        encoder = self.device.create_command_encoder(label="preparation ABBA timestamps")
        records = []
        index = 0
        for _ in range(frames_per_condition // 2):
            for condition in order:
                records.append((condition, index))
                index = self.encode(encoder, prepared, condition, query, index)
        if index != query_count:
            raise AssertionError("timestamp allocation does not match encoded passes")
        # Exactly one resolve and read after the measured loop. Textures,
        # uniforms, views, pipelines, and bind groups were already allocated.
        encoder.resolve_query_set(query, 0, query_count, resolved, 0)
        self.device.queue.submit([encoder.finish()])
        values = np.frombuffer(self.device.queue.read_buffer(resolved), dtype=np.uint64)
        samples = {
            "baseline_canonical_ms": [], "baseline_normals_ms": [],
            "baseline_interval_sum_ms": [], "baseline_first_to_last_ms": [],
            "fused_interval_sum_ms": [], "fused_first_to_last_ms": [],
        }
        for condition, at in records:
            first, second = int(values[at]), int(values[at + 1])
            if second < first:
                raise AssertionError("non-monotonic GPU timestamp interval")
            if condition == "baseline":
                third, fourth = int(values[at + 2]), int(values[at + 3])
                if fourth < third or fourth < first:
                    raise AssertionError("non-monotonic baseline GPU timestamp span")
                canonical = (second - first) / 1e6
                normals = (fourth - third) / 1e6
                samples["baseline_canonical_ms"].append(canonical)
                samples["baseline_normals_ms"].append(normals)
                samples["baseline_interval_sum_ms"].append(canonical + normals)
                samples["baseline_first_to_last_ms"].append((fourth - first) / 1e6)
            else:
                elapsed = (second - first) / 1e6
                samples["fused_interval_sum_ms"].append(elapsed)
                samples["fused_first_to_last_ms"].append(elapsed)

        # Post-measurement correctness readbacks also cover production-scale
        # reciprocal coordinates. They are outside all measured timestamps.
        _, baseline_depth, baseline_normals, fused_depth, fused_normals = prepared.textures
        size = (width, height)
        for label, baseline, fused, dtype in (
            ("canonical raw-depth bits", baseline_depth, fused_depth, np.float32),
            ("packed normal bits", baseline_normals, fused_normals, np.uint32),
        ):
            np.testing.assert_array_equal(
                self._read(fused, size, dtype).view(np.uint32),
                self._read(baseline, size, dtype).view(np.uint32), err_msg=label,
            )

        def distribution(data):
            data = np.asarray(data, dtype=np.float64)
            return {"count": int(data.size), "min": float(data.min()),
                    "p50": float(np.percentile(data, 50)), "p90": float(np.percentile(data, 90)),
                    "p95": float(np.percentile(data, 95)), "p99": float(np.percentile(data, 99)),
                    "max": float(data.max()), "mean": float(data.mean())}

        return {
            "size": [width, height], "scene": "synthetic feet/contact depth",
            "warmup_frames_per_condition": 20, "measured_frames_per_condition": frames_per_condition,
            "order": "ABBA (baseline, fused, fused, baseline), repeated 60 times",
            "post_measurement_bitwise_depth_and_normals_equal": True,
            "distributions_ms": {key: distribution(data) for key, data in samples.items()},
        }


def _run_timing_experiment(shader_dir):
    experiment = _TimingExperiment(shader_dir)
    scope = (
        "Synthetic canonical-depth/normal preparation only; excludes clean-depth production, "
        "other XeGTAO stages, MP6/native frame timing, and Android performance. "
        "Repeated static scene with preallocated resources; no speed acceptance assertion."
    )
    print(json.dumps({"scope": scope, "adapter": dict(experiment.adapter.info)}, sort_keys=True), flush=True)
    for width, height in ((2340, 1080), (2560, 1440)):
        print(json.dumps(experiment.run_size(width, height), sort_keys=True), flush=True)
    return 0


def main(argv=None):
    global SHADER_DIR
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--shader-dir", type=Path, default=upstream.DEFAULT_SHADER_DIR)
    parser.add_argument("--backend", choices=("Vulkan", "D3D12"))
    parser.add_argument("--timing-experiment", action="store_true")
    parsed, unittest_args = parser.parse_known_args(argv)
    SHADER_DIR = parsed.shader_dir
    if parsed.backend:
        os.environ["WGPU_BACKEND_TYPE"] = parsed.backend
    if parsed.timing_experiment:
        return _run_timing_experiment(SHADER_DIR)
    result = unittest.main(module=__name__, argv=[sys.argv[0], *unittest_args], exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
