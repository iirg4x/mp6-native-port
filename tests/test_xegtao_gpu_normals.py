"""CPU contracts for the optional XeGTAO supplied-normal harness path."""

from __future__ import annotations

import inspect
from pathlib import Path
import sys
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / "tests" / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

import test_xegtao_gpu as harness


@unittest.skipUnless(harness.np is not None, "optional NumPy dependency is unavailable")
class XeGTAONormalPackingCPU(unittest.TestCase):
    def test_pack_uses_upstream_r11g11b10_unorm_rounding(self):
        np = harness.np
        values = np.asarray(
            [[[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [-1.0, 0.0, 0.0]]],
            dtype=np.float32,
        )
        packed = harness.pack_viewspace_normals(values, (1, 4))
        expected = np.asarray(
            [
                (2047) | (1024 << 11) | (512 << 22),
                (1024) | (2047 << 11) | (512 << 22),
                (1024) | (1024 << 11) | (1023 << 22),
                (1024 << 11) | (512 << 22),
            ],
            dtype=np.uint32,
        ).reshape(1, 4)
        np.testing.assert_array_equal(packed, expected)

    def test_input_is_normalized_before_packing(self):
        np = harness.np
        values = np.asarray([[[3.0, 0.0, 4.0]]], dtype=np.float32)
        packed = harness.pack_viewspace_normals(values, (1, 1))
        decoded = harness._decode_packed_normals(packed)
        np.testing.assert_allclose(decoded[0, 0], [0.6, 0.0, 0.8], atol=1.0 / 1023.0)

    def test_shape_finite_and_nonzero_length_validation(self):
        np = harness.np
        valid = np.ones((2, 3, 3), dtype=np.float32)
        with self.assertRaisesRegex(ValueError, "shape"):
            harness.pack_viewspace_normals(valid, (3, 2))
        with self.assertRaisesRegex(ValueError, "HxWx3"):
            harness.pack_viewspace_normals(np.ones((2, 3, 2), dtype=np.float32), (2, 3))
        for bad in (
            np.full((2, 3, 3), np.nan, dtype=np.float32),
            np.full((2, 3, 3), np.inf, dtype=np.float32),
            np.zeros((2, 3, 3), dtype=np.float32),
        ):
            with self.subTest(value=bad.flat[0]):
                with self.assertRaises(ValueError):
                    harness.pack_viewspace_normals(bad, (2, 3))

    def test_default_and_supplied_stage_orders_are_explicit(self):
        default = harness._render_stage_order("CSGTAOHigh", 2, False)
        supplied = harness._render_stage_order("CSGTAOHigh", 2, True)
        self.assertEqual(default[0], harness.PREFILTER.name)
        self.assertEqual(default[1], harness.NORMALS.name)
        self.assertEqual(supplied[0], harness.PREFILTER.name)
        self.assertNotIn(harness.NORMALS.name, supplied)
        self.assertEqual(default[-1], supplied[-1])
        self.assertEqual(default[-2:], ["CSDenoisePass", "CSDenoiseLastPass"])

    def test_optional_projection_aspect_is_finite_positive_and_packed(self):
        self.assertAlmostEqual(harness._validated_aspect(1280, 720), 1280.0 / 720.0)
        captured = harness._validated_aspect(1280, 720, 1.59000015)
        self.assertAlmostEqual(captured, 1.59000015)
        default_constants = harness._pack_constants(1280, 720)
        captured_constants = harness._pack_constants(1280, 720, captured)
        self.assertEqual(len(default_constants), 96)
        self.assertEqual(len(captured_constants), 96)
        # tanHalfFOV.x is the first scalar after the two depth-unpack values.
        default_tan_x = struct.unpack_from("<f", default_constants, 24)[0]
        captured_tan_x = struct.unpack_from("<f", captured_constants, 24)[0]
        self.assertNotEqual(default_tan_x, captured_tan_x)
        for bad in (0.0, -1.0, float("nan"), float("inf"), "wide"):
            with self.subTest(aspect=bad):
                with self.assertRaises(ValueError):
                    harness._validated_aspect(1280, 720, bad)
        source = inspect.getsource(harness.Renderer.render)
        self.assertIn("aspect: float | None = None", source)

    def test_main_binding_and_snapshot_lifecycle_remain_present(self):
        self.assertIn("g_srcNormalmapMain", harness.MAIN_REQUIRED)
        source = inspect.getsource(harness.Renderer.render)
        self.assertIn("supplied_packed_normals", source)
        self.assertIn("reused.normal_snapshot", source)
        upload = source.index("self._write(normalmap, supplied_packed_normals")
        snapshot = source.index("reused.normal_snapshot", upload)
        stage_loop = source.index("for index, stage_name in enumerate(stage_order)")
        self.assertLess(snapshot, stage_loop)


if __name__ == "__main__":
    unittest.main()
