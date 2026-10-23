"""CPU-only contracts for the synthetic leaf-motion registration fixture."""

from pathlib import Path
import sys
import unittest


try:
    import numpy as np
except ImportError:  # pragma: no cover - optional analytic dependency
    np = None


INTEGRATION = Path(__file__).resolve().parent / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

# The integration module keeps its optional GPU imports inside the test method,
# so importing these pure fixture helpers does not initialize a device.
from test_ao_leaf_motion import leaf_scene, world_samples


@unittest.skipIf(np is None, "optional NumPy unavailable")
class LeafSceneContracts(unittest.TestCase):
    @staticmethod
    def _basis():
        normal = np.array([0.05, 0.55, 0.833], dtype=np.float64)
        normal /= np.linalg.norm(normal)
        across = np.cross(normal, np.array([0.0, 1.0, 0.0]))
        across /= np.linalg.norm(across)
        along = np.cross(normal, across)
        return normal, across, along

    def test_world_ray_projection_round_trip_even_and_odd_sizes(self):
        normal, across, along = self._basis()
        base = np.array([0.0, 0.0, -180.0], dtype=np.float64)
        gy, gx = np.mgrid[-35:36:1.0, -62:63:1.0]
        world = base + gx[..., None] * across + gy[..., None] * along
        tan = np.tan(np.pi / 8.0)

        for width, height in ((640, 360), (641, 361)):
            for shift in (-0.7, 0.7):
                with self.subTest(size=(width, height), shift=shift):
                    _, _, (expected_u, expected_v), _ = leaf_scene(
                        np, width, height, shift, lift=10.0
                    )
                    origin = across * shift
                    view = world - origin
                    projected_u = (
                        view[..., 0] / (-view[..., 2]) / (tan * width / height) + 1
                    ) * width / 2 - 0.5
                    projected_v = (
                        1 - view[..., 1] / (-view[..., 2]) / tan
                    ) * height / 2 - 0.5
                    np.testing.assert_allclose(
                        expected_u, projected_u, rtol=0, atol=1e-12
                    )
                    np.testing.assert_allclose(
                        expected_v, projected_v, rtol=0, atol=1e-12
                    )

                    rays = np.stack(
                        (
                            ((expected_u + 0.5) / width * 2 - 1)
                            * tan
                            * width
                            / height,
                            -((expected_v + 0.5) / height * 2 - 1) * tan,
                            -np.ones_like(expected_u),
                        ),
                        axis=-1,
                    )
                    denominator = rays @ normal
                    distance = ((base - origin) @ normal) / denominator
                    recovered = origin + rays * distance[..., None]
                    np.testing.assert_allclose(
                        recovered, world, rtol=0, atol=1e-9
                    )

    def test_positive_across_translation_moves_fixed_world_point_right(self):
        for width, height in ((640, 360), (641, 361)):
            with self.subTest(size=(width, height)):
                center = (35, 62)
                projected_u = [
                    float(leaf_scene(np, width, height, shift, lift=10.0)[2][0][center])
                    for shift in (-0.7, -0.2, 0.2, 0.7)
                ]
                self.assertTrue(
                    np.all(np.diff(projected_u) > 0),
                    f"expected increasing u for positive across shifts: {projected_u}",
                )

    def test_common_mask_rejects_a_covered_bilinear_tap(self):
        visibility = np.full((12, 12), 0.73, dtype=np.float64)
        coords = (np.array([[5.25]], dtype=np.float64), np.array([[5.25]], dtype=np.float64))
        clear = np.ones((12, 12), dtype=bool)
        covered = clear.copy()
        covered[5, 6] = False

        _, valid_clear = world_samples(np, visibility, clear, coords)
        _, valid_covered = world_samples(np, visibility, covered, coords)
        common = np.logical_and.reduce((valid_clear, valid_covered))

        self.assertTrue(bool(valid_clear[0, 0]))
        self.assertFalse(bool(valid_covered[0, 0]))
        self.assertFalse(bool(common[0, 0]))

    def test_constant_visibility_stays_constant_on_valid_receivers(self):
        for width, height in ((12, 10), (13, 11)):
            with self.subTest(size=(width, height)):
                visibility = np.full((height, width), 0.73, dtype=np.float64)
                u = np.array([[4.125, 5.5, 6.875], [4.75, 5.25, 6.5]])
                v = np.array([[4.125, 4.5, 5.25], [4.75, 5.5, 4.25]])
                aligned, valid = world_samples(
                    np, visibility, np.ones((height, width), dtype=bool), (u, v)
                )
                self.assertTrue(bool(valid.all()))
                np.testing.assert_allclose(aligned[valid], 0.73, rtol=0, atol=1e-12)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
