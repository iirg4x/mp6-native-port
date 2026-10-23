"""Explicit GPU diagnostic separating half AO input and reconstruction errors.

No production shader changes. Analytic low-resolution ray intersections are a
diagnostic oracle, not an implementable scene-independent downsampler. Camera
motion is registered to fixed receiver points; there is no history filtering.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]

# XeGTAO's main pass stores the pre-denoise working term as
# ``saturate(visibility / XE_GTAO_OCCLUSION_TERM_SCALE) * 255``.  The final
# denoise pass multiplies by that scale before packing, while
# ``RenderResult.decoded_visibility`` in test_xegtao_gpu.py is simply final
# packed-value / 255.  Keep the diagnostic's raw-AO variants in the same
# physical visibility units as the final variants, and apply MP6 strength
# exactly once at the final mapping/composite.
_RAW_AO_SCALE = 1.5

# leaf_scene() uses this one plane normal for both the receiver and the two
# raised leaves.  XeGTAO's view-space convention has positive Z along the
# forward/depth axis (see XeGTAO_ComputeViewspacePosition); the fixture camera
# looks along world -Z, so X/Y stay positive and world Z changes sign.
_GROUND_NORMAL_WORLD = (0.05, 0.55, 0.833)
_GROUND_NORMAL_LENGTH = math.sqrt(sum(value * value for value in _GROUND_NORMAL_WORLD))
_GROUND_NORMAL_VIEW = (
    _GROUND_NORMAL_WORLD[0] / _GROUND_NORMAL_LENGTH,
    _GROUND_NORMAL_WORLD[1] / _GROUND_NORMAL_LENGTH,
    -_GROUND_NORMAL_WORLD[2] / _GROUND_NORMAL_LENGTH,
)


def _pack_analytic_normal(np, width, height):
    """Pack the known fixture plane normal in XeGTAO R11G11B10 form."""

    encoded = [(value * 0.5 + 0.5) for value in _GROUND_NORMAL_VIEW]
    channels = (
        int(math.floor(min(1.0, max(0.0, encoded[0])) * 2047.0 + 0.5)),
        int(math.floor(min(1.0, max(0.0, encoded[1])) * 2047.0 + 0.5)),
        int(math.floor(min(1.0, max(0.0, encoded[2])) * 1023.0 + 0.5)),
    )
    packed = channels[0] | (channels[1] << 11) | (channels[2] << 22)
    return np.full((height, width), packed, dtype=np.uint32)


def _far_from_silhouette(np, leaf_mask, radius=2):
    """Return ground pixels whose radius-neighborhood contains no leaf."""

    mask = np.asarray(leaf_mask, dtype=bool)
    height, width = mask.shape
    result = np.ones_like(mask, dtype=bool)
    result[:radius] = False
    result[-radius:] = False
    result[:, :radius] = False
    result[:, -radius:] = False
    for dy in range(-radius, radius + 1):
        y0, y1 = max(0, -dy), min(height, height - dy)
        for dx in range(-radius, radius + 1):
            x0, x1 = max(0, -dx), min(width, width - dx)
            result[y0:y1, x0:x1] &= ~mask[y0 + dy:y1 + dy, x0 + dx:x1 + dx]
    return result


def _make_analytic_normal_renderer(np, renderer_type):
    """Build a local Renderer subclass without changing the shared harness."""

    class AnalyticNormalRenderer(renderer_type):
        """Override only the main pass normal input for one diagnostic render.

        The base Renderer owns all production-equivalent stage scheduling and
        readbacks.  CSGenerateNormals still executes and its snapshot remains
        the diagnostic result returned by RenderResult.packed_normals.
        """

        def __init__(self):
            super().__init__()
            self._np = np
            self._analytic_normal_view = None
            self._analytic_normal_textures = {}

        def _bind_group(self, info, pipeline, uniform, objects):
            if self._analytic_normal_view is not None and info.name == 'CSGTAOUltra':
                objects = dict(objects)
                objects['g_srcNormalmapMain'] = self._analytic_normal_view
            return super()._bind_group(info, pipeline, uniform, objects)

        def render_with_analytic_normals(self, z, quality='Ultra', denoise_passes=3,
                                         *, reuse_resources=True):
            height, width = map(int, self._np.asarray(z).shape)
            key = (width, height)
            cached = self._analytic_normal_textures.get(key)
            if cached is None:
                import wgpu

                texture = self._texture(
                    (width, height),
                    'r32uint',
                    wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST,
                )
                self._write(texture, _pack_analytic_normal(self._np, width, height), key)
                cached = (texture, texture.create_view())
                self._analytic_normal_textures[key] = cached
            self._analytic_normal_view = cached[1]
            try:
                return self.render(z, quality, denoise_passes, reuse_resources=reuse_resources)
            finally:
                self._analytic_normal_view = None

    return AnalyticNormalRenderer()


def _decode_raw_ao(np, packed):
    """Decode RenderResult.raw_ao into the final-pass visibility contract."""

    packed = np.asarray(packed, dtype=np.uint32)
    return np.clip(
        packed.astype(np.float32) / np.float32(255.0) * np.float32(_RAW_AO_SCALE),
        0.0,
        1.0,
    )


def _raw_ao_as_final_packed(np, packed):
    """Repack raw AO at final-pass scale for HalfCompositeShader input."""

    # XeGTAO's WGSL uint conversion is ``uint(value * 255.0 + 0.5)``;
    # use floor rather than NumPy's ties-to-even rint for the same rule.
    return np.floor(_decode_raw_ao(np, packed) * np.float32(255.0) +
                    np.float32(0.5)).astype(np.uint32)


def statistics(np, fields, common):
    values = 1.0 - np.stack(fields)
    selected = values[:, common]
    region = common[2:-2, 2:-2].copy()
    coherent = np.zeros_like(values[:, 2:-2, 2:-2])
    for dy in range(5):
        for dx in range(5):
            region &= common[dy:dy+region.shape[0], dx:dx+region.shape[1]]
            coherent += values[:, dy:dy+region.shape[0], dx:dx+region.shape[1]]/25.
    return dict(mean_darkening=float(selected.mean()),
                std_mean=float(np.std(selected, axis=0).mean()),
                step_max=float(np.abs(np.diff(selected, axis=0)).max()),
                coherent_std_mean=float(np.std(coherent[:, region], axis=0).mean()),
                coherent_step_max=float(np.abs(np.diff(coherent[:, region], axis=0)).max()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=('Vulkan', 'D3D12'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists() or not args.output.resolve().is_relative_to((ROOT/'build').resolve()):
        parser.error('--output must be a new file within build/')
    os.environ['WGPU_BACKEND_TYPE'] = args.backend
    sys.path[:0] = [str(ROOT/'build/ao-gpu-test-deps'), str(ROOT/'tests/integration'), str(ROOT/'tests')]
    import numpy as np
    from test_ao_leaf_motion import leaf_scene, world_samples
    from test_xegtao_gpu import Renderer, _decode_packed_normals
    from test_xegtao_resolution import XeGTAOResolutionGPU as Half, _linearize_canonical_depth
    xe = _make_analytic_normal_renderer(np, Renderer)
    Half.setUpClass()
    names = ('full', 'full_raw', 'half_current', 'half_actual_mip0',
             'half_raw_actual_mip0', 'half_analytic_input',
             'half_analytic_normals', 'half_analytic_normals_raw',
             'reconstruct_constant', 'reconstruct_constant_analytic_depth')
    fields = {name: [] for name in names}
    validities = []
    depth_errors = []
    normal_angle_errors = []
    for shift in np.linspace(-.7, .7, 8):
        depth, ground, coords, mask = leaf_scene(np, 640, 360, float(shift), 10.)
        analytic, _, _, low_mask = leaf_scene(np, 320, 180, float(shift), 10.)
        foliage = np.zeros((360, 640, 4), dtype=np.float32)
        constant = np.full((180, 320), 128, dtype=np.uint32)
        canonical, reconstructed_constant = Half._render_half(
            depth, constant, foliage, .35, (0, 0, 1, 1), (0, 0, 1, 1))
        adapter_linear = _linearize_canonical_depth(canonical)
        result_full = xe.render(depth, 'Ultra', 3, reuse_resources=True)
        result_low = xe.render(adapter_linear, 'Ultra', 3, reuse_resources=True)
        result_analytic = xe.render(analytic, 'Ultra', 3, reuse_resources=True)
        result_analytic_normals = xe.render_with_analytic_normals(
            adapter_linear, 'Ultra', 3, reuse_resources=True)
        # The oracle is a main-input replacement, not a different depth or
        # normal producer. Prove that the retained diagnostic stages match.
        np.testing.assert_array_equal(result_low.packed_normals,
                                      result_analytic_normals.packed_normals)
        for actual, overridden in zip(result_low.depth_mips,
                                      result_analytic_normals.depth_mips):
            np.testing.assert_array_equal(actual, overridden)
        original_normals = _decode_packed_normals(result_low.packed_normals)
        original_normals /= np.maximum(
            np.linalg.norm(original_normals, axis=-1, keepdims=True), 1e-8)
        target_normal = np.asarray(_GROUND_NORMAL_VIEW, dtype=np.float32)
        normal_cosine = np.clip(np.sum(original_normals * target_normal, axis=-1), -1.0, 1.0)
        normal_angles = np.arccos(normal_cosine)
        normal_ground = _far_from_silhouette(np, low_mask, radius=2)
        normal_angle_errors.extend(normal_angles[normal_ground].tolist())
        ao = np.rint(result_low.visibility*255.).astype(np.uint32)
        analytic_ao = np.rint(result_analytic.visibility*255.).astype(np.uint32)
        analytic_normal_ao = np.rint(result_analytic_normals.visibility*255.).astype(np.uint32)
        outputs = {
            'full': 1.-.35*(1.-result_full.visibility),
            # Raw AO is pre-denoise packed at the upstream 1.5x working-term
            # scale; decode it before applying MP6 strength once.
            'full_raw': 1.-.35*(1.-_decode_raw_ao(np, result_full.raw_ao)),
            'reconstruct_constant': reconstructed_constant[..., 3],
        }
        for name, signal, linear in (
            ('half_current', ao, None),
            ('half_actual_mip0', ao, result_low.mip0),
            ('half_raw_actual_mip0', _raw_ao_as_final_packed(np, result_low.raw_ao),
             result_low.mip0),
            ('half_analytic_input', analytic_ao, result_analytic.mip0),
            ('half_analytic_normals', analytic_normal_ao, result_analytic_normals.mip0),
            ('half_analytic_normals_raw',
             _raw_ao_as_final_packed(np, result_analytic_normals.raw_ao),
             result_analytic_normals.mip0),
            ('reconstruct_constant_analytic_depth', constant, result_analytic.mip0)):
            _, composite = Half._render_half(depth, signal, foliage, .35,
                                             (0, 0, 1, 1), (0, 0, 1, 1),
                                             linear_depth_override=linear)
            outputs[name] = composite[..., 3]
        errors = np.abs(adapter_linear-analytic)
        depth_errors.append(dict(mean=float(errors.mean()), maximum=float(errors.max()),
                                 count_over_one_unit=int((errors>1.).sum()),
                                 upstream_mip0_max=float(np.abs(adapter_linear-result_low.mip0).max())))
        for name, visibility in outputs.items():
            if not np.isfinite(visibility).all():
                raise AssertionError(f'nonfinite {name}')
            samples, valid = world_samples(np, visibility, ~mask, coords)
            fields[name].append(samples)
            validities.append(valid)
    common = np.logical_and.reduce(validities)
    if common.sum() < 1000:
        raise AssertionError('insufficient common receiver samples')
    normal_errors = np.asarray(normal_angle_errors, dtype=np.float32)
    if normal_errors.size < 1000 or not np.isfinite(normal_errors).all():
        raise AssertionError('insufficient or nonfinite far-ground normal errors')
    normal_error_stats = dict(
        target_world=list(_GROUND_NORMAL_WORLD),
        target_world_normalized=list(
            value / _GROUND_NORMAL_LENGTH for value in _GROUND_NORMAL_WORLD
        ),
        target_view=list(_GROUND_NORMAL_VIEW),
        source='result_low.packed_normals from CSGenerateNormals before main override',
        silhouette_exclusion_radius=2,
        samples=int(normal_errors.size),
        mean_radians=float(normal_errors.mean()),
        p95_radians=float(np.quantile(normal_errors, .95)),
        max_radians=float(normal_errors.max()),
    )
    # A Z-sign mistake produces an error near pi across this mask.  Keep the
    # bound deliberately loose for backend/packing differences while making
    # the independent view-space mapping an executable diagnostic contract.
    if normal_error_stats['p95_radians'] >= 0.25:
        raise AssertionError(
            f"ground normal view-space sign/mapping suspect: {normal_error_stats}"
        )
    report = dict(backend=args.backend, adapter=dict(xe.adapter.info),
                  scope='Stage-isolated synthetic motion diagnostic, not native parity or performance. '
                        'CPU float32 readback bridge remains; analytic low input is an unattainable '
                        'reference at silhouettes without subpixel scene coverage. Raw variants '
                        'isolate pre-denoise AO sampling from denoise/reconstruction; they are not '
                        'quality acceptance. Analytic-normal variants override only the low main '
                        'pass normal binding; the actual normal pass and its readback remain intact. '
                        'World registration excludes receiver disocclusion, but legitimate '
                        'view-dependent occluder coverage can still contribute to variation.',
                  common_receivers=int(common.sum()), frames=8, leaf_lift=10., strength=.35,
                  raw_ao_contract=dict(
                      upstream_working_scale=_RAW_AO_SCALE,
                      decode='clip(raw_uint / 255 * 1.5, 0, 1)',
                      final_decode='final_uint / 255',
                      strength_application='MP6 strength applied exactly once',
                      half_raw_repacking='Rounded to final 8-bit visibility for existing '
                                         'composite; full raw stays float. Additional half raw '
                                         'quantization is at most 0.5/255 upstream visibility.',
                  ),
                  normal_contract=normal_error_stats,
                  depth_errors=depth_errors,
                  signals={name: statistics(np, values, common) for name, values in fields.items()},
                  sources={str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in (Path(__file__),
                                        ROOT/'tests/integration/test_ao_leaf_motion.py',
                                        ROOT/'src/gx/xegtao/half_adapter_shader.hpp',
                                        ROOT/'src/gx/xegtao/half_composite_shader.hpp',
                                        ROOT/'src/gx/xegtao/generated_shaders.hpp')})
    # Directly quantify the fixture roundtrip difference independently of the
    # temporal statistics so a quiet but biased test bridge is not hidden.
    report['fixture_mip0_visibility_max_difference'] = float(np.abs(
        np.stack(fields['half_current'])-np.stack(fields['half_actual_mip0'])).max())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
