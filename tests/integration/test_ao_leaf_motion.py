"""Opt-in, world-aligned thin-leaf AO diagnostic; no GPU work on import.

Set MP6_RUN_AO_LEAF_GPU=1 and run directly. Measurements compare the actual
shipping kernel with pinned XeGTAO presets; they are not Android/FPS evidence.
"""
import json
import hashlib
import math
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]


def leaf_scene(np, width, height, shift, lift=4.):
    """Ray-intersect two fixed raised ellipses and their supporting plane."""
    normal = np.array([.05, .55, .833])
    normal /= np.linalg.norm(normal)
    across = np.cross(normal, [0., 1., 0.])
    across /= np.linalg.norm(across)
    along = np.cross(normal, across)
    origin = across * shift
    base = np.array([0., 0., -180.])
    y, x = np.mgrid[:height, :width]
    tan = math.tan(math.pi / 8)
    rays = np.stack((((x+.5)/width*2-1)*tan*width/height,
                     -((y+.5)/height*2-1)*tan, -np.ones_like(x)), axis=-1)
    denominator = rays @ normal
    ground = ((base-origin) @ normal) / denominator
    leaf_t = ((base + lift*normal-origin) @ normal) / denominator
    leaf_points = origin + rays * leaf_t[..., None]
    leaf_mask = np.zeros((height, width), bool)
    for side in (-1., 1.):
        center = base + across*(side*20.) + normal*lift
        angle = math.radians(side*20.)
        a = across*math.cos(angle) + along*math.sin(angle)
        b = -across*math.sin(angle) + along*math.cos(angle)
        local = leaf_points-center
        leaf_mask |= ((local@a)/23.)**2 + ((local@b)/9.)**2 <= 1.
    depth = np.where(leaf_mask & (leaf_t>0), np.minimum(ground, leaf_t), ground)
    gy, gx = np.mgrid[-35:36:1., -62:63:1.]
    world = base + gx[..., None]*across + gy[..., None]*along
    view = world-origin
    u = (view[..., 0]/(-view[..., 2])/(tan*width/height)+1)*width/2-.5
    v = (1-view[..., 1]/(-view[..., 2])/tan)*height/2-.5
    return depth, ground, (u, v), leaf_mask


def world_samples(np, visibility, ground_mask, coords):
    u, v = coords
    h, w = visibility.shape
    x, y = np.floor(u).astype(int), np.floor(v).astype(int)
    valid = (x>=3) & (y>=3) & (x<w-4) & (y<h-4)
    x, y = np.clip(x, 0, w-2), np.clip(y, 0, h-2)
    fx, fy = u-np.floor(u), v-np.floor(v)
    result = np.zeros(u.shape)
    for dx, dy in ((0,0), (1,0), (0,1), (1,1)):
        valid &= ground_mask[y+dy, x+dx]
        weight = (fx if dx else 1-fx)*(fy if dy else 1-fy)
        result += visibility[y+dy, x+dx]*weight
    return result, valid


