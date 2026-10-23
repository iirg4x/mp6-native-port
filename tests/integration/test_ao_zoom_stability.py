"""Zoom/camera-distance contracts for the production AO paths.

The production AO radius is authored in view/world units.  A camera move or a
FOV change therefore changes the *screen-space* footprint by design.  This
fixture keeps that distinction explicit:

* the CPU tests prove the legacy projection, its close-range cap, and its
  far-range minimum-step behavior from the production constants;
* the optional GPU test renders one unchanged contact scene at several camera
  distances and reports plane neutrality, contact darkening, and the analytic
  projected footprint.  It is intentionally opt-in because this fixture may
  allocate a GPU device and is not part of the normal CPU regression suite.

Run the GPU section only after an explicit test slot is available:

    $env:MP6_RUN_AO_ZOOM_GPU = "1"
    python tests/integration/test_ao_zoom_stability.py -v

The GPU section reports measurements; it does not invent a visual-quality
threshold from one device.  It only asserts structural facts (finite output,
flat-plane neutrality for the legacy control, and valid contact-mask geometry).
"""

from __future__ import annotations

from dataclasses import dataclass
import importlib.util
import math
import os
from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
LEGACY_SHADER = ROOT / "src" / "gx" / "ambient_occlusion_shader.hpp"
XEGTAO_PARAMETERS = ROOT / "src" / "gx" / "xegtao" / "parameters.hpp"
XEGTAO_GENERATED = ROOT / "src" / "gx" / "xegtao" / "generated_shaders.hpp"


@dataclass(frozen=True)
class CameraProjection:
    """The camera values consumed by the legacy projected-radius expression."""

    width: int
    height: int
    tan_half_fov_y: float
    aspect: float
    viewport_x: float = 0.0
    viewport_y: float = 0.0
    viewport_width: float = 1.0
    viewport_height: float = 1.0


@dataclass(frozen=True)
class RadiusObservation:
    depth: float
    unclamped_uv: tuple[float, float]
    clamped_uv: tuple[float, float]
    pixels: tuple[float, float]
    cap_active: tuple[bool, bool]


def _legacy_radius(camera: CameraProjection, depth: float, radius: float = 32.0) -> RadiusObservation:
    """Evaluate the legacy shader's projectedRadius/minStep expressions.

    This is a scalar CPU model of the exact WGSL at lines 189-210.  It is not
    an AO implementation and deliberately does not approximate shading.
    """

    if depth <= 0.0:
        raise ValueError("view depth must be positive")
    common = radius / (2.0 * depth * camera.tan_half_fov_y)
    unclamped = (
        common * camera.viewport_width / camera.aspect,
        common * camera.viewport_height,
    )
    clamped = tuple(min(0.08, value) for value in unclamped)
    pixels = (clamped[0] * camera.width, clamped[1] * camera.height)
    return RadiusObservation(
        depth=depth,
        unclamped_uv=unclamped,
        clamped_uv=clamped,
        pixels=pixels,
        cap_active=(unclamped[0] > 0.08, unclamped[1] > 0.08),
    )


def _legacy_min_step_for_angle(observation: RadiusObservation, camera: CameraProjection,
                               angle: float) -> float:
    """Evaluate the legacy shader's exact minStep for one unit direction.

    ``fs_ao`` multiplies the two-component projected radius by
    ``vec2(cos(angle), -sin(angle))`` before multiplying by the full image
    dimensions.  The step is therefore direction-dependent; this helper keeps
    the direction explicit instead of collapsing it to the diagonal length.
    """

    direction_x = math.cos(angle)
    direction_y = math.sin(angle)
    sample_length = math.hypot(
        direction_x * observation.clamped_uv[0] * camera.width,
        direction_y * observation.clamped_uv[1] * camera.height,
    )
    return 1.3 / max(sample_length, 1.3)


def _xegtao_projected_pixels(camera: CameraProjection, depth: float, radius: float = 32.0,
                             radius_multiplier: float = 1.457) -> tuple[float, float]:
    """XeGTAO's upstream effective radius projected into working pixels.

    The pinned generated shader evaluates ``effectRadius = Radius * 1.457``
    before dividing by ``viewspaceZ * NDCToViewMul_x_PixelSize.x``.  Its
    ``omega`` offset is a scalar pixel radius multiplied by the unit direction,
    so both returned components use that same scalar.  This is the same
    physical projection as the legacy path, without the legacy 8% cap or its
    per-slice minimum-step guard.
    """

    if depth <= 0.0:
        raise ValueError("view depth must be positive")
    # NDCToViewMul_x_PixelSize.x = (2 * tanHalfFovX) / width for the full
    # normalized viewport.  XeGTAO then uses this scalar as both x/y pixel
    # components of omega, rather than a legacy vec2 projected radius.
    tan_half_fov_x = camera.tan_half_fov_y * camera.aspect
    pixel_radius = (radius * radius_multiplier) / (2.0 * depth * tan_half_fov_x) * camera.width
    return (pixel_radius, pixel_radius)


