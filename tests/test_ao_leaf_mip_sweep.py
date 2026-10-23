"""CPU-only metric checks for the depth-MIP diagnostic."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "build/ao-gpu-test-deps"), str(ROOT / "tests/integration")]
try:
    import numpy as np
except ImportError:
    np = None
from ao_leaf_mip_sweep import motion_metrics, raw_visibility


@unittest.skipIf(np is None, "optional NumPy unavailable")
class LeafMipMetrics(unittest.TestCase):
    def test_constant_field_has_no_motion(self):
        row = motion_metrics(np, np.full((8, 9, 11), .75), np.ones((9, 11), bool))
        self.assertEqual(row["mean_darkening"], .25)
        self.assertEqual(row["temporal_std_mean"], 0.)
        self.assertEqual(row["coherent_5x5_std_mean"], 0.)
        self.assertEqual(row["samples"], 99)
        self.assertEqual(row["coherent_samples"], 35)

    def test_constant_receiver_with_masked_changes_has_no_motion(self):
        values = np.full((8, 9, 11), .75)
        values[:, 0, 0] = np.linspace(0, 1, 8)
        common = np.ones((9, 11), bool)
        common[0, 0] = False
        row = motion_metrics(np, values, common)
        self.assertEqual(row["temporal_std_mean"], 0.)
        self.assertEqual(row["coherent_5x5_std_mean"], 0.)

    def test_known_uniform_change_is_measured_not_hidden(self):
        values = np.broadcast_to(np.array([.8, .6])[:, None, None], (2, 9, 11))
        row = motion_metrics(np, values, np.ones((9, 11), bool))
        self.assertAlmostEqual(row["mean_darkening"], .3)
        self.assertAlmostEqual(row["temporal_std_mean"], .1)
        self.assertAlmostEqual(row["temporal_step_max"], .2)
        self.assertAlmostEqual(row["coherent_5x5_std_mean"], .1)

    def test_invalid_fields_fail_closed(self):
        common = np.ones((9, 11), bool)
        for values, mask in ((np.ones((1, 9, 11)), common),
                             (np.ones((2, 8, 11)), common),
                             (np.full((2, 9, 11), np.nan), common),
                             (np.full((2, 9, 11), 1.1), common),
                             (np.ones((2, 9, 11)), np.zeros((9, 11), bool))):
            with self.subTest(shape=values.shape):
                with self.assertRaises(ValueError):
                    motion_metrics(np, values, mask)

    def test_raw_working_ao_is_unscaled_and_saturated(self):
        header = (ROOT / "third_party/xegtao/XeGTAO.h").read_text()
        self.assertRegex(header, r"XE_GTAO_OCCLUSION_TERM_SCALE\s+\(1\.5")
        np.testing.assert_allclose(raw_visibility(np, np.array([0, 85, 170, 255], dtype=np.uint32)),
                                   [0, .5, 1, 1], rtol=0, atol=1e-6)

    def test_raw_working_ao_rejects_invalid_encoding(self):
        for values in ([256], [-1], [float("nan")], [float("inf")]):
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    raw_visibility(np, np.array(values))


if __name__ == "__main__":
    unittest.main()
