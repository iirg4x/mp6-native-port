"""Bounded direct-Hilbert versus LUT shader comparison; explicit invocation only.

The direct directory must be a saved, accepted output of compile_xegtao.py
from before the LUT change. No native game, user settings, or device install
is touched. Timing covers compute passes only, not complete AO pipeline cost.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--direct', type=Path, required=True)
    parser.add_argument('--lut', type=Path, default=ROOT/'build/ao-upstream-toolchain/xegtao')
    parser.add_argument('--backend', choices=('Vulkan', 'D3D12'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=40)
    args = parser.parse_args(argv)
    if not 4 <= args.samples <= 100:
        parser.error('--samples must be 4..100')
    os.environ['WGPU_BACKEND_TYPE'] = args.backend
    sys.path.insert(0, str(ROOT))
    from tests.integration import test_xegtao_gpu as gpu
    np = gpu.np
    if np is None:
        raise RuntimeError('optional GPU dependencies are unavailable')

    # Bind the comparison to hash-checked accepted artifacts, not whichever
    # cached WGSL happens to exist. Normal Renderer requires the LUT binding.
    manifests = {}
    for name, directory in (('direct', args.direct), ('lut', args.lut)):
        manifest = json.loads((directory/'manifest.json').read_text())
        for entry in manifest['entries']:
            digest = hashlib.sha256((directory/entry['wgsl']).read_bytes()).hexdigest()
            if digest != entry['wgslSha256']:
                raise RuntimeError(f'{name}: stale shader {entry["name"]}')
        manifests[name] = manifest
    direct_code = (args.direct/'CSGTAOHigh.wgsl').read_text()
    lut_code = (args.lut/'CSGTAOHigh.wgsl').read_text()
    if 'HilbertIndex' not in direct_code or 'HilbertIndex' in lut_code:
        raise RuntimeError('expected direct Hilbert arithmetic and LUT shader pair')
    with patch.object(gpu, 'MAIN_REQUIRED', gpu.MAIN_REQUIRED-{'g_srcHilbertLUTMain'}):
        direct = gpu.Renderer(args.direct, timing=True)
    lut = gpu.Renderer(args.lut, timing=True)
    original_pack = gpu._pack_constants

    def constants_at(noise_index):
        def pack(*a, **kw):
            data = bytearray(original_pack(*a, **kw))
            assert len(data) == 96
            struct.pack_into('<i', data, 92, noise_index)
            return bytes(data)
        return pack

    def identical(a, b):
        for field in ('decoded_visibility', 'packed_normals', 'raw_ao'):
            np.testing.assert_array_equal(getattr(a, field).view(np.uint32),
                                          getattr(b, field).view(np.uint32), err_msg=field)
        for index, (left, right) in enumerate(zip(a.depth_mips, b.depth_mips)):
            np.testing.assert_array_equal(left.view(np.uint32), right.view(np.uint32),
                                          err_msg=f'depth MIP {index}')

    cases = 0
    # Full Hilbert-domain crossing, odd-size edge lanes, and all temporal
    # phases are covered. Production still intentionally uses NoiseIndex=0.
    for width, height in ((129, 73), (640, 360)):
        scene, _ = gpu.feet_scene(width, height)
        for quality in ('Low', 'Medium', 'High', 'Ultra'):
            for phase in (0, 1, 63):
                with patch.object(gpu, '_pack_constants', constants_at(phase)):
                    identical(direct.render(scene, quality, 3, reuse_resources=True),
                              lut.render(scene, quality, 3, reuse_resources=True))
                cases += 1
    scene, _ = gpu.feet_scene(129, 73)
    for phase in range(64):
        with patch.object(gpu, '_pack_constants', constants_at(phase)):
            identical(direct.render(scene, 'High', 2, reuse_resources=True),
                      lut.render(scene, 'High', 2, reuse_resources=True))
        cases += 1
    for scene in (gpu.plane(129, 73)[0], np.zeros((73, 129), dtype=np.float32)):
        identical(direct.render(scene, 'High', 2, reuse_resources=True),
                  lut.render(scene, 'High', 2, reuse_resources=True))
        cases += 1
    print(f'{args.backend}: {cases} exact direct/LUT parity cases', flush=True)

    scene, _ = gpu.feet_scene(640, 360)
    for _ in range(12):
        direct.render(scene, 'High', 2, reuse_resources=True)
        lut.render(scene, 'High', 2, reuse_resources=True)
    samples = {'direct': [], 'lut': []}
    # Alternate order to limit monotonic load/clock drift. Readback and upload
    # remain outside timestamp intervals, so these are stage-only diagnostics.
    for index in range(args.samples):
        order = (('direct', direct), ('lut', lut))
        for name, renderer in order if index % 2 == 0 else reversed(order):
            result = renderer.render(scene, 'High', 2, reuse_resources=True)
            samples[name].append(result.timings_ms)

    def distribution(values):
        return {'median_ms': float(np.median(values)), 'p95_ms': float(np.quantile(values, .95))}

    timing = {}
    for name, rows in samples.items():
        timing[name] = {
            'main': distribution([row['CSGTAOHigh'] for row in rows]),
            'compute_pass_sum': distribution([sum(row.values()) for row in rows]),
        }
    report = {
        'backend': args.backend, 'bit_exact_parity_cases': cases,
        'policy': 'High/2/full, 640x360 timing', 'samples_per_variant': args.samples,
        'warmup_pairs': 12, 'timing': timing, 'raw_stage_samples_ms': samples,
        'scope': 'Compute-pass timestamps only; excludes CPU, upload, readback, '
                 'inter-pass overhead, depth preparation and final composite. '
                 'Separate devices on one adapter, alternating order, existing PC load. '
                 'Not complete AO timing or Android acceptance.',
        'shader_manifests': manifests,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(timing, indent=2), flush=True)
    print(f'Evidence: {args.output}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