def _camera(width: int = 1024, height: int = 576, tan_half_fov_y: float = math.tan(math.pi / 8.0),
            aspect: float | None = None) -> CameraProjection:
    return CameraProjection(
        width=width,
        height=height,
        tan_half_fov_y=tan_half_fov_y,
        aspect=width / height if aspect is None else aspect,
    )


def _optional_gpu_modules():
    """Load the established GPU fixture only when explicitly requested."""

    if os.environ.get("MP6_RUN_AO_ZOOM_GPU") != "1":
        return None
    dependency_root = ROOT / "build" / "ao-gpu-test-deps"
    if dependency_root.is_dir() and str(dependency_root) not in sys.path:
        sys.path.insert(0, str(dependency_root))
    tests_root = ROOT / "tests"
    if str(tests_root) not in sys.path:
        sys.path.insert(0, str(tests_root))
    if importlib.util.find_spec("numpy") is None or importlib.util.find_spec("wgpu") is None:
        return None
    from integration.test_ao_gpu import Renderer as LegacyRenderer, feet_scene, plane
    from integration.test_xegtao_gpu import (
        GpuUnavailable,
        Renderer as XeGTAORenderer,
        ToolchainUnavailable,
    )
    import numpy as np
    return LegacyRenderer, XeGTAORenderer, GpuUnavailable, ToolchainUnavailable, feet_scene, plane, np


def _feet_scene_at_distance(np, plane, width: int, height: int, distance: float):
    """Build the established two-shoe contact at a different camera depth.

    This is the existing ``feet_scene`` fixture with only its camera-space
    placement parameterized.  Shoe dimensions, lift, separation, and the
    supporting plane are otherwise unchanged.  It lets the opt-in GPU sweep
    distinguish a real contact from a flat-plane control without changing the
    production AO radius or strength.
    """

    z, _ = plane(width, height, distance=distance)
    y, x = np.mgrid[:height, :width]
    tan = math.tan(math.radians(45) / 2.0)
    ray = np.stack((((x + .5) / width * 2 - 1) * tan * width / height,
                    -((y + .5) / height * 2 - 1) * tan,
                    -np.ones_like(z)), axis=-1)
    normal = np.array([.10, .65, .753325958])
    normal /= np.linalg.norm(normal)
    across = np.cross(normal, [0, 1, 0])
    across /= np.linalg.norm(across)
    along = np.cross(normal, across)
    axes = np.stack((across, normal, along), axis=1)
    size = np.array([9., 7., 15.])
    local_ray = (ray @ axes) / size
    a = (local_ray * local_ray).sum(axis=-1)
    ground = ray * z[..., None]
    contact = np.zeros(z.shape, dtype=bool)
    for offset in (-12., 12.):
        base = np.array([0., 0., -distance]) + across * offset
        center = base + normal * 8.
        local_origin = (-center @ axes) / size
        b = 2 * (local_ray * local_origin).sum(axis=-1)
        c = (local_origin * local_origin).sum() - 1
        discriminant = b * b - 4 * a * c
        hit = (-b - np.sqrt(np.maximum(discriminant, 0))) / (2 * a)
        z = np.where((discriminant >= 0) & (hit > 0), np.minimum(z, hit), z)
        on_ground = (ground - base) @ axes
        contact |= (on_ground[..., 0] / 17) ** 2 + (on_ground[..., 2] / 23) ** 2 < 1
    original, _ = plane(width, height, distance=distance)
    contact &= np.abs(original - z) < .0001
    return z, contact