class LeafMotion(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('MP6_RUN_AO_LEAF_GPU') == '1', 'explicit GPU opt-in required')
    def test_fixed_world_leaves_camera_translation(self):
        sys.path[:0] = [str(ROOT/'build/ao-gpu-test-deps'), str(ROOT/'tests/integration'), str(ROOT/'tests')]
        try:
            import numpy as np
            from PIL import Image, ImageDraw
            from test_ao_gpu import Renderer as Legacy
            from test_xegtao_gpu import Renderer as XeGTAO
        except ImportError as exc:
            self.skipTest(f'optional GPU dependencies unavailable: {exc}')
        legacy, xe = Legacy(), XeGTAO()
        variants = {'legacy': legacy}
        if os.environ.get('MP6_AO_LEAF_SAMPLING_SWEEP') == '1':
            from ao_shader_source import shader_source
            base_source = shader_source()
            for slices, steps in ((6, 2), (8, 2), (12, 2)):
                source = base_source.replace('slice < 3u', f'slice < {slices}u').replace(
                    'pi / 3.0', f'pi / {float(slices)}').replace('step < 4u', f'step < {steps}u')
                source = source.replace('let t = (f32(step) + 0.5) / 4.0;',
                    f'let t = (f32(step) + fract(f32(slice)*0.61803398875+0.5)) / {float(steps)};')
                variants[f'probe_{slices}x{steps}'] = Legacy(shader_source=source)
            for name, phase in (
                ('ign_angle', 'fract(52.9829189 * fract(dot(pixel.xy, vec2f(0.06711056, 0.00583715))))'),
                ('quad_angle', 'f32((u32(pixel.x) & 1u) * 2u + (u32(pixel.y) & 1u)) * 0.25'),
            ):
                source = base_source.replace('let angle = (f32(slice) + 0.5) * pi / 3.0;',
                    f'let angle = (f32(slice) + {phase}) * pi / 3.0;')
                variants[name] = Legacy(shader_source=source)
                if name == 'quad_angle':
                    for beta in (0.08, 0.0):
                        filtered_source = source.replace('f32(i * i) * 0.32', f'f32(i * i) * {beta}')
                        variants[f'quad_blur_{beta}'] = Legacy(shader_source=filtered_source)
            dense_source = base_source.replace('slice < 3u', 'slice < 32u').replace(
                'pi / 3.0', 'pi / 32.0').replace('step < 4u', 'step < 16u').replace(
                'let t = (f32(step) + 0.5) / 4.0;', 'let t = (f32(step) + 0.5) / 16.0;')
            variants['dense_reference'] = Legacy(shader_source=dense_source)
            for slices, steps in ((6,4), (8,4), (12,4), (3,8), (6,8)):
                source = base_source.replace('slice < 3u', f'slice < {slices}u').replace(
                    'pi / 3.0', f'pi / {float(slices)}').replace('step < 4u', f'step < {steps}u').replace(
                    'let t = (f32(step) + 0.5) / 4.0;', f'let t = (f32(step) + 0.5) / {float(steps)};')
                variants[f'fixed_{slices}x{steps}'] = Legacy(shader_source=source)
        upstream_presets = {'xegtao': ('High', 2)}
        if os.environ.get('MP6_AO_LEAF_XE_SWEEP') == '1':
            upstream_presets.update(xegtao_high3=('High',3), xegtao_ultra2=('Ultra',2),
                                    xegtao_ultra3=('Ultra',3))
        samples = {name: [] for name in (*variants, *upstream_presets)}
        half = None
        if os.environ.get('MP6_AO_LEAF_HALF') == '1':
            from test_xegtao_resolution import XeGTAOResolutionGPU, _linearize_canonical_depth
            half = XeGTAOResolutionGPU
            half.setUpClass()
            samples['xegtao_half_ultra3'] = []
        validity = []
        panels = []
        lift = float(os.environ.get('MP6_AO_LEAF_LIFT', '4'))
        for index, shift in enumerate(np.linspace(-.7, .7, 8)):
            depth, ground, coords, mask = leaf_scene(np, 640, 360, float(shift), lift)
            outputs = {name: renderer.render(depth, strength=.35, ao_scale=1.)[2][...,0]
                       for name, renderer in variants.items()}
            for name, (quality, passes) in upstream_presets.items():
                upstream = xe.render(depth, quality=quality, denoise_passes=passes).decoded_visibility
                outputs[name] = 1.-.35*(1.-upstream)
            if half is not None:
                # Exercise the production half adapter/reconstruction shaders.
                # The isolated harness crosses devices via readback and a
                # canonical->linear->canonical float32 round trip. This is a
                # quality diagnostic, not a native bit-parity or timing test.
                foliage = np.zeros((*depth.shape, 4), dtype=np.float32)
                neutral = np.full(((depth.shape[0]+1)//2, (depth.shape[1]+1)//2),
                                  255, dtype=np.uint32)
                canonical, _ = half._render_half(depth, neutral, foliage, .35,
                                                  (0,0,1,1), (0,0,1,1))
                low = xe.render(_linearize_canonical_depth(canonical), quality='Ultra',
                                denoise_passes=3).decoded_visibility
                final_ao = np.rint(low*255.).astype(np.uint32)
                _, composite = half._render_half(depth, final_ao, foliage, .35,
                                                  (0,0,1,1), (0,0,1,1))
                outputs['xegtao_half_ultra3'] = composite[...,3]
            for name, visibility in outputs.items():
                self.assertTrue(np.isfinite(visibility).all())
                aligned, valid = world_samples(np, visibility, ~mask, coords)
                samples[name].append(aligned)
                validity.append(valid)
                if index == 0:
                    # Contrast-amplified AO-only evidence, identical mapping.
                    gray = np.clip(1.-(1.-visibility)*3., 0., 1.)
                    rgb = np.repeat((gray[..., None]*255).astype('uint8'), 3, axis=2)
                    rgb[mask] = [95, 154, 52]
                    panel = Image.fromarray(rgb)
                    ImageDraw.Draw(panel).text((10, 8), name+' AO x3', fill=(0, 0, 0))
                    panels.append(panel)
        common = np.logical_and.reduce(validity)
        self.assertGreater(int(common.sum()), 1000)
        metrics = {'world_ground_samples': int(common.sum()), 'frames': 8,
                   'camera_shift_world': [-.7, .7], 'leaf_lift_world': lift,
                   'strength': .35, 'scope': 'synthetic quality only; not native/mobile performance'}
        metrics['backend'] = os.environ.get('WGPU_BACKEND_TYPE', 'default')
        if half is not None:
            metrics['half_bridge'] = ('Production half shaders with CPU float32 depth round trip; '
                                      'quality diagnostic only, not native bit parity or timing')
        metrics['legacy_shader_sha256'] = hashlib.sha256(
            (ROOT/'src/gx/ambient_occlusion_shader.hpp').read_bytes()).hexdigest()
        for name, values in samples.items():
            signal = 1.-np.stack(values)[:, common]
            # Smoke checks only: reject a disabled/white AO path, without
            # pretending these loose bounds constitute a motion-quality gate.
            self.assertGreater(float(signal.max()), .01, name)
            self.assertGreater(float(signal.mean()), .0001, name)
            temporal = np.std(signal, axis=0)
            steps = np.abs(np.diff(signal, axis=0))
            metrics[name] = {'mean_darkening': float(signal.mean()),
                             'p95_darkening': float(np.quantile(signal, .95)),
                             'temporal_std_mean': float(temporal.mean()),
                             'temporal_std_p95': float(np.quantile(temporal, .95)),
                             'temporal_step_p95': float(np.quantile(steps, .95)),
                             'temporal_step_max': float(steps.max()),
                             'temporal_std_relative': float(temporal.mean()/max(signal.mean(), 1e-8))}
            # Large coherent changes (moving outlines) and fine-grain shimmer
            # are distinct artifacts. Report both; never replace the raw metric.
            field = 1.-np.stack(values)
            region = common[2:-2, 2:-2].copy()
            smoothed = np.zeros_like(field[:,2:-2,2:-2])
            for dy in range(5):
                for dx in range(5):
                    region &= common[dy:dy+region.shape[0],dx:dx+region.shape[1]]
                    smoothed += field[:,dy:dy+region.shape[0],dx:dx+region.shape[1]]/25.
            metrics[name]['coherent_5x5_std_mean'] = float(np.std(smoothed[:,region],axis=0).mean())
            metrics[name]['coherent_5x5_step_max'] = float(np.abs(np.diff(smoothed[:,region],axis=0)).max())
            if 'dense_reference' in samples:
                reference = 1.-np.stack(samples['dense_reference'])[:, common]
                metrics[name]['dense_reference_rmse'] = float(np.sqrt(np.mean((signal-reference)**2)))
                metrics[name]['dense_reference_mean_ratio'] = float(signal.mean()/max(reference.mean(), 1e-8))
        scope = 'legacy-sweep' if os.environ.get('MP6_AO_LEAF_SAMPLING_SWEEP') == '1' else 'upstream-sweep'
        if half is not None:
            scope += '-half'
        output = ROOT/'build/ao-leaf-motion'/metrics['backend']/f'lift-{lift:g}'/scope
        output.mkdir(parents=True, exist_ok=True)
        canvas = Image.new('RGB', (640*len(panels), 360), 'white')
        for i, panel in enumerate(panels):
            canvas.paste(panel, (640*i, 0))
            panel.save(output/(list(samples)[i]+'.png'))
        canvas.save(output/'comparison.png')
        (output/'metrics.json').write_text(json.dumps(metrics, indent=2)+'\n')
        print(json.dumps(metrics, indent=2), flush=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
