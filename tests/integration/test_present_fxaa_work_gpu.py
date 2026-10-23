"""Exact-pixel and bounded ABBA timing checks for present FXAA work.

This is a private desktop GPU experiment. It reuses the existing present
renderer behavior in a local subclass with an explicit shader-source override;
the shared ``test_present_fusion_gpu.py`` harness is not edited. It makes no
Android performance claim.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import statistics
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/ao-gpu-test-deps"))
sys.path.insert(0, str(ROOT / "tests/integration"))
import numpy as np
import wgpu

from test_present_fusion_gpu import PresentRenderer as _BasePresentRenderer
from tests.test_present_fxaa_work import replay_pre_0075


RUN_BENCHMARK = os.environ.get("MP6_BENCHMARK_PRESENT_FXAA") == "1"


def resample_shader(text: str) -> str:
    match = re.search(r'resampleShaderSource = R"\((.*?)\)"sv;', text, re.S)
    if not match:
        raise AssertionError("resample shader literal is absent")
    return match.group(1)


def outer_candidate(shader: str) -> str:
    old = """@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    var color = textureSample(t, s, in.uv);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    if (uniforms.post_aa_mode == 1u) {
        color = vec4<f32>(apply_fxaa(in.uv), color.a);
    }
    return vec4(color.rgb, 1.0);
}"""
    new = """@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    if (uniforms.post_aa_mode == 1u) {
        return vec4<f32>(apply_fxaa(in.uv), 1.0);
    }
    var color = textureSample(t, s, in.uv);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    return vec4(color.rgb, 1.0);
}"""
    if shader.count(old) != 1:
        raise AssertionError("current fs_main shape is not the post-0074 form")
    return shader.replace(old, new, 1)


def zero_direction_candidate(shader: str) -> str:
    anchor = """    dir = clamp(dir * rcp_dir_min, vec2<f32>(-8.0, -8.0), vec2<f32>(8.0, 8.0)) * rcp;

    let rgb_a = 0.5 * ("""
    replacement = """    dir = clamp(dir * rcp_dir_min, vec2<f32>(-8.0, -8.0), vec2<f32>(8.0, 8.0)) * rcp;
    if (all(dir == vec2<f32>(0.0, 0.0))) {
        return rgb_m;
    }

    let rgb_a = 0.5 * ("""
    if shader.count(anchor) != 1:
        raise AssertionError("normalized-dir anchor is not unique")
    return shader.replace(anchor, replacement, 1)


class PresentRenderer(_BasePresentRenderer):
    """Existing renderer with only the resample WGSL source overridden."""

    def __init__(self, shader: str, source: str):
        copy = re.search(r'sourceDescriptor.code = R"""\((.*?)\)""";', source, re.S).group(1)
        self.adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        self.device = self.adapter.request_device_sync(required_features=["timestamp-query"])
        self.modules = [self.device.create_shader_module(code=s) for s in (shader, copy)]
        self.sampler = self.device.create_sampler(mag_filter="linear", min_filter="linear")
        self.pipelines = {}


def assert_exact(before, after, label):
    np.testing.assert_array_equal(after, before, err_msg=label)


class PresentFxaaWorkGpu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base_text = replay_pre_0075()
        cls.base_shader = resample_shader(base_text)
        cls.outer_shader = outer_candidate(cls.base_shader)
        cls.zero_shader = zero_direction_candidate(cls.outer_shader)
        cls.base = PresentRenderer(cls.base_shader, base_text)
        cls.outer = PresentRenderer(cls.outer_shader, base_text)
        cls.zero = PresentRenderer(cls.zero_shader, base_text)
        print("GPU:", json.dumps(dict(cls.base.adapter.info)), flush=True)

    def test_exact_bytes_rgba_bgra_sizes_modes_and_overlays(self):
        sizes = [((1, 1), (1, 1)), ((127, 73), (511, 257)),
                 ((511, 257), (127, 73)), ((257, 129), (257, 129)),
                 ((1024, 576), (512, 288))]
        comparisons = 0
        for fmt in ("rgba8unorm", "bgra8unorm"):
            for source, target in sizes:
                for fxaa, area in ((0, 0), (0, 1), (1, 0), (1, 1)):
                    for overlay in (False, True):
                        with self.subTest(fmt=fmt, source=source, target=target,
                                          fxaa=fxaa, area=area, overlay=overlay):
                            args = dict(fmt=fmt, fxaa=fxaa, area=area,
                                        direct=True, overlay=overlay)
                            expected = self.base.run(source, target, **args)
                            actual = self.outer.run(source, target, **args)
                            assert_exact(expected, actual, "outer FXAA branch changed bytes")
                            optional = self.zero.run(source, target, **args)
                            assert_exact(expected, optional, "zero-dir candidate changed bytes")
                            comparisons += 1
        print("EXACT_CASES_MAIN", comparisons, flush=True)
        self.assertEqual(comparisons, 80)

    def test_exact_bytes_letterbox_and_up_downsample(self):
        comparisons = 0
        for fmt in ("rgba8unorm", "bgra8unorm"):
            for source, target, surface, origin in (
                    ((2957, 1848), (2957, 1848), (2960, 1848), (1, 0)),
                    ((127, 73), (127, 73), (135, 79), (3, 2)),
                    ((511, 257), (511, 257), (520, 280), (4, 11))):
                for fxaa in (0, 1):
                    args = dict(fmt=fmt, fxaa=fxaa, area=1, direct=True,
                                surface_size=surface, origin=origin)
                    expected = self.base.run(source, target, **args)
                    actual = self.outer.run(source, target, **args)
                    optional = self.zero.run(source, target, **args)
                    assert_exact(expected, actual, "outer letterbox bytes changed")
                    assert_exact(expected, optional, "zero-dir letterbox bytes changed")
                    comparisons += 1
        print("EXACT_CASES_LETTERBOX", comparisons, flush=True)
        self.assertEqual(comparisons, 12)

    def _abba(self, reference, candidate, *, fxaa, area):
        values = []
        # Same-process alternating order reduces drift from device clocks and
        # driver compilation. Reference and candidate are both timestamped
        # across the same A/B/A/B sequence.
        for trial in range(2):
            order = ((reference, candidate, candidate, reference)
                     if trial % 2 == 0 else
                     (candidate, reference, reference, candidate))
            for renderer in order:
                renderer.run((1024, 576), (1024, 576), fmt="rgba8unorm",
                             fxaa=fxaa, area=area, direct=True, repeat=32)
                values.append(("candidate" if renderer is candidate else "reference",
                               float(renderer.last_ms)))
        baseline = [value for name, value in values if name == "reference"]
        optimized = [value for name, value in values if name == "candidate"]
        return {
            "reference_ms": baseline,
            "candidate_ms": optimized,
            "reference_median_ms": statistics.median(baseline),
            "candidate_median_ms": statistics.median(optimized),
            "regressed_over_5pct": statistics.median(optimized) > statistics.median(baseline) * 1.05,
        }

    def test_outer_branch_timestamp_abba(self):
        if not RUN_BENCHMARK:
            self.skipTest("set MP6_BENCHMARK_PRESENT_FXAA=1 for timestamp report")
        for area, label in ((0, "BILINEAR_DEFAULT"), (1, "AREA_SAMPLER")):
            report = self._abba(self.base, self.outer, fxaa=1, area=area)
            report = dict(source_size=(1024, 576), target_size=(1024, 576),
                          repeat=32, area=area, **report)
            print("OUTER_ABBA_" + label, json.dumps(report), flush=True)

    def test_zero_direction_timestamp_abba_separate(self):
        if not RUN_BENCHMARK:
            self.skipTest("set MP6_BENCHMARK_PRESENT_FXAA=1 for timestamp report")
        report = self._abba(self.outer, self.zero, fxaa=1, area=1)
        print("ZERO_DIR_ABBA", json.dumps(report), flush=True)
        # This is evidence only; the optional candidate is not in 0075. A
        # measurable regression keeps it out of the production patch.


if __name__ == "__main__":
    unittest.main(verbosity=2)
