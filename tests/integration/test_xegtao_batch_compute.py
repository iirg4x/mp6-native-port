"""Bounded GPU parity checks for the one-compute-pass XeGTAO harness path.

The batch path keeps the generated shaders, stage order, dispatch dimensions,
and production alias plan unchanged.  It only changes command encoding: the
prefilter, optional normal reconstruction, quality pass, and denoise passes
share one compute pass.  Intermediate normal/raw-AO diagnostics are therefore
intentionally unavailable (``None``); final packed AO and all depth MIPs are
the parity contract.

This is an optional WebGPU test and has no native/Android assumptions.  Run it
serially with the same generated shader directory used by
``test_xegtao_gpu.py``::

    python tests/integration/test_xegtao_batch_compute.py -v
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys
import unittest

INTEGRATION = Path(__file__).resolve().parent
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

try:
    import test_xegtao_gpu as harness
except ImportError:  # pragma: no cover - malformed checkout
    harness = None


HAS_GPU_DEPS = (
    harness is not None
    and harness.HAS_GPU_DEPS
)
REQUESTED_BACKEND = os.environ.get("WGPU_BACKEND_TYPE") or None


def _assert_backend(adapter, requested: str | None, label: str) -> None:
    """Fail loudly if an explicit backend request was silently ignored."""

    if not requested:
        return
    info = dict(adapter.info)
    selected = str(info.get("backend_type", "")).strip()
    if selected.casefold() != requested.strip().casefold():
        raise AssertionError(
            f"{label} selected backend {selected!r}, requested {requested!r}; "
            f"adapter info={info!r}"
        )


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class XeGTAOBatchComputeGPU(unittest.TestCase):
    """Compare split and single-pass command encoding across alias transitions."""

    baseline: object
    batched: object

    @classmethod
    def setUpClass(cls):
        try:
            cls.baseline = harness.Renderer(harness.RENDERER_SHADER_DIR)
            cls.batched = harness.Renderer(harness.RENDERER_SHADER_DIR)
        except (harness.GpuUnavailable, harness.ToolchainUnavailable) as exc:
            raise unittest.SkipTest(str(exc)) from exc
        if "CSGTAOUltra" not in cls.baseline.pipelines:
            raise unittest.SkipTest("generated upstream Ultra shader is unavailable")
        requested = REQUESTED_BACKEND or os.environ.get("WGPU_BACKEND_TYPE")
        _assert_backend(cls.baseline.adapter, requested, "baseline renderer")
        _assert_backend(cls.batched.adapter, requested, "batch renderer")

    def test_final_ao_and_depth_mip_bits_match_for_reused_scene_matrix(self):
        """Exercise clear -> plane -> contacts -> clear for every policy."""

        np = harness.np
        quality_levels = ("Low", "Medium", "High", "Ultra")
        # One even and one odd viewport cover padded and non-padded dispatch
        # dimensions without turning this optional serial test into a stress
        # benchmark.
        sizes = ((128, 72), (129, 73))

        for width, height in sizes:
            ground, _ = harness.plane(width, height)
            contacts, _ = harness.feet_scene(width, height)
            clear = np.zeros((height, width), dtype=np.float32)
            scenes = (
                ("clear", clear),
                ("plane", ground),
                ("contacts", contacts),
                ("clear-again", clear),
            )

            for quality in quality_levels:
                for denoise_passes in (1, 2, 3):
                    with self.subTest(
                        size=(width, height),
                        quality=quality,
                        denoise_passes=denoise_passes,
                    ):
                        baseline_ids = None
                        batch_ids = None
                        for scene_name, scene in scenes:
                            baseline = self.baseline.render(
                                scene,
                                quality=quality,
                                denoise_passes=denoise_passes,
                                reuse_resources=True,
                            )
                            batched = self.batched.render(
                                scene,
                                quality=quality,
                                denoise_passes=denoise_passes,
                                reuse_resources=True,
                                batch_compute=True,
                            )

                            with self.subTest(scene=scene_name):
                                self.assertEqual(self.batched.last_compute_pass_count, 1)
                                self.assertEqual(
                                    self.baseline.last_compute_pass_count,
                                    len(harness._render_stage_order(
                                        f"CSGTAO{quality}", denoise_passes, False
                                    )),
                                )
                                self.assertIsNone(batched.packed_normals)
                                self.assertIsNone(batched.raw_ao)
                                self.assertIsNotNone(baseline.packed_normals)
                                self.assertIsNotNone(baseline.raw_ao)
                                self.assertIsNotNone(baseline.final_ao)
                                self.assertIsNotNone(batched.final_ao)
                                np.testing.assert_array_equal(
                                    batched.final_ao,
                                    baseline.final_ao,
                                    err_msg="finalAO bits differ",
                                )
                                self.assertEqual(len(batched.depth_mips), 5)
                                for level, (expected, actual) in enumerate(
                                    zip(baseline.depth_mips, batched.depth_mips)
                                ):
                                    np.testing.assert_array_equal(
                                        actual.view(np.uint32),
                                        expected.view(np.uint32),
                                        err_msg=f"depth MIP {level} bits differ",
                                    )

                                reused_baseline = self.baseline._reused_textures[(width, height)]
                                reused_batch = self.batched._reused_textures[(width, height)]
                                current_baseline_ids = tuple(
                                    id(texture) for texture in reused_baseline.working_textures
                                )
                                current_batch_ids = tuple(
                                    id(texture) for texture in reused_batch.working_textures
                                )
                                self.assertEqual(len(set(current_baseline_ids)), 4)
                                self.assertEqual(len(set(current_batch_ids)), 4)
                                if baseline_ids is None:
                                    baseline_ids = current_baseline_ids
                                    batch_ids = current_batch_ids
                                self.assertEqual(current_baseline_ids, baseline_ids)
                                self.assertEqual(current_batch_ids, batch_ids)

    def test_batch_timing_reports_one_aggregate_row(self):
        """Timestamp output must describe the shared pass, never fake stages."""

        try:
            timed = harness.Renderer(harness.RENDERER_SHADER_DIR, timing=True)
        except (harness.GpuUnavailable, harness.ToolchainUnavailable) as exc:
            self.skipTest(f"timestamp-query is unavailable: {exc}")
        requested = REQUESTED_BACKEND or os.environ.get("WGPU_BACKEND_TYPE")
        _assert_backend(timed.adapter, requested, "timed batch renderer")

        scene = harness.np.zeros((19, 33), dtype=harness.np.float32)
        result = timed.render(
            scene,
            quality="High",
            denoise_passes=3,
            reuse_resources=True,
            batch_compute=True,
        )
        self.assertIsNotNone(result.timings_ms)
        self.assertEqual(tuple(result.timings_ms), (harness.BATCH_TIMING_LABEL,))
        elapsed = result.timings_ms[harness.BATCH_TIMING_LABEL]
        self.assertTrue(math.isfinite(elapsed))
        self.assertGreaterEqual(elapsed, 0.0)
        self.assertEqual(timed.last_timings_ms, result.timings_ms)
        self.assertNotIn(harness.PREFILTER.name, result.timings_ms)
        self.assertNotIn(harness.NORMALS.name, result.timings_ms)
        self.assertNotIn("CSGTAOHigh", result.timings_ms)
        self.assertNotIn("CSDenoisePass", result.timings_ms)
        self.assertNotIn("CSDenoiseLastPass", result.timings_ms)


def main(argv: list[str] | None = None) -> int:
    """Run the optional parity matrix with the upstream harness options."""

    global REQUESTED_BACKEND
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--shader-dir", type=Path, default=None)
    parser.add_argument("--backend", choices=("Vulkan", "D3D12"))
    parsed, unittest_args = parser.parse_known_args(argv)
    if parsed.shader_dir is not None:
        harness.RENDERER_SHADER_DIR = parsed.shader_dir
    if parsed.backend:
        REQUESTED_BACKEND = parsed.backend
        os.environ["WGPU_BACKEND_TYPE"] = parsed.backend
    unittest_argv = [sys.argv[0], *unittest_args]
    result = unittest.main(module=__name__, argv=unittest_argv, exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