class ZoomProjectionContracts(unittest.TestCase):
    """Exact camera/radius invariants; no GPU allocation."""

    def test_source_keeps_world_radius_and_explicit_zoom_policy(self):
        source = LEGACY_SHADER.read_text(encoding="utf-8")
        self.assertIn("let radius = 32.0; // world units, independent of resolution/camera zoom", source)
        self.assertIn("let projectedRadius = min(vec2f(0.08), radius / (2.0 * z * p.projection.x)", source)
        self.assertIn("let minStep = 1.3 / max(length(screenDirection * p.sizes.xy), 1.3);", source)

    def test_same_world_radius_scales_inversely_with_view_depth(self):
        camera = _camera()
        near = _legacy_radius(camera, 800.0)
        far = _legacy_radius(camera, 1600.0)
        # Use depths outside the 0.08 cap so this is the underlying projection,
        # not a statement that the cap itself is scale invariant.
        self.assertFalse(any(near.cap_active))
        self.assertFalse(any(far.cap_active))
        self.assertTrue(math.isclose(near.unclamped_uv[0] / far.unclamped_uv[0], 2.0,
                                     rel_tol=1e-12, abs_tol=0.0))
        self.assertTrue(math.isclose(near.unclamped_uv[1] / far.unclamped_uv[1], 2.0,
                                     rel_tol=1e-12, abs_tol=0.0))

    def test_same_world_radius_scales_with_inverse_fov_tangent(self):
        narrow = _legacy_radius(_camera(tan_half_fov_y=0.4), 1600.0)
        wide = _legacy_radius(_camera(tan_half_fov_y=0.8), 1600.0)
        self.assertFalse(any(narrow.cap_active))
        self.assertFalse(any(wide.cap_active))
        self.assertTrue(math.isclose(narrow.unclamped_uv[0] / wide.unclamped_uv[0], 2.0,
                                     rel_tol=1e-12, abs_tol=0.0))
        self.assertTrue(math.isclose(narrow.unclamped_uv[1] / wide.unclamped_uv[1], 2.0,
                                     rel_tol=1e-12, abs_tol=0.0))

    def test_normalized_radius_is_resolution_independent(self):
        small = _legacy_radius(_camera(640, 360), 1600.0)
        large = _legacy_radius(_camera(2560, 1440), 1600.0)
        self.assertEqual(small.clamped_uv, large.clamped_uv)
        self.assertTrue(math.isclose(large.pixels[0] / small.pixels[0], 4.0,
                                     rel_tol=1e-12, abs_tol=0.0))
        self.assertTrue(math.isclose(large.pixels[1] / small.pixels[1], 4.0,
                                     rel_tol=1e-12, abs_tol=0.0))

    def test_cap_and_directional_minimum_step_are_measurable_policy_boundaries(self):
        camera = _camera()
        close = _legacy_radius(camera, 100.0)
        # At this deliberately far depth the projected radius is below the
        # shader's 1.3-pixel guard, so its minimum-step term reaches exactly
        # one.  This is a sampling boundary, not an AO-quality target.
        far = _legacy_radius(camera, 160000.0)
        self.assertEqual(close.clamped_uv, (0.08, 0.08))
        self.assertEqual(close.cap_active, (True, True))
        self.assertEqual(far.cap_active, (False, False))
        # minStep is evaluated per explicit unit sample direction in the
        # shader.  These values expose the sampling boundary without claiming
        # a particular visual-quality tolerance.
        close_x = _legacy_min_step_for_angle(close, camera, 0.0)
        close_y = _legacy_min_step_for_angle(close, camera, math.pi / 2.0)
        far_x = _legacy_min_step_for_angle(far, camera, 0.0)
        far_diagonal = _legacy_min_step_for_angle(far, camera, math.pi / 4.0)
        self.assertLess(close_x, 1.0)
        self.assertLess(close_y, 1.0)
        self.assertEqual(far_x, 1.0)
        self.assertEqual(far_diagonal, 1.0)

    def test_xegtao_projection_uses_camera_fov_and_pixel_size(self):
        parameters = XEGTAO_PARAMETERS.read_text(encoding="utf-8")
        generated = XEGTAO_GENERATED.read_text(encoding="utf-8")
        self.assertIn("const float tanHalfFovX = tanHalfFovY * aspect;", parameters)
        self.assertIn("constants.NDCToViewMul_x_PixelSize", parameters)
        # Slang's temporary IDs change when bindings (e.g. the Hilbert LUT)
        # change. Check the complete radius expression for every quality,
        # without mistaking an incidental _S16 -> _S14 rename for a defect.
        projection = re.compile(
            r"var screenspaceRadius_\d+ : f32 = effectRadius_\d+ / "
            r"\(vec2<f32>\(viewspaceZ_\d+\) \* "
            r"\(\*_S\d+\)\.NDCToViewMul_x_PixelSize_\d+\)\.x;"
        )
        self.assertEqual(len(projection.findall(generated)), 4)

    def test_xegtao_effective_radius_follows_same_world_projection(self):
        camera = _camera()
        near = _xegtao_projected_pixels(camera, 800.0)
        far = _xegtao_projected_pixels(camera, 1600.0)
        self.assertTrue(math.isclose(near[0] / far[0], 2.0,
                                     rel_tol=1e-12, abs_tol=0.0))
        self.assertEqual(near[0], near[1])
        self.assertEqual(far[0], far[1])

        narrow = _xegtao_projected_pixels(_camera(tan_half_fov_y=0.4), 1600.0)
        wide = _xegtao_projected_pixels(_camera(tan_half_fov_y=0.8), 1600.0)
        self.assertTrue(math.isclose(narrow[0] / wide[0], 2.0,
                                     rel_tol=1e-12, abs_tol=0.0))
        self.assertTrue(all(value > 0.0 for value in near + far + narrow + wide))


