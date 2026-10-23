"""Exploratory XeGTAO crease-normal fixtures.

The fixture is intentionally a perspective construction, not a screen-space
linear-z toy.  Each half is a real plane intersected by the camera ray.  A
convex ridge therefore has its crease closer to the camera while a concave
valley has its crease farther away.  The optional GPU test reports edge and
side visibility metrics for both crease alignments; it does not impose a
vendor-sensitive pass/fail threshold yet.

Run explicitly (GPU and optional dependencies required)::

    python tests/integration/test_xegtao_crease_normals.py --report
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "tests" / "integration") not in sys.path:
    sys.path.insert(0, str(ROOT / "tests" / "integration"))

from test_xegtao_gpu import (
    GpuUnavailable,
    RENDERER_SHADER_DIR,
    Renderer,
    ToolchainUnavailable,
    wgpu,
    np,
)


FOV_DEGREES = 45.0
NEAR = 20.0
FAR = 10000.0
RADIUS = 32.0
REFERENCE_DEPTH = 1000.0
SLOPE = 0.75


@dataclass(frozen=True)
class CreaseFixture:
    """A perspective depth field and masks around one geometric crease."""

    kind: str
    alignment: str
    depth: "np.ndarray"
    edge_mask: "np.ndarray"
    left_mask: "np.ndarray"
    right_mask: "np.ndarray"
    crease_u: float
    slope: float


def _require_numpy():
    if np is None:
        raise RuntimeError("NumPy is required for crease fixtures")


def _camera_ray_xy(width: int, height: int):
    """Return per-pixel ray x/z and y/z for a 45-degree perspective camera."""

    _require_numpy()
    tan_y = math.tan(math.radians(FOV_DEGREES) * 0.5)
    tan_x = tan_y * width / height
    x = ((np.arange(width, dtype=np.float32) + 0.5) / width * 2.0 - 1.0) * tan_x
    y = ((np.arange(height, dtype=np.float32) + 0.5) / height * 2.0 - 1.0) * tan_y
    return np.broadcast_to(x[None, :], (height, width)), np.broadcast_to(y[:, None], (height, width)), tan_x


def _crease_u(width: int, height: int, alignment: str) -> float:
    tan_y = math.tan(math.radians(FOV_DEGREES) * 0.5)
    tan_x = tan_y * width / height
    if alignment == "pixel":
        # For the default odd width, this is exactly the center pixel ray.
        pixel = (width - 1) // 2
        sample = pixel + 0.5
    elif alignment == "between":
        # For an even width, this is exactly the boundary between the two
        # central pixels.  It remains a sub-pixel boundary for odd widths.
        sample = width // 2
    else:
        raise ValueError("alignment must be 'pixel' or 'between'")
    return float((2.0 * sample / width - 1.0) * tan_x)


def make_crease_fixture(width: int = 257, height: int = 145, *, kind: str = "convex",
                        alignment: str = "pixel", slope: float = SLOPE,
                        depth: float = REFERENCE_DEPTH) -> CreaseFixture:
    """Construct two perspective plane pieces meeting on a half-plane crease.

    A plane is represented as ``z = depth + plane_slope*x`` in camera space.
    With ray coordinate ``u=x/z``, its intersection is
    ``z = depth / (1 - plane_slope*u)``.  This keeps the fixture geometric and
    perspective-correct while placing the crease around z=1000.
    """

    _require_numpy()
    if not isinstance(width, int) or not isinstance(height, int) or width <= 16 or height <= 16:
        raise ValueError("fixture dimensions must be integer values greater than 16")
    if kind not in ("convex", "concave"):
        raise ValueError("kind must be convex or concave")
    try:
        slope = float(slope)
        depth = float(depth)
    except (TypeError, ValueError) as exc:
        raise ValueError("slope and depth must be finite numbers") from exc
    if not math.isfinite(slope) or not 0.5 <= slope <= 1.0:
        raise ValueError("slope must be in the requested 0.5..1 range")
    if not math.isfinite(depth) or depth <= 0.0:
        raise ValueError("reference depth must be a positive finite number")
    ray_x, _, _ = _camera_ray_xy(width, height)
    crease = _crease_u(width, height, alignment)
    local_u = ray_x - np.float32(crease)

    # Convex: the crease is nearer and both planes recede away from it.
    # Concave: the crease is farther and both planes approach it.
    if kind == "convex":
        left_slope, right_slope = -float(slope), float(slope)
    else:
        left_slope, right_slope = float(slope), -float(slope)
    plane_slope = np.where(local_u < 0.0, left_slope, right_slope)
    z = np.float32(depth) / (1.0 - plane_slope * local_u)
    z = np.ascontiguousarray(z, dtype=np.float32)
    if not bool(np.isfinite(z).all()) or float(np.min(z)) < NEAR or float(np.max(z)) > FAR:
        raise ValueError("crease fixture produces depth outside the configured near/far range")

    # A narrow edge strip is the metric of interest; side masks provide a
    # local, same-fixture reference without assuming a global scene average.
    tan_y = math.tan(math.radians(FOV_DEGREES) * 0.5)
    tan_x = tan_y * width / height
    pixel_u = 2.0 * tan_x / width
    edge = np.abs(local_u) <= np.float32(1.5 * pixel_u)
    left = local_u < np.float32(-3.0 * pixel_u)
    right = local_u > np.float32(3.0 * pixel_u)
    # Keep the masks away from the 8-pixel sampler/clamp border so the report
    # measures the crease rather than edge handling at the render target.
    interior = np.zeros_like(edge, dtype=bool)
    interior[8:-8, 8:-8] = True
    edge &= interior
    left &= interior
    right &= interior
    return CreaseFixture(kind, alignment, z, edge, left, right, crease, float(slope))


def metric_mask_report(result, baseline, fixture: CreaseFixture) -> dict:
    """Summarize visibility around the crease for exploratory CLI output."""

    _require_numpy()
    visibility = np.asarray(result.decoded_visibility, dtype=np.float32)
    base_visibility = np.asarray(baseline.decoded_visibility, dtype=np.float32)
    edge = fixture.edge_mask
    side = fixture.left_mask | fixture.right_mask
    if not bool(edge.any()) or not bool(side.any()):
        raise ValueError("crease masks unexpectedly empty")

    def median(values):
        return float(np.median(values))

    edge_visibility = visibility[edge]
    side_visibility = visibility[side]
    return {
        "kind": fixture.kind,
        "alignment": fixture.alignment,
        "fov_degrees": FOV_DEGREES,
        "near": NEAR,
        "far": FAR,
        "radius": RADIUS,
        "slope": fixture.slope,
        "crease_u": fixture.crease_u,
        "edge_pixels": int(edge.sum()),
        "side_pixels": int(side.sum()),
        "edge_visibility_median": median(edge_visibility),
        "side_visibility_median": median(side_visibility),
        "edge_occlusion_vs_flat_median": median(base_visibility[edge] - edge_visibility),
        "side_occlusion_vs_flat_median": median(base_visibility[side] - side_visibility),
        "visibility_min": float(np.min(visibility)),
        "visibility_max": float(np.max(visibility)),
    }


def _flat_fixture(width: int, height: int):
    _require_numpy()
    return np.full((height, width), np.float32(REFERENCE_DEPTH), dtype=np.float32)


def _gpu_report(renderer: Renderer, width: int, height: int, quality: str,
                denoise_passes: int) -> list[dict]:
    """Render both shapes/alignment cases and return metrics without thresholds."""

    baseline = renderer.render(_flat_fixture(width, height), quality=quality,
                               denoise_passes=denoise_passes)
    reports = []
    for alignment in ("pixel", "between"):
        for kind in ("convex", "concave"):
            fixture = make_crease_fixture(width, height, kind=kind, alignment=alignment)
            result = renderer.render(fixture.depth, quality=quality,
                                     denoise_passes=denoise_passes)
            report = metric_mask_report(result, baseline, fixture)
            # These are invariant harness sanity checks, not quality thresholds.
            if not np.isfinite(result.decoded_visibility).all():
                raise AssertionError(f"non-finite visibility in {kind}/{alignment}")
            reports.append(report)
    return reports


class CreaseFixtureCPU(unittest.TestCase):
    @unittest.skipUnless(np is not None, "optional NumPy unavailable")
    def test_perspective_plane_pieces_have_requested_ridge_and_valley_geometry(self):
        for alignment in ("pixel", "between"):
            convex = make_crease_fixture(kind="convex", alignment=alignment)
            concave = make_crease_fixture(kind="concave", alignment=alignment)
            center = convex.depth.shape[1] // 2
            edge = convex.edge_mask
            self.assertTrue(bool(edge.any()))
            self.assertEqual(convex.depth.shape, (145, 257))
            self.assertGreaterEqual(float(convex.depth[72, center]), REFERENCE_DEPTH - 1.0e-3)
            self.assertLessEqual(float(concave.depth[72, center]), REFERENCE_DEPTH + 1.0e-3)
            self.assertGreater(float(np.max(convex.depth) - REFERENCE_DEPTH), 1.0)
            self.assertLess(float(np.min(concave.depth) - REFERENCE_DEPTH), -1.0)

            # Verify the plane equation on both sides. This rejects a fake
            # linear-z fixture while tolerating float32 construction error.
            ray_x, _, _ = _camera_ray_xy(257, 145)
            local_u = ray_x - np.float32(convex.crease_u)
            for fixture, expected_left, expected_right in (
                (convex, -convex.slope, convex.slope),
                (concave, concave.slope, -concave.slope),
            ):
                left = local_u < 0.0
                right = ~left
                np.testing.assert_allclose(
                    fixture.depth[left] * (1.0 - expected_left * local_u[left]),
                    REFERENCE_DEPTH, rtol=2.0e-6, atol=2.0e-3)
                np.testing.assert_allclose(
                    fixture.depth[right] * (1.0 - expected_right * local_u[right]),
                    REFERENCE_DEPTH, rtol=2.0e-6, atol=2.0e-3)

    @unittest.skipUnless(np is not None, "optional NumPy unavailable")
    def test_metric_masks_are_local_to_the_crease(self):
        fixture = make_crease_fixture(kind="convex", alignment="between")
        self.assertGreater(int(fixture.edge_mask.sum()), 0)
        self.assertGreater(int(fixture.left_mask.sum()), 0)
        self.assertGreater(int(fixture.right_mask.sum()), 0)
        self.assertFalse(bool(np.any(fixture.edge_mask & fixture.left_mask)))
        self.assertFalse(bool(np.any(fixture.edge_mask & fixture.right_mask)))

    @unittest.skipUnless(np is not None, "optional NumPy unavailable")
    def test_fixture_rejects_invalid_dimensions_and_nonphysical_depth(self):
        for width, height in ((16, 145), (257, 16)):
            with self.subTest(width=width, height=height):
                with self.assertRaises(ValueError):
                    make_crease_fixture(width, height)
        with self.assertRaises(ValueError):
            make_crease_fixture(slope=math.nan)
        with self.assertRaises(ValueError):
            make_crease_fixture(depth=math.inf)
        with self.assertRaises(ValueError):
            make_crease_fixture(depth=NEAR * 0.5)
        with self.assertRaises(ValueError):
            make_crease_fixture(depth=FAR * 2.0)
        # A very wide frustum drives one plane intersection behind the camera;
        # the construction must reject it instead of returning fake geometry.
        with self.assertRaises(ValueError):
            make_crease_fixture(width=4096, height=17, slope=1.0)


HAS_GPU_DEPS = np is not None and wgpu is not None and Renderer is not None


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class CreaseNormalGPU(unittest.TestCase):
    renderer: Renderer

    @classmethod
    def setUpClass(cls):
        try:
            cls.renderer = Renderer(RENDERER_SHADER_DIR)
        except (GpuUnavailable, ToolchainUnavailable) as exc:
            raise unittest.SkipTest(str(exc)) from exc

    def test_exploratory_convex_ridge_and_concave_valley_metrics(self):
        reports = _gpu_report(self.renderer, 257, 145, "High", 2)
        for report in reports:
            print("[XEGTAO-CREASE] " + json.dumps(report, sort_keys=True), flush=True)
        self.assertEqual(len(reports), 4)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-dir", type=Path, default=RENDERER_SHADER_DIR)
    parser.add_argument("--quality", choices=("Low", "Medium", "High", "Ultra"), default="High")
    parser.add_argument("--denoise", type=int, choices=(1, 2, 3), default=2)
    parser.add_argument("--size", default="257x145", help="WIDTHxHEIGHT (default: 257x145)")
    parser.add_argument("--report", action="store_true", help="run exploratory GPU report")
    args = parser.parse_args(argv)
    if not args.report:
        parser.error("--report is required for the exploratory GPU run")
    match = args.size.lower().split("x")
    if len(match) != 2:
        parser.error("--size must be WIDTHxHEIGHT")
    try:
        width, height = (int(value) for value in match)
    except ValueError:
        parser.error("--size must be WIDTHxHEIGHT")
    if width <= 16 or height <= 16:
        parser.error("--size dimensions must be integer values greater than 16")
    if np is None or wgpu is None or Renderer is None:
        print("NumPy/wgpu optional dependencies are unavailable; no GPU report run.")
        return 2
    try:
        renderer = Renderer(args.shader_dir)
        reports = _gpu_report(renderer, width, height, args.quality, args.denoise)
    except (GpuUnavailable, ToolchainUnavailable) as exc:
        print(f"GPU fixture unavailable: {exc}")
        return 2
    except ValueError as exc:
        print(f"Invalid crease fixture: {exc}", file=sys.stderr)
        return 2
    for report in reports:
        print(json.dumps(report, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