class OptionalZoomGpuMeasurements(unittest.TestCase):
    """Distance sweep; enabled only with MP6_RUN_AO_ZOOM_GPU=1."""

    @unittest.skipUnless(os.environ.get("MP6_RUN_AO_ZOOM_GPU") == "1",
                         "GPU zoom sweep is explicitly opt-in")
    def test_same_contact_scene_reports_zoom_metrics(self):
        modules = _optional_gpu_modules()
        if modules is None:
            self.skipTest("numpy/wgpu optional dependencies are unavailable")
        LegacyRenderer, XeGTAORenderer, GpuUnavailable, ToolchainUnavailable, _feet_scene, plane, np = modules
        try:
            legacy = LegacyRenderer()
            xegtao = XeGTAORenderer()
        except (GpuUnavailable, ToolchainUnavailable) as exc:
            self.skipTest(str(exc))
        width, height = 1024, 576
        camera = _camera(width, height)
        strength = .35
        for distance in (200.0, 400.0, 800.0, 1600.0):
            # Recreate the same small contact at each depth by changing only
            # camera-space placement; mesh dimensions, lift, radius and shader
            # strength stay unchanged.  The flat plane is the neutral control.
            ground, _ = plane(width, height, distance=distance)
            _raw, _filtered, legacy_plane_output = legacy.render(
                ground, strength=strength, ao_scale=1.0)
            feet, contact = _feet_scene_at_distance(np, plane, width, height, distance)
            _raw, _filtered, legacy_feet_output = legacy.render(
                feet, strength=strength, ao_scale=1.0)
            xe_ground = xegtao.render(ground, quality="High", denoise_passes=2).decoded_visibility
            xe_feet = xegtao.render(feet, quality="High", denoise_passes=2).decoded_visibility

            # XeGTAO exposes AO-only decoded visibility.  Match the production
            # composite's MP6 strength application exactly once, as in
            # src/gx/xegtao/composite_shader.hpp.
            xe_ground_mp6 = 1.0 - strength * (1.0 - xe_ground)
            xe_feet_mp6 = 1.0 - strength * (1.0 - xe_feet)
            legacy_plane_signal = 1.0 - legacy_plane_output[..., 0]
            legacy_contact_delta = (legacy_plane_output[..., 0] - legacy_feet_output[..., 0])[contact]
            xe_plane_signal = 1.0 - xe_ground_mp6
            xe_contact_delta = (xe_ground_mp6 - xe_feet_mp6)[contact]
            self.assertTrue(np.isfinite(legacy_plane_output).all())
            self.assertTrue(np.isfinite(legacy_feet_output).all())
            self.assertTrue(np.isfinite(xe_ground_mp6).all())
            self.assertTrue(np.isfinite(xe_feet_mp6).all())
            self.assertTrue(np.isfinite(legacy_contact_delta).all())
            self.assertTrue(np.isfinite(xe_contact_delta).all())
            self.assertTrue(np.all(legacy_plane_output[..., 0] == 1.0))
            self.assertGreater(int(contact.sum()), 0)
            radius = _legacy_radius(camera, distance)
            xegtao_radius = _xegtao_projected_pixels(camera, distance)
            print("AO_ZOOM_METRIC", {
                "depth": distance,
                "legacy_projected_radius_px": radius.pixels,
                "xegtao_projected_radius_px": xegtao_radius,
                "legacy_cap_active": radius.cap_active,
                "legacy_min_step_angle0": _legacy_min_step_for_angle(radius, camera, 0.0),
                "legacy_min_step_angle90": _legacy_min_step_for_angle(radius, camera, math.pi / 2.0),
                "legacy_min_step_angle45": _legacy_min_step_for_angle(radius, camera, math.pi / 4.0),
                "legacy_plane_max_darkening": float(legacy_plane_signal.max()),
                "legacy_plane_mean_darkening": float(legacy_plane_signal.mean()),
                "legacy_contact_delta_max": float(legacy_contact_delta.max()),
                "legacy_contact_delta_q75": float(np.quantile(legacy_contact_delta, .75)),
                "legacy_contact_delta_mean": float(legacy_contact_delta.mean()),
                "xegtao_plane_max_darkening": float(xe_plane_signal.max()),
                "xegtao_plane_mean_darkening": float(xe_plane_signal.mean()),
                "xegtao_contact_delta_max": float(xe_contact_delta.max()),
                "xegtao_contact_delta_q75": float(np.quantile(xe_contact_delta, .75)),
                "xegtao_contact_delta_mean": float(xe_contact_delta.mean()),
            }, flush=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
